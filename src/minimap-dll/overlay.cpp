// ImGui-DX12 overlay rendered inside the game's Present call.
//
// Lifecycle:
//   First successful Present (after ExecuteCommandLists has captured a
//   queue) -> init(): create our own descriptor heaps, RTVs for each
//   back buffer, a per-frame command allocator + a shared command list,
//   then ImGui_ImplWin32_Init + ImGui_ImplDX12_Init.
//
//   Each Present -> render(): build an ImGui frame, record draw calls
//   into our command list against the *current* back buffer, submit
//   onto the game's queue. We never touch the game's command lists.
//
//   ResizeBuffers -> drop back-buffer-sized resources, then recreate
//   after the game has finished its own resize.

#include "overlay.h"
#include "log.h"
#include "live_position.h"
#include "textures.h"

#include <windows.h>
#include <atomic>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

// Declared in imgui_impl_win32.cpp so we can chain WndProc.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace farevermod {
namespace {

// Conservative cap; if the swap chain has more buffers we re-init.
constexpr UINT kMaxBackBuffers = 8;

struct FrameContext {
    ID3D12CommandAllocator*    allocator    = nullptr;
    ID3D12GraphicsCommandList* command_list = nullptr;
    ID3D12Resource*            back_buffer  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    UINT64                  fence_value = 0;  // queue Signal() after our submit
};

struct Overlay {
    bool initialized = false;
    bool init_failed = false;

    // Identity of the swap chain we own. Other swap chains (e.g.,
    // Steam Overlay's, FRAPS, etc.) trip our Present hook but must not
    // be rendered onto — their back buffers, formats, even queues are
    // different and using ours against them is a fast path to GPU AV.
    IDXGISwapChain3*            owned_swap_chain = nullptr;

    ID3D12Device*               device           = nullptr;
    ID3D12DescriptorHeap*       rtv_heap         = nullptr;
    ID3D12DescriptorHeap*       srv_heap         = nullptr;
    UINT                        rtv_descriptor_size = 0;
    UINT                        back_buffer_count = 0;
    std::vector<FrameContext>   frames;

    // Per-frame fence so we don't Reset() an allocator the GPU is still
    // using. Without this an overlay can stay alive for thousands of
    // frames before tripping a race when GPU latency catches up — the
    // crash then manifests as an AV inside the game's next Present.
    ID3D12Fence*                fence        = nullptr;
    UINT64                      next_fence_value = 0;
    HANDLE                      fence_event  = nullptr;

    HWND                        hwnd     = nullptr;
    WNDPROC                     orig_wndproc = nullptr;
    DXGI_FORMAT                 rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;

    LoadedTexture               mosaic{};
};

Overlay g_overlay;
std::atomic<bool> g_in_render{false};

// --- WndProc chain ----------------------------------------------------------

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
            // ImGui consumed the message (e.g., text input in a focused box).
            // Still chain so the game gets a chance — keep the overlay
            // strictly non-interfering with gameplay input.
        }
    }
    return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
}

// --- Helpers ----------------------------------------------------------------

void release_frame_targets() {
    for (auto& f : g_overlay.frames) {
        if (f.back_buffer) { f.back_buffer->Release(); f.back_buffer = nullptr; }
    }
}

void release_all() {
    release_frame_targets();
    for (auto& f : g_overlay.frames) {
        if (f.command_list) { f.command_list->Release(); f.command_list = nullptr; }
        if (f.allocator)    { f.allocator->Release();    f.allocator    = nullptr; }
    }
    g_overlay.frames.clear();
    if (g_overlay.rtv_heap) {
        g_overlay.rtv_heap->Release();
        g_overlay.rtv_heap = nullptr;
    }
    if (g_overlay.srv_heap) {
        g_overlay.srv_heap->Release();
        g_overlay.srv_heap = nullptr;
    }
    if (g_overlay.fence) {
        g_overlay.fence->Release();
        g_overlay.fence = nullptr;
    }
    if (g_overlay.fence_event) {
        CloseHandle(g_overlay.fence_event);
        g_overlay.fence_event = nullptr;
    }
    if (g_overlay.device) {
        g_overlay.device->Release();
        g_overlay.device = nullptr;
    }
}

void wait_for_frame(FrameContext& frame) {
    if (frame.fence_value == 0) return;  // never submitted
    if (g_overlay.fence->GetCompletedValue() >= frame.fence_value) return;
    g_overlay.fence->SetEventOnCompletion(frame.fence_value,
                                          g_overlay.fence_event);
    WaitForSingleObject(g_overlay.fence_event, INFINITE);
}

bool create_back_buffer_targets(IDXGISwapChain3* swap_chain) {
    auto rtv_cpu_start = g_overlay.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(swap_chain->GetBuffer(i, __uuidof(ID3D12Resource),
                                         reinterpret_cast<void**>(&f.back_buffer)))) {
            logf("overlay: GetBuffer(%u) failed", i);
            return false;
        }
        f.rtv_handle.ptr = rtv_cpu_start.ptr + i * g_overlay.rtv_descriptor_size;
        g_overlay.device->CreateRenderTargetView(f.back_buffer, nullptr,
                                                  f.rtv_handle);
    }
    return true;
}

bool overlay_init(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    g_overlay.owned_swap_chain = swap_chain;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(&g_overlay.device)))) {
        logf("overlay: GetDevice failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) {
        logf("overlay: swap_chain->GetDesc failed");
        return false;
    }
    g_overlay.hwnd            = desc.OutputWindow;
    g_overlay.back_buffer_count = desc.BufferCount;
    g_overlay.rt_format       = desc.BufferDesc.Format;
    if (g_overlay.back_buffer_count == 0 ||
        g_overlay.back_buffer_count > kMaxBackBuffers) {
        logf("overlay: unexpected back-buffer count %u",
             g_overlay.back_buffer_count);
        return false;
    }
    g_overlay.frames.assign(g_overlay.back_buffer_count, FrameContext{});

    // RTV heap — CPU-visible, one slot per back buffer.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = g_overlay.back_buffer_count;
    rtv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &rtv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.rtv_heap)))) {
        logf("overlay: CreateDescriptorHeap(RTV) failed");
        return false;
    }
    g_overlay.rtv_descriptor_size = g_overlay.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV heap — shader-visible. Reserve slot 0 for ImGui's font atlas,
    // remaining slots for our map textures (added in M3.3).
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 64;
    srv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &srv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.srv_heap)))) {
        logf("overlay: CreateDescriptorHeap(SRV) failed");
        return false;
    }

    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(g_overlay.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&f.allocator)))) {
            logf("overlay: CreateCommandAllocator(%u) failed", i);
            return false;
        }
        if (FAILED(g_overlay.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator, nullptr,
                __uuidof(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&f.command_list)))) {
            logf("overlay: CreateCommandList(%u) failed", i);
            return false;
        }
        f.command_list->Close();
    }

    if (FAILED(g_overlay.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&g_overlay.fence)))) {
        logf("overlay: CreateFence failed");
        return false;
    }
    g_overlay.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_overlay.fence_event) {
        logf("overlay: CreateEventW failed");
        return false;
    }
    g_overlay.next_fence_value = 0;

    if (!create_back_buffer_targets(swap_chain)) {
        return false;
    }

    // We deliberately do NOT use the ImGui Win32 backend yet — no input
    // capture, the overlay is read-only. WndProc subclass would also
    // need stacking with Steam Overlay, which has shipping risks. v0.1
    // is display-only.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Pick up the real swap chain size for the display so ImGui's
    // clipping & coordinate math matches.
    io.DisplaySize = ImVec2(
        static_cast<float>(desc.BufferDesc.Width),
        static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;   // disable imgui.ini IO
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();

    // Force font atlas build at init time. The lazy build inside the
    // first ImGui::NewFrame call uses stb_truetype, whose glyph
    // rasterizer has a sizeable stack frame and reliably kills the
    // game on HashLink's main thread (manifests as AV inside
    // h3d.impl.DX12Driver.present or .initViewport). Doing the build
    // here works because at this point our hook is on the main thread
    // and the call chain above us is shallower.
    if (!io.Fonts->Build()) {
        logf("overlay: io.Fonts->Build() failed");
        return false;
    }
    logf("overlay: font atlas built");

    auto srv_cpu = g_overlay.srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto srv_gpu = g_overlay.srv_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(g_overlay.device,
                             static_cast<int>(g_overlay.back_buffer_count),
                             g_overlay.rt_format, g_overlay.srv_heap,
                             srv_cpu, srv_gpu)) {
        logf("overlay: ImGui_ImplDX12_Init failed");
        return false;
    }

    // Load the world mosaic into SRV slot 1 (slot 0 is ImGui's font in
    // legacy InitLegacySingleDescriptorMode). The 4096² preview PNG is
    // ~64 MB of GPU memory; the full 11264² mosaic is too big and is
    // saved for tile streaming later.
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 1,
            L"D:\\farevermod\\research\\maps\\W1_Siagarta.preview.png",
            &g_overlay.mosaic)) {
        logf("overlay: mosaic load failed; running without map background");
    }

    logf("overlay: DX12+ImGui init OK (hwnd=%p, buffers=%u, fmt=%d, queue=%p)",
         g_overlay.hwnd, g_overlay.back_buffer_count,
         static_cast<int>(g_overlay.rt_format), static_cast<void*>(queue));
    return true;
}

// Calibration knobs — tune these live, refresh via re-inject.
// Initial guess: 1 world unit ≈ 1 mosaic-full pixel, with the mosaic
// rooted at world (grid_min_x * tile_px, grid_min_y * tile_px) =
// (-4096, -6144). Heaps Y points down in world; image Y points down on
// screen, so we keep flip_y off initially.
struct Calibration {
    float world_to_full_x_scale  = 1.0f;
    float world_to_full_y_scale  = 1.0f;
    float world_to_full_x_offset = 4096.0f;
    float world_to_full_y_offset = 6144.0f;
    bool  flip_y                 = false;
};
Calibration g_calib;

// Image we render is 512x512, mosaic full is 11264x11264 (preview is
// pre-downsampled to 4096x4096 inside the PNG file).
constexpr float kImageSizePx = 512.0f;
constexpr float kFullMosaicPx = 11264.0f;

ImVec2 world_to_image(double world_x, double world_y) {
    float full_x = static_cast<float>(world_x) * g_calib.world_to_full_x_scale
                   + g_calib.world_to_full_x_offset;
    float full_y = static_cast<float>(world_y) * g_calib.world_to_full_y_scale
                   + g_calib.world_to_full_y_offset;
    if (g_calib.flip_y) full_y = kFullMosaicPx - full_y;
    float u = full_x / kFullMosaicPx;
    float v = full_y / kFullMosaicPx;
    return ImVec2(u * kImageSizePx, v * kImageSizePx);
}

void render_imgui_window() {
    ImGui::SetNextWindowSize(ImVec2(560, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("minimap v0.1");

    LivePosition lp = live_position_get();
    if (lp.valid) {
        ImGui::Text("world: %.1f, %.1f, %.1f", lp.x, lp.y, lp.z);
        ImGui::Text("yaw:   %.3f rad", lp.rot_z);
    } else {
        ImGui::TextDisabled("no live position yet — run find_hero.py loop");
    }
    ImGui::Separator();

    if (g_overlay.mosaic.resource) {
        ImVec2 image_size(kImageSizePx, kImageSizePx);
        ImGui::Image(static_cast<ImTextureID>(g_overlay.mosaic.srv_gpu.ptr),
                     image_size);
        ImVec2 image_min = ImGui::GetItemRectMin();

        if (lp.valid) {
            ImVec2 dot = world_to_image(lp.x, lp.y);
            ImVec2 abs_dot(image_min.x + dot.x, image_min.y + dot.y);
            auto* dl = ImGui::GetWindowDrawList();
            float r = 5.0f;
            dl->AddCircleFilled(abs_dot, r, IM_COL32(255, 200, 50, 255), 16);
            // Yaw indicator (12px line in heading direction).
            float c = cosf(static_cast<float>(lp.rot_z));
            float s = sinf(static_cast<float>(lp.rot_z));
            ImVec2 tip(abs_dot.x + c * 14.0f, abs_dot.y + s * 14.0f);
            dl->AddLine(abs_dot, tip, IM_COL32(255, 200, 50, 255), 2.0f);
            dl->AddCircle(abs_dot, r + 1.0f, IM_COL32(0, 0, 0, 200), 16, 1.5f);
        }
    } else {
        ImGui::TextDisabled("mosaic not loaded");
    }

    if (ImGui::CollapsingHeader("calibration")) {
        ImGui::DragFloat("scale_x",  &g_calib.world_to_full_x_scale,  0.001f);
        ImGui::DragFloat("scale_y",  &g_calib.world_to_full_y_scale,  0.001f);
        ImGui::DragFloat("offset_x", &g_calib.world_to_full_x_offset, 1.0f);
        ImGui::DragFloat("offset_y", &g_calib.world_to_full_y_offset, 1.0f);
        ImGui::Checkbox("flip_y",   &g_calib.flip_y);
    }

    ImGui::Text("Build: " __DATE__ " " __TIME__);
    ImGui::End();
}

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    UINT idx = swap_chain->GetCurrentBackBufferIndex();
    if (idx >= g_overlay.frames.size()) return;
    auto& frame = g_overlay.frames[idx];
    if (!frame.allocator || !frame.back_buffer) return;

    // Critical: must finish on the GPU before we Reset() this slot's
    // allocator. With back_buffer_count=2 the GPU is typically still
    // working on this allocator from one or two frames ago.
    wait_for_frame(frame);

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    render_imgui_window();
    ImGui::Render();

    frame.allocator->Reset();
    frame.command_list->Reset(frame.allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = frame.back_buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->OMSetRenderTargets(1, &frame.rtv_handle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {g_overlay.srv_heap};
    frame.command_list->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frame.command_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->Close();
    ID3D12CommandList* lists[] = {frame.command_list};
    queue->ExecuteCommandLists(1, lists);

    g_overlay.next_fence_value++;
    queue->Signal(g_overlay.fence, g_overlay.next_fence_value);
    frame.fence_value = g_overlay.next_fence_value;
}

}  // namespace

// --- Public hooks-side API --------------------------------------------------

void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* queue) {
    // First-sighting diagnostics so we can see Steam-Overlay /
    // secondary swap chains in the log.
    {
        static constexpr int kMaxLogged = 4;
        static void* logged[kMaxLogged] = {};
        static int logged_count = 0;
        bool seen = false;
        for (int i = 0; i < logged_count; ++i) {
            if (logged[i] == swap_chain) { seen = true; break; }
        }
        if (!seen && logged_count < kMaxLogged) {
            logged[logged_count++] = swap_chain;
            logf("overlay: new swap chain %p (queue=%p)",
                 static_cast<void*>(swap_chain), static_cast<void*>(queue));
        }
    }

    if (g_overlay.init_failed) return;
    if (!queue) return;  // ExecuteCommandLists hasn't run yet

    // Re-entrancy guard: ImGui-DX12 internally may trigger calls that
    // re-enter Present (rare but seen in flip-model games).
    bool expected = false;
    if (!g_in_render.compare_exchange_strong(expected, true)) return;
    struct Scope { ~Scope() { g_in_render.store(false); } } scope;

    // Diagnostic stage 2: minimal command list with barrier-only
    // round-trip on the back buffer, NO ImGui draw. Confirms whether
    // the issue is the state transitions themselves.
    static int frame_count = 0;
    if ((++frame_count) % 3600 == 1) {
        logf("overlay: frame %d", frame_count);
    }

    if (!g_overlay.initialized) {
        if (!overlay_init(swap_chain, queue)) {
            g_overlay.init_failed = true;
            release_all();
            return;
        }
        g_overlay.initialized = true;
    }
    // After init we ONLY render to our captured swap chain; ignore any
    // other Present calls (Steam Overlay, in-process tools, secondary
    // viewports). Drawing our resources against an unrelated back
    // buffer is the classic cause of GPU-side AV-in-Present.
    if (swap_chain != g_overlay.owned_swap_chain) return;
    overlay_render(swap_chain, queue);
}

void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT /*width*/, UINT /*height*/) {
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    logf("overlay: resize -> %u buffers", buffer_count);
    // Drain any in-flight GPU work before yanking the back buffers.
    for (auto& f : g_overlay.frames) {
        wait_for_frame(f);
        f.fence_value = 0;
    }
    release_frame_targets();
    if (buffer_count != 0 && buffer_count != g_overlay.back_buffer_count) {
        // Buffer count changed; redo the RTV heap on next after_resize.
        g_overlay.back_buffer_count = buffer_count;
        g_overlay.frames.resize(buffer_count);
    }
}

void overlay_after_resize(IDXGISwapChain3* swap_chain) {
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    if (!create_back_buffer_targets(swap_chain)) {
        logf("overlay: re-creating RTVs after resize failed");
        g_overlay.init_failed = true;
    }
}

void overlay_shutdown() {
    if (!g_overlay.initialized && !g_overlay.init_failed) return;

    if (g_overlay.orig_wndproc && g_overlay.hwnd) {
        SetWindowLongPtrW(g_overlay.hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_overlay.orig_wndproc));
        g_overlay.orig_wndproc = nullptr;
    }
    if (g_overlay.initialized) {
        // Drain GPU before destroying anything it might still reference.
        for (auto& f : g_overlay.frames) wait_for_frame(f);
        release_texture(&g_overlay.mosaic);
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

}  // namespace farevermod
