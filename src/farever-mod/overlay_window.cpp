// v0.4.17 Option B: dedicated overlay window with own D3D12 stack.
//
// We've proven the recurring `DX12Driver.present line 3306` AV is
// triggered by vtable hooks on the game's swap chain (no_d3d12.flag
// bisection in v0.4.15.2 was rock stable, every variant that left
// any Present / ResizeBuffers / ExecuteCommandLists hook on the
// game's swap chain crashed eventually). The fix is to stop touching
// the game's swap chain at all for rendering and host our overlay in
// our own window with our own swap chain.
//
// Architecture:
//   * Own top-level WS_EX_LAYERED + WS_EX_TOPMOST + WS_EX_NOACTIVATE
//     popup window, color-keyed magenta for transparency.
//   * Own D3D12 device + DIRECT command queue + flip-discard
//     swap chain on that window.
//   * Own render thread runs at ~60 Hz, calls overlay_on_present
//     (the existing ImGui pipeline from overlay.cpp), then
//     IDXGISwapChain::Present on our swap chain.
//   * Window position polled each frame to follow the game window
//     so the user sees the overlay sitting on top of the game.
//   * The game's swap chain still has a Present hook installed by
//     d3d12_hook.cpp, but only to drive damage_tick / hero_state_tick
//     (HashLink heap reads must happen on a thread the game GC
//     knows about — see feedback_hashlink_pump_thread.md). The hook
//     does no rendering, no submission, no queue capture.

#include "overlay_window.h"
#include "overlay.h"
#include "hero_state.h"
#include "log.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dcomp.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace farever {
namespace {

constexpr wchar_t kWndClass[] = L"farever_overlay_window";

// v0.4.17.1: switched from SetLayeredWindowAttributes(COLORKEY) to
// DirectComposition. COLORKEY worked in the GDI era but is unreliable
// with DXGI/DWM on modern Windows — rami's first test showed our
// magenta clear pixels rendering as opaque pink instead of being
// keyed out. DirectComposition + a composition swap chain with
// DXGI_ALPHA_MODE_PREMULTIPLIED is the modern path for transparent
// D3D windows: the compositor does per-pixel alpha blending with the
// desktop (game) underneath.
// v0.4.17.9: instead of creating our own window, mount the DCOMP
// visual directly onto the game's HWND. Our composition swap chain
// becomes the topmost visual in the game window's composition tree.
// Benefits:
//   * No second window → no mouse-capture issues, no NCHITTEST hack
//   * DWM is always actively compositing the game window, so no
//     "DCOMP wake-up" dance is needed
//   * Our visual follows the game window automatically (resize,
//     minimize, alt-tab all just work)
//   * No vtable patches on the game's swap chain — the crash-safe
//     property of Option B is preserved
struct OverlayWindow {
    HMODULE                 self_module    = nullptr;
    HWND                    game_hwnd      = nullptr;

    ID3D12Device*           device         = nullptr;
    ID3D12CommandQueue*     queue          = nullptr;
    IDXGISwapChain3*        swap_chain     = nullptr;

    IDCompositionDevice*    dcomp_device   = nullptr;
    IDCompositionTarget*    dcomp_target   = nullptr;
    IDCompositionVisual*    dcomp_visual   = nullptr;

    std::thread             render_thread;
    std::atomic<bool>       stop{false};
    std::atomic<bool>       ready{false};

    // F11-polled visibility. When off, render thread skips rendering;
    // the swap chain back buffer keeps its last "cleared to fully
    // transparent" state and the visual contributes nothing visually.
    std::atomic<bool>       visible{true};
};

OverlayWindow g_win{};

// v0.4.17.9: no wndproc needed — we don't create our own window.

// v0.4.17.9: find the game's window (must wait for the game to
// create it). Polls for up to ~30 s.
bool wait_for_game_window() {
    for (int i = 0; i < 300; ++i) {
        HWND found = nullptr;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId()) return TRUE;
            if (!IsWindowVisible(h))           return TRUE;
            HWND* out = reinterpret_cast<HWND*>(lp);
            *out = h;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&found));
        if (found) {
            g_win.game_hwnd = found;
            logf("overlay_window: game hwnd=%p found after %d × 100ms",
                 (void*)found, i);
            return true;
        }
        Sleep(100);
    }
    logf("overlay_window: timed out waiting for game window");
    return false;
}

bool create_d3d12() {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                  reinterpret_cast<void**>(&factory)))) {
        logf("overlay_window: CreateDXGIFactory1 failed");
        return false;
    }

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                 __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&g_win.device)))) {
        logf("overlay_window: D3D12CreateDevice failed");
        factory->Release();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(g_win.device->CreateCommandQueue(
            &qd, __uuidof(ID3D12CommandQueue),
            reinterpret_cast<void**>(&g_win.queue)))) {
        logf("overlay_window: CreateCommandQueue failed");
        factory->Release();
        return false;
    }

    RECT rc;
    GetClientRect(g_win.game_hwnd, &rc);

    // Composition swap chain: not bound to an HWND, instead handed
    // to DCOMP for rendering. Premultiplied alpha so the compositor
    // blends our output over the game window's existing composition.
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width       = static_cast<UINT>(rc.right - rc.left);
    sd.Height      = static_cast<UINT>(rc.bottom - rc.top);
    sd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;   // DCOMP prefers BGRA
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags       = 0;

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForComposition(
        g_win.queue, &sd, nullptr, &sc1);
    if (FAILED(hr)) {
        logf("overlay_window: CreateSwapChainForComposition failed "
             "hr=0x%08lx",
             (unsigned long)hr);
        factory->Release();
        return false;
    }

    hr = sc1->QueryInterface(__uuidof(IDXGISwapChain3),
                             reinterpret_cast<void**>(&g_win.swap_chain));
    sc1->Release();
    if (FAILED(hr)) {
        logf("overlay_window: QI(IDXGISwapChain3) failed hr=0x%08lx",
             (unsigned long)hr);
        factory->Release();
        return false;
    }
    factory->Release();

    // DirectComposition: mount the visual on the GAME's window.
    // CreateTargetForHwnd(game_hwnd, TRUE) makes our visual the
    // topmost in the game window's composition tree — the
    // compositor blends our content over whatever the game renders.
    // No second window of our own, no mouse capture, no DWM
    // wake-up dance needed.
    if (FAILED(DCompositionCreateDevice(
            nullptr, __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(&g_win.dcomp_device)))) {
        logf("overlay_window: DCompositionCreateDevice failed");
        return false;
    }
    if (FAILED(g_win.dcomp_device->CreateTargetForHwnd(
            g_win.game_hwnd, TRUE, &g_win.dcomp_target))) {
        logf("overlay_window: CreateTargetForHwnd(game_hwnd) failed");
        return false;
    }
    if (FAILED(g_win.dcomp_device->CreateVisual(&g_win.dcomp_visual))) {
        logf("overlay_window: CreateVisual failed");
        return false;
    }
    if (FAILED(g_win.dcomp_visual->SetContent(g_win.swap_chain))) {
        logf("overlay_window: visual SetContent failed");
        return false;
    }
    if (FAILED(g_win.dcomp_target->SetRoot(g_win.dcomp_visual))) {
        logf("overlay_window: target SetRoot failed");
        return false;
    }
    if (FAILED(g_win.dcomp_device->Commit())) {
        logf("overlay_window: DCOMP Commit failed");
        return false;
    }

    logf("overlay_window: D3D12+DCOMP ready, device=%p queue=%p swap=%p "
         "(%ux%u)",
         (void*)g_win.device, (void*)g_win.queue,
         (void*)g_win.swap_chain, sd.Width, sd.Height);
    return true;
}

void release_d3d12() {
    if (g_win.dcomp_visual) { g_win.dcomp_visual->Release(); g_win.dcomp_visual = nullptr; }
    if (g_win.dcomp_target) { g_win.dcomp_target->Release(); g_win.dcomp_target = nullptr; }
    if (g_win.dcomp_device) { g_win.dcomp_device->Release(); g_win.dcomp_device = nullptr; }
    if (g_win.swap_chain)   { g_win.swap_chain->Release();   g_win.swap_chain   = nullptr; }
    if (g_win.queue)        { g_win.queue->Release();        g_win.queue        = nullptr; }
    if (g_win.device)       { g_win.device->Release();       g_win.device       = nullptr; }
}

// v0.4.17.9: no sync needed — our visual is mounted on the game's
// window via DCOMP target, so it follows the game window
// automatically.

void render_thread_main() {
    if (!wait_for_game_window()) return;

    // v0.5.1 (issue #18 part 1): wait for the first hero lock before
    // bringing up the DCOMP visual. Pre-v0.5.1 the visual was
    // created immediately at game-boot, which conflicted with
    // Steam's notification toasts during character select (Steam
    // friend-online / game-started popups got stuck or visually
    // glitched). By the time the local Hero is locked the user is
    // through character select and into the world, and Steam's
    // notification animations have already finished their lifecycle.
    int waited_100ms = 0;
    while (!g_win.stop.load(std::memory_order_acquire) &&
           hero_state_locked_ptr() == 0) {
        Sleep(100);
        ++waited_100ms;
    }
    if (g_win.stop.load()) return;
    logf("overlay_window: hero locked after %d × 100ms, starting DCOMP",
         waited_100ms);

    if (!create_d3d12())          return;

    // ImGui-Win32 backend uses the hwnd to compute IO.DisplaySize via
    // GetClientRect each frame, which now naturally follows the game
    // window (resize, fullscreen toggle etc all work transparently).
    overlay_set_window_hwnd(g_win.game_hwnd);

    g_win.ready.store(true);
    logf("overlay_window: thread up, visual mounted on game window "
         "(F11 toggles overlay on/off)");

    auto next_frame = std::chrono::steady_clock::now();
    constexpr auto kFrameDur = std::chrono::microseconds(16'667);  // ~60 Hz
    // v0.5.1: show/hide on F7 (was F11 in v0.5 but F11 is also the
    // default toggle_clickthru keybind handled by the wndproc — same
    // keypress was hitting both paths, which is confusing UX).
    bool last_f7 = false;

    // v0.4.17.11: track game window client size so we can resize the
    // swap chain when the game grows from its tiny loading window to
    // its real gameplay size. Without this, the swap chain stays at
    // the initial dimensions and the overlay renders into a
    // top-left rectangle the size of the old window (rami screenshot
    // confirmed this).
    UINT current_w = 0, current_h = 0;
    {
        RECT rc;
        if (GetClientRect(g_win.game_hwnd, &rc)) {
            current_w = static_cast<UINT>(rc.right - rc.left);
            current_h = static_cast<UINT>(rc.bottom - rc.top);
        }
    }

    while (!g_win.stop.load(std::memory_order_acquire)) {
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !last_f7) {
            bool v = !g_win.visible.load();
            g_win.visible.store(v);
            logf("overlay_window: F7 -> %s",
                 v ? "rendering" : "blanked");
        }
        last_f7 = f7;

        // Detect game window resize and resize our swap chain to match.
        RECT rc;
        if (GetClientRect(g_win.game_hwnd, &rc)) {
            UINT new_w = static_cast<UINT>(rc.right - rc.left);
            UINT new_h = static_cast<UINT>(rc.bottom - rc.top);
            if (new_w > 0 && new_h > 0 &&
                (new_w != current_w || new_h != current_h)) {
                logf("overlay_window: game window resized %ux%u -> %ux%u, "
                     "rebuilding swap chain RTVs",
                     current_w, current_h, new_w, new_h);
                overlay_on_resize(g_win.swap_chain, 0, new_w, new_h);
                HRESULT hr = g_win.swap_chain->ResizeBuffers(
                    0, new_w, new_h, DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED(hr)) {
                    logf("overlay_window: ResizeBuffers failed hr=0x%08lx",
                         (unsigned long)hr);
                } else {
                    overlay_after_resize(g_win.swap_chain);
                    current_w = new_w;
                    current_h = new_h;
                }
            }
        }

        if (g_win.visible.load()) {
            overlay_on_present(g_win.swap_chain, g_win.queue);
            g_win.swap_chain->Present(1, 0);   // vsync
        }

        next_frame += kFrameDur;
        auto now = std::chrono::steady_clock::now();
        if (next_frame > now) {
            std::this_thread::sleep_until(next_frame);
        } else {
            next_frame = now;
        }
    }

    logf("overlay_window: render loop exiting");
    overlay_shutdown();
    release_d3d12();
}

}  // namespace

bool overlay_window_start() {
    g_win.self_module = GetModuleHandleW(L"dinput8.dll");
    if (!g_win.self_module) {
        logf("overlay_window: can't resolve own HMODULE");
        return false;
    }
    // v0.4.17.10: enable the wndproc subclass. Now that the visual
    // is mounted on the game's HWND (not a separate window of ours),
    // the subclass installs on the game's wndproc and gets to see
    // every mouse message before the game does — so ImGui can grab
    // clicks/drags that land on UI windows, and pass the rest
    // through to the game. The subclass was only ever an issue when
    // it was on our OWN window (paired with click-blocking we couldn't
    // override); on the game's window it's the standard approach.
    overlay_set_standalone_window(false);
    g_win.stop.store(false);
    g_win.render_thread = std::thread(render_thread_main);
    return true;
}

void overlay_window_stop() {
    g_win.stop.store(true);
    if (g_win.render_thread.joinable()) {
        g_win.render_thread.join();
    }
}

}  // namespace farever
