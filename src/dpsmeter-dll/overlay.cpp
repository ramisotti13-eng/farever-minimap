// ImGui-DX12 overlay for the DPS meter. The DX12 init / fence / WndProc
// machinery here is the same pattern as minimap-dll/overlay.cpp; see
// those comments for the deep rationale. The dpsmeter-specific bits
// live in render_imgui_window() — one table, no textures.

#include "overlay.h"
#include "log.h"
#include "aggregator.h"
#include "damage_scan.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dpsmeter {
namespace {

constexpr UINT kMaxBackBuffers = 8;

struct FrameContext {
    ID3D12CommandAllocator*     allocator    = nullptr;
    ID3D12GraphicsCommandList*  command_list = nullptr;
    ID3D12Resource*             back_buffer  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    UINT64                      fence_value  = 0;
};

struct Overlay {
    bool initialized = false;
    bool init_failed = false;

    IDXGISwapChain3*            owned_swap_chain = nullptr;
    ID3D12Device*               device           = nullptr;
    ID3D12DescriptorHeap*       rtv_heap         = nullptr;
    ID3D12DescriptorHeap*       srv_heap         = nullptr;
    UINT                        rtv_descriptor_size = 0;
    UINT                        back_buffer_count   = 0;
    std::vector<FrameContext>   frames;

    ID3D12Fence*                fence            = nullptr;
    UINT64                      next_fence_value = 0;
    HANDLE                      fence_event      = nullptr;

    HWND                        hwnd         = nullptr;
    WNDPROC                     orig_wndproc = nullptr;
    DXGI_FORMAT                 rt_format    = DXGI_FORMAT_R8G8B8A8_UNORM;
};

Overlay           g_overlay;
std::atomic<bool> g_in_render{false};

// Panic switch — F8 toggles, repeated fence stalls auto-disable.
std::atomic<bool> g_overlay_enabled{true};
constexpr int kFenceTimeoutMs        = 50;
constexpr int kAutoDisableSlowFrames = 30;
int g_consecutive_slow_frames = 0;

// WoW-bezel palette, matching minimap's UI vocabulary.
constexpr ImU32 kColBezel       = IM_COL32(212, 175,  55, 240);
constexpr ImU32 kColBezelShadow = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kColBtnFill     = IM_COL32( 32,  24,  12, 235);
constexpr ImU32 kColText        = IM_COL32(255, 230, 180, 255);
constexpr ImU32 kColCrit        = IM_COL32(255, 200,  80, 255);
constexpr ImU32 kColHeader      = IM_COL32(255, 220, 130, 255);

// --- WndProc chain --------------------------------------------------

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_F8 && (lp & (1u << 30)) == 0) {
        bool now_enabled = !g_overlay_enabled.load();
        g_overlay_enabled.store(now_enabled);
        g_consecutive_slow_frames = 0;
        logf("overlay: F8 toggle -> %s",
             now_enabled ? "ENABLED" : "DISABLED");
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_F9 && (lp & (1u << 30)) == 0) {
        aggregator_reset();
        logf("overlay: F9 -> manual fight reset");
        return 0;
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse) {
            switch (msg) {
                case WM_MOUSEMOVE:
                case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                    return 0;
            }
        }
    }
    return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
}

// --- DX12 plumbing --------------------------------------------------

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
    if (g_overlay.rtv_heap)  { g_overlay.rtv_heap->Release();  g_overlay.rtv_heap  = nullptr; }
    if (g_overlay.srv_heap)  { g_overlay.srv_heap->Release();  g_overlay.srv_heap  = nullptr; }
    if (g_overlay.fence)     { g_overlay.fence->Release();     g_overlay.fence     = nullptr; }
    if (g_overlay.fence_event) {
        CloseHandle(g_overlay.fence_event);
        g_overlay.fence_event = nullptr;
    }
    if (g_overlay.device) { g_overlay.device->Release(); g_overlay.device = nullptr; }
}

bool wait_for_frame(FrameContext& frame, DWORD timeout_ms) {
    if (frame.fence_value == 0) return true;
    if (g_overlay.fence->GetCompletedValue() >= frame.fence_value) return true;
    g_overlay.fence->SetEventOnCompletion(frame.fence_value,
                                          g_overlay.fence_event);
    return WaitForSingleObject(g_overlay.fence_event, timeout_ms) ==
           WAIT_OBJECT_0;
}

bool create_back_buffer_targets(IDXGISwapChain3* swap_chain) {
    auto rtv_cpu_start =
        g_overlay.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(swap_chain->GetBuffer(
                i, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&f.back_buffer)))) {
            logf("overlay: GetBuffer(%u) failed", i);
            return false;
        }
        f.rtv_handle.ptr =
            rtv_cpu_start.ptr + i * g_overlay.rtv_descriptor_size;
        g_overlay.device->CreateRenderTargetView(f.back_buffer, nullptr,
                                                  f.rtv_handle);
    }
    return true;
}

bool overlay_init(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    g_overlay.owned_swap_chain = swap_chain;
    if (FAILED(swap_chain->GetDevice(
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&g_overlay.device)))) {
        logf("overlay: GetDevice failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) {
        logf("overlay: swap_chain->GetDesc failed");
        return false;
    }
    g_overlay.hwnd              = desc.OutputWindow;
    g_overlay.back_buffer_count = desc.BufferCount;
    g_overlay.rt_format         = desc.BufferDesc.Format;
    if (g_overlay.back_buffer_count == 0 ||
        g_overlay.back_buffer_count > kMaxBackBuffers) {
        logf("overlay: unexpected back-buffer count %u",
             g_overlay.back_buffer_count);
        return false;
    }
    g_overlay.frames.assign(g_overlay.back_buffer_count, FrameContext{});

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
    g_overlay.rtv_descriptor_size =
        g_overlay.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV heap: ImGui uses slot 0 for its font atlas. We don't load any
    // textures, but ImGui_ImplDX12 still needs a shader-visible heap.
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 4;
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

    if (!create_back_buffer_targets(swap_chain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize  = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                             static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime    = 1.0f / 60.0f;
    io.IniFilename  = nullptr;
    io.LogFilename  = nullptr;
    ImGui::StyleColorsDark();

    // Pre-build the font atlas (lazy build inside NewFrame stack-
    // overflows the HashLink host thread — see memory feedback).
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
    logf("overlay: DX12+ImGui init OK (hwnd=%p, buffers=%u, fmt=%d, queue=%p)",
         g_overlay.hwnd, g_overlay.back_buffer_count,
         static_cast<int>(g_overlay.rt_format), static_cast<void*>(queue));
    return true;
}

// --- The DPS table window ------------------------------------------

void render_imgui_window() {
    aggregator_tick();
    AggSnapshot snap = aggregator_snapshot();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,    kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Border,      kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,     kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,        kColText);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(48, 36, 16, 240));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,    IM_COL32( 0,  0,  0, 60));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, IM_COL32( 0,  0,  0, 110));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColBezel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    ImGui::Begin("DPS Meter", nullptr,
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar);

    // Header line: status + fight + totals
    if (!snap.scanning_ready) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
                           "Anchoring DamageDisplay type tag...");
    } else if (!snap.locked) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
                           "Locating local Hero...");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                           "Fight #%d  %s",
                           snap.fight_id,
                           snap.in_combat ? "[IN COMBAT]" : "[idle]");
    }
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("elapsed %5.1fs", snap.elapsed_sec);
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::Text("total %10.0f", snap.total_damage);
    ImGui::SameLine(0.0f, 24.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                       "DPS %9.1f", snap.dps);

    ImGui::Spacing();

    // Table.
    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##skills", 7, table_flags,
                          ImVec2(0.0f, -ImGui::GetTextLineHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Skill",  ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Hits",   ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Total",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Max",    ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("Crit%",  ImGuiTableColumnFlags_WidthStretch, 0.65f);
        ImGui::TableSetupColumn("DPS",    ImGuiTableColumnFlags_WidthStretch, 0.85f);
        ImGui::TableSetupColumn("%",      ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snap.row_count; ++i) {
            const SkillRow& r = snap.rows[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.skill);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", r.hit_count);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.0f", r.total);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f", r.max_hit);

            ImGui::TableSetColumnIndex(4);
            float crit_pct = r.hit_count > 0
                ? 100.0f * static_cast<float>(r.crit_count) /
                           static_cast<float>(r.hit_count)
                : 0.0f;
            if (crit_pct > 0.0f) {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                   "%.0f%%", crit_pct);
            } else {
                ImGui::TextUnformatted("-");
            }

            ImGui::TableSetColumnIndex(5);
            double row_dps = (snap.elapsed_sec > 0.001)
                ? r.total / snap.elapsed_sec : 0.0;
            ImGui::Text("%.1f", row_dps);

            ImGui::TableSetColumnIndex(6);
            float pct = snap.total_damage > 0.001
                ? 100.0f * static_cast<float>(r.total / snap.total_damage)
                : 0.0f;
            ImGui::Text("%.0f%%", pct);
        }
        ImGui::EndTable();
    }

    // Footer.
    ScanStats st = damage_scan_stats();
    ImGui::Text("scan: %lu polls, last %u ms, hot=%u, DRs=%llu  "
                "tag=0x%llx   F8 hide / F9 reset",
                static_cast<unsigned long>(st.poll_count),
                st.last_scan_ms, st.hot_regions,
                static_cast<unsigned long long>(st.unique_drs),
                static_cast<unsigned long long>(st.type_tag));

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
}

// --- Render frame ---------------------------------------------------

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* queue) {
    UINT idx = swap_chain->GetCurrentBackBufferIndex();
    if (idx >= g_overlay.frames.size()) return;
    auto& frame = g_overlay.frames[idx];
    if (!frame.allocator || !frame.back_buffer) return;

    if (!wait_for_frame(frame, kFenceTimeoutMs)) {
        int n = ++g_consecutive_slow_frames;
        if (n == 1 || (n % 30) == 0) {
            logf("overlay: fence wait timed out (%d consecutive)", n);
        }
        if (n >= kAutoDisableSlowFrames) {
            g_overlay_enabled.store(false);
            logf("overlay: %d slow frames in a row -> auto-disabled "
                 "(press F8 to re-enable)", n);
        }
        return;
    }
    g_consecutive_slow_frames = 0;

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

    frame.command_list->OMSetRenderTargets(1, &frame.rtv_handle, FALSE,
                                           nullptr);
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

// --- Public hook-side API ------------------------------------------

void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* queue) {
    if (g_overlay.init_failed) return;
    if (!queue) return;
    if (!g_overlay_enabled.load()) return;

    bool expected = false;
    if (!g_in_render.compare_exchange_strong(expected, true)) return;
    struct Scope { ~Scope() { g_in_render.store(false); } } scope;

    if (!g_overlay.initialized) {
        if (!overlay_init(swap_chain, queue)) {
            g_overlay.init_failed = true;
            release_all();
            return;
        }
        g_overlay.initialized = true;
    }
    if (swap_chain != g_overlay.owned_swap_chain) return;
    overlay_render(swap_chain, queue);
}

void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT /*width*/, UINT /*height*/) {
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    logf("overlay: resize -> %u buffers", buffer_count);
    for (auto& f : g_overlay.frames) {
        if (!wait_for_frame(f, 1000)) {
            logf("overlay: resize drain timed out");
        }
        f.fence_value = 0;
    }
    release_frame_targets();
    if (buffer_count != 0 && buffer_count != g_overlay.back_buffer_count) {
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
        for (auto& f : g_overlay.frames) {
            if (!wait_for_frame(f, 1000)) {
                logf("overlay: shutdown drain timed out");
            }
        }
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

}  // namespace dpsmeter
