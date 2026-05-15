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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();
        // Swallow mouse events the cursor is using on an ImGui widget so
        // the game doesn't also receive them (otherwise scrolling our
        // zoom slider would also scroll the game's camera). Anything not
        // hovered over our UI is forwarded normally — gameplay input
        // stays intact.
        if (io.WantCaptureMouse) {
            switch (msg) {
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
                case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                    return 0;
            }
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Avoid mucking with the game's cursor — the user keeps the OS
    // cursor at all times, ImGui just reads its position. Game keeps
    // full keyboard control; we only ever react to mouse wheel today.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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

    // Win32 backend so ImGui sees mouse position + wheel. We chain our
    // overlay_wndproc on top of whatever subclass exists (Steam Overlay
    // installs its own; we never block events from reaching the game).
    if (!ImGui_ImplWin32_Init(g_overlay.hwnd)) {
        logf("overlay: ImGui_ImplWin32_Init failed");
        return false;
    }
    g_overlay.orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_overlay.hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(overlay_wndproc)));
    if (!g_overlay.orig_wndproc) {
        logf("overlay: SetWindowLongPtrW(GWLP_WNDPROC) failed");
        return false;
    }
    logf("overlay: WndProc subclass installed");

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

// Calibration knobs — analytically derived from the mosaic geometry
// in research/maps/W1_Siagarta.mosaic.json combined with the game's
// world-unit-per-tile constant (256 m/tile, see fow_viewer.py):
//   px_per_meter = TILE_PX / METERS_PER_TILE = 1024 / 256 = 4
//   min_world_x  = grid_min_x * METERS_PER_TILE = -4 * 256 = -1024
//   full_x       = (world_x - (-1024)) * 4 = 4 * world_x + 4096
//   full_y_flip  = (1280 - world_y) * 4
//                = kFullMosaicPx - (4 * world_y + 6144)   (flip_y=true)
// Hot-reloadable from research/minimap_calibration.json so the user can
// fine-tune without rebuilding the DLL.
struct Calibration {
    float world_to_full_x_scale  = 4.0f;
    float world_to_full_y_scale  = 4.0f;
    float world_to_full_x_offset = 4096.0f;
    float world_to_full_y_offset = 6144.0f;
    bool  flip_y                 = true;
    // View zoom: 1.0 shows the full mosaic, 19.0 shows ~1/19 of it
    // centered on the player. Player-centered crop is clamped near the
    // world edges so the image stays in-bounds.
    float zoom                   = 1.0f;
};
Calibration g_calib;

const wchar_t* kCalibPath =
    L"D:\\farevermod\\research\\minimap_calibration.json";

bool calib_extract_double(const std::string& json, const char* key,
                          double& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    char* end = nullptr;
    double v = strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

bool calib_extract_bool(const std::string& json, const char* key, bool& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (json.compare(i, 4, "true") == 0)  { out = true;  return true; }
    if (json.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

// Hot-reload poll. Cheap: only re-reads when file mtime changes. Called
// once per frame.
void calib_maybe_reload() {
    static FILETIME last_write{};
    static bool     first_check = true;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(kCalibPath, GetFileExInfoStandard, &attr)) {
        return;  // file missing — keep current (defaults)
    }
    if (!first_check &&
        attr.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime) {
        return;  // unchanged
    }
    last_write  = attr.ftLastWriteTime;
    first_check = false;

    std::ifstream f(kCalibPath);
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    Calibration c = g_calib;  // start from current
    double v;
    if (calib_extract_double(text, "scale_x",  v)) c.world_to_full_x_scale  = static_cast<float>(v);
    if (calib_extract_double(text, "scale_y",  v)) c.world_to_full_y_scale  = static_cast<float>(v);
    if (calib_extract_double(text, "offset_x", v)) c.world_to_full_x_offset = static_cast<float>(v);
    if (calib_extract_double(text, "offset_y", v)) c.world_to_full_y_offset = static_cast<float>(v);
    if (calib_extract_double(text, "zoom",     v)) c.zoom                   = static_cast<float>(v);
    calib_extract_bool(text, "flip_y", c.flip_y);
    if (c.zoom < 1.0f)   c.zoom = 1.0f;
    if (c.zoom > 64.0f)  c.zoom = 64.0f;
    g_calib = c;
    logf("overlay: calibration reloaded "
         "(scale=%.3f,%.3f offset=%.1f,%.1f flip_y=%d zoom=%.2f)",
         c.world_to_full_x_scale,  c.world_to_full_y_scale,
         c.world_to_full_x_offset, c.world_to_full_y_offset,
         static_cast<int>(c.flip_y), c.zoom);
}

// Mosaic full is 11264x11264 (preview is pre-downsampled to 4096x4096
// inside the PNG file). The on-screen compass diameter cycles through
// a few fixed steps via the size button on the bezel.
constexpr float kFullMosaicPx = 11264.0f;
constexpr float kCompassSizes[3] = { 256.0f, 384.0f, 512.0f };
int g_compass_size_idx = 2;   // start at largest

float compass_size_px() { return kCompassSizes[g_compass_size_idx]; }

ImVec2 world_to_full(double world_x, double world_y) {
    float full_x = static_cast<float>(world_x) * g_calib.world_to_full_x_scale
                   + g_calib.world_to_full_x_offset;
    float full_y = static_cast<float>(world_y) * g_calib.world_to_full_y_scale
                   + g_calib.world_to_full_y_offset;
    if (g_calib.flip_y) full_y = kFullMosaicPx - full_y;
    return ImVec2(full_x, full_y);
}

struct ViewUV {
    ImVec2 uv0;        // top-left UV of the image we render
    ImVec2 uv1;        // bottom-right UV
};

// Compute the player-centered crop of the mosaic at current zoom, with
// edge clamping so the UV rectangle stays within [0, 1].
ViewUV compute_view_uv(double world_x, double world_y, bool have_player) {
    float vsize = 1.0f / g_calib.zoom;
    if (vsize >= 1.0f || !have_player) {
        return ViewUV{ImVec2(0, 0), ImVec2(1, 1)};
    }
    ImVec2 full = world_to_full(world_x, world_y);
    float cu = full.x / kFullMosaicPx;
    float cv = full.y / kFullMosaicPx;
    float half = vsize * 0.5f;
    if (cu < half)        cu = half;
    if (cu > 1.0f - half) cu = 1.0f - half;
    if (cv < half)        cv = half;
    if (cv > 1.0f - half) cv = 1.0f - half;
    return ViewUV{
        ImVec2(cu - half, cv - half),
        ImVec2(cu + half, cv + half),
    };
}

// Player position on the rendered compass image (size px square) given
// the view crop.
ImVec2 player_to_screen(double world_x, double world_y, const ViewUV& view,
                        float size_px) {
    ImVec2 full = world_to_full(world_x, world_y);
    float u = full.x / kFullMosaicPx;
    float v = full.y / kFullMosaicPx;
    float sx = (u - view.uv0.x) / (view.uv1.x - view.uv0.x) * size_px;
    float sy = (v - view.uv0.y) / (view.uv1.y - view.uv0.y) * size_px;
    return ImVec2(sx, sy);
}

constexpr float kZoomMin = 10.0f;
constexpr float kZoomMax = 20.0f;
constexpr float kZoomStep = 1.0f;

// WoW-style palette: deep brass/stone with a gold bezel.
constexpr ImU32 kColBezel       = IM_COL32(212, 175,  55, 240);  // #d4af37
constexpr ImU32 kColBezelShadow = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kColBtnFill     = IM_COL32( 32,  24,  12, 235);
constexpr ImU32 kColBtnHover    = IM_COL32( 70,  52,  24, 245);
constexpr ImU32 kColBtnActive   = IM_COL32( 18,  12,   6, 255);
constexpr ImU32 kColIcon        = IM_COL32(255, 220, 130, 255);
constexpr ImU32 kColNorth       = IM_COL32(255,  80,  80, 230);
constexpr ImU32 kColPlayer      = IM_COL32(255, 200,  50, 255);

struct BezelButton {
    ImVec2 center;
    float  radius;
    bool   clicked;
    bool   hovered;
    bool   active;
};

// Hit-test only. Visual drawing is split out so we can declare all
// hit areas in priority order (smaller first) and then draw the
// compass background BENEATH and the button visuals ON TOP.
BezelButton bezel_hit(const char* id, ImVec2 center, float bezel_r,
                      float angle_rad, float btn_r) {
    BezelButton b{};
    b.center = ImVec2(center.x + cosf(angle_rad) * bezel_r,
                      center.y + sinf(angle_rad) * bezel_r);
    b.radius = btn_r;
    ImGui::SetCursorScreenPos(ImVec2(b.center.x - btn_r, b.center.y - btn_r));
    ImGui::SetNextItemAllowOverlap();
    b.clicked = ImGui::InvisibleButton(id, ImVec2(btn_r * 2, btn_r * 2));
    b.hovered = ImGui::IsItemHovered();
    b.active  = ImGui::IsItemActive();
    return b;
}

void bezel_draw_circle_base(ImDrawList* dl, const BezelButton& b) {
    ImU32 fill = b.active ? kColBtnActive
               : b.hovered ? kColBtnHover : kColBtnFill;
    dl->AddCircleFilled(b.center, b.radius, fill, 32);
    dl->AddCircle(b.center, b.radius, kColBezel, 32, 2.0f);
    dl->AddCircle(b.center, b.radius - 1.5f, kColBezelShadow, 32, 1.0f);
}

void bezel_draw_plus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_circle_base(dl, b);
    const float arm = b.radius * 0.45f;
    dl->AddLine(ImVec2(b.center.x - arm, b.center.y),
                ImVec2(b.center.x + arm, b.center.y), kColIcon, 2.5f);
    dl->AddLine(ImVec2(b.center.x, b.center.y - arm),
                ImVec2(b.center.x, b.center.y + arm), kColIcon, 2.5f);
}

void bezel_draw_minus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_circle_base(dl, b);
    const float arm = b.radius * 0.45f;
    dl->AddLine(ImVec2(b.center.x - arm, b.center.y),
                ImVec2(b.center.x + arm, b.center.y), kColIcon, 2.5f);
}

// Pushpin glyph: round head + sloped needle, body and needle in gold.
void bezel_draw_pin(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_circle_base(dl, b);
    ImVec2 head(b.center.x - 1.5f, b.center.y - 3.5f);
    ImVec2 tip (b.center.x + 4.5f, b.center.y + 5.5f);
    dl->AddLine(head, tip, kColIcon, 2.0f);
    dl->AddCircleFilled(head, 3.0f, kColIcon, 12);
    dl->AddCircle(head, 3.0f, kColBezelShadow, 12, 1.0f);
}

// Resize glyph: a small square that grows in size by tier.
void bezel_draw_size(ImDrawList* dl, const BezelButton& b, int size_idx) {
    bezel_draw_circle_base(dl, b);
    const float steps[3] = { 4.0f, 6.0f, 8.0f };
    float h = steps[size_idx] * 0.5f;
    dl->AddRect(ImVec2(b.center.x - h, b.center.y - h),
                ImVec2(b.center.x + h, b.center.y + h),
                kColIcon, 1.0f, 0, 1.8f);
}

void render_compass(const LivePosition& lp) {
    constexpr float kPi = 3.14159265358979323846f;
    const float size = compass_size_px();
    const float r    = size * 0.5f;
    const float btn_r = (size <= 256.0f) ? 11.0f
                       : (size <= 384.0f) ? 13.0f : 14.0f;

    ImVec2 p_min = ImGui::GetCursorScreenPos();
    ImVec2 p_max(p_min.x + size, p_min.y + size);
    ImVec2 center(p_min.x + r, p_min.y + r);

    // === HIT-TEST: ImGui's rule is "last overlapping item wins". So we
    // declare the big body first (marked as AllowOverlap so later items
    // can take hover from it), then the four small bezel buttons last.
    ImGui::SetCursorScreenPos(p_min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##compass_body", ImVec2(size, size));

    BezelButton pin   = bezel_hit("##pin",        center, r, -kPi * 0.32f, btn_r);
    BezelButton sizeb = bezel_hit("##size_cycle", center, r, -kPi * 0.20f, btn_r);
    BezelButton plus  = bezel_hit("##zoom_plus",  center, r,  kPi * 0.20f, btn_r);
    BezelButton minus = bezel_hit("##zoom_minus", center, r,  kPi * 0.32f, btn_r);

    // === DRAWING (DrawList is z-ordered by call sequence) ===
    auto* dl = ImGui::GetWindowDrawList();
    ViewUV view = compute_view_uv(lp.x, lp.y, lp.valid);

    if (g_overlay.mosaic.resource) {
        dl->AddImageRounded(
            static_cast<ImTextureID>(g_overlay.mosaic.srv_gpu.ptr),
            p_min, p_max, view.uv0, view.uv1, IM_COL32_WHITE, r);
    } else {
        dl->AddCircleFilled(center, r, IM_COL32(20, 22, 30, 255), 64);
    }
    dl->AddCircle(center, r,        kColBezel,       96, 3.0f);
    dl->AddCircle(center, r - 2.0f, kColBezelShadow, 96, 1.0f);

    // North tick on the bezel.
    dl->AddLine(ImVec2(center.x, p_min.y),
                ImVec2(center.x, p_min.y + 10.0f),
                kColNorth, 2.5f);

    if (lp.valid) {
        ImVec2 dot_local = player_to_screen(lp.x, lp.y, view, size);
        ImVec2 dot(p_min.x + dot_local.x, p_min.y + dot_local.y);
        float dr = 5.0f;
        float c = cosf(static_cast<float>(lp.rot_z));
        float s = sinf(static_cast<float>(lp.rot_z));
        dl->AddLine(dot, ImVec2(dot.x + c * 14.0f, dot.y + s * 14.0f),
                    kColPlayer, 2.0f);
        dl->AddCircleFilled(dot, dr,           kColPlayer, 16);
        dl->AddCircle(dot, dr + 1.0f, kColBezelShadow, 16, 1.5f);
    }

    // Buttons on top.
    bezel_draw_pin(dl,   pin);
    bezel_draw_size(dl,  sizeb, g_compass_size_idx);
    bezel_draw_plus(dl,  plus);
    bezel_draw_minus(dl, minus);

    // === ACTIONS ===
    if (plus.clicked)  {
        g_calib.zoom += kZoomStep;
        if (g_calib.zoom > kZoomMax) g_calib.zoom = kZoomMax;
    }
    if (minus.clicked) {
        g_calib.zoom -= kZoomStep;
        if (g_calib.zoom < kZoomMin) g_calib.zoom = kZoomMin;
    }
    if (sizeb.clicked) {
        g_compass_size_idx = (g_compass_size_idx + 1) % 3;
    }
    // Drag: while the pin button is held, move the window with mouse.
    if (pin.active) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImGui::SetWindowPos(ImVec2(wp.x + delta.x, wp.y + delta.y));
        }
    }
}

void render_imgui_window() {
    calib_maybe_reload();

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar    |
        ImGuiWindowFlags_NoResize      |
        ImGuiWindowFlags_NoMove        |  // only the pin button moves us
        ImGuiWindowFlags_NoScrollbar   |
        ImGuiWindowFlags_NoBackground  |
        ImGuiWindowFlags_NoCollapse    |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("minimap", nullptr, wflags);

    LivePosition lp = live_position_get();
    render_compass(lp);

    ImGui::End();
    ImGui::PopStyleVar();
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
    ImGui_ImplWin32_NewFrame();
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
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

}  // namespace farevermod
