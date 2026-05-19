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
#include <cstdio>
#include <cstring>

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

// v0.5.2.3: when DCOMP swap chain creation fails on every variant
// (so the overlay cannot render at all), drop three files next to
// dinput8.dll: a ready-to-apply .reg file that disables AMD MPO,
// an undo .reg that removes the same values again, and a plain-text
// README that explains in 5 steps what the user should do. This is
// the cheapest user-side fix for the AMD MPO rejection pattern from
// issues #20 / #21 / #25 / #26: double-click, accept UAC, reboot.
// The DLL only writes these files in the failure path so working
// installs never see them. Content is fixed string literals only, so
// no external input can reach the file contents.
void write_amd_repair_files() {
    if (!g_win.self_module) return;
    wchar_t dll_path[MAX_PATH];
    DWORD got = GetModuleFileNameW(g_win.self_module, dll_path, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return;
    wchar_t* last_slash = wcsrchr(dll_path, L'\\');
    if (!last_slash) return;
    *last_slash = L'\0';  // strip filename, keep directory

    wchar_t reg_path[MAX_PATH];
    wchar_t undo_path[MAX_PATH];
    wchar_t txt_path[MAX_PATH];
    swprintf_s(reg_path, MAX_PATH,
               L"%s\\farever-fix-amd-overlay.reg", dll_path);
    swprintf_s(undo_path, MAX_PATH,
               L"%s\\farever-undo-amd-overlay-fix.reg", dll_path);
    swprintf_s(txt_path, MAX_PATH,
               L"%s\\OVERLAY_NOT_WORKING.txt", dll_path);

    const char* reg_apply =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "; farever-mod: AMD MPO disable workaround for DCOMP overlay\r\n"
        "; rejection (issues #20, #21, #25, #26). Reboot after applying.\r\n"
        "; To undo, run farever-undo-amd-overlay-fix.reg in this folder.\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\Dwm]\r\n"
        "\"DisableMPO\"=dword:00000001\r\n"
        "\"OverlayTestMode\"=dword:00000005\r\n";

    // Trailing "=-" deletes the registry value, restoring DWM
    // default behaviour. Safe to run even if the apply .reg was
    // never used: missing values just stay missing.
    const char* reg_undo =
        "Windows Registry Editor Version 5.00\r\n"
        "\r\n"
        "; farever-mod: undo the AMD MPO workaround. Reboot after\r\n"
        "; running. Restores DWM's default Multi-Plane Overlay\r\n"
        "; behaviour by removing both registry values.\r\n"
        "\r\n"
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\Dwm]\r\n"
        "\"DisableMPO\"=-\r\n"
        "\"OverlayTestMode\"=-\r\n";

    const char* txt_content =
        "farever-mod: overlay could not start\r\n"
        "====================================\r\n"
        "\r\n"
        "Your GPU driver (most likely AMD) rejected the DirectComposition\r\n"
        "swap chain we use to draw the overlay on top of the game. This\r\n"
        "is a known issue with AMD's MPO (Multi-Plane Overlay) feature\r\n"
        "and DirectComposition.\r\n"
        "\r\n"
        "Fix it in 5 steps:\r\n"
        "\r\n"
        "  1. Close the game.\r\n"
        "  2. Double-click \"farever-fix-amd-overlay.reg\" in this folder.\r\n"
        "  3. Click \"Yes\" on the User Account Control prompt.\r\n"
        "  4. Reboot Windows.\r\n"
        "  5. Launch the game again. The overlay should now show up.\r\n"
        "\r\n"
        "What this changes\r\n"
        "-----------------\r\n"
        "It sets two registry values under\r\n"
        "  HKLM\\SOFTWARE\\Microsoft\\Windows\\Dwm\r\n"
        "(DisableMPO=1 and OverlayTestMode=5) that turn off the GPU's\r\n"
        "Multi-Plane Overlay optimization. This is a documented\r\n"
        "Microsoft workaround and does not affect anything else than\r\n"
        "the Windows compositor path. Performance impact on a modern\r\n"
        "GPU is not measurable.\r\n"
        "\r\n"
        "How to undo\r\n"
        "-----------\r\n"
        "Double-click \"farever-undo-amd-overlay-fix.reg\" in this\r\n"
        "folder, accept the UAC prompt, reboot. That removes both\r\n"
        "registry values and Windows is back to its default Multi-\r\n"
        "Plane Overlay behaviour.\r\n"
        "\r\n"
        "Still not working?\r\n"
        "------------------\r\n"
        "If the overlay still does not show after the reboot, open\r\n"
        "an issue at\r\n"
        "https://github.com/ramisotti13-eng/farever-minimap/issues\r\n"
        "and attach farever-mod.log from this folder.\r\n";

    auto write_file = [](const wchar_t* path, const char* data) -> bool {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        DWORD len = static_cast<DWORD>(strlen(data));
        BOOL ok = WriteFile(h, data, len, &written, nullptr);
        CloseHandle(h);
        return ok && written == len;
    };

    bool ok_reg  = write_file(reg_path,  reg_apply);
    bool ok_undo = write_file(undo_path, reg_undo);
    bool ok_txt  = write_file(txt_path,  txt_content);

    char dir_utf8[MAX_PATH * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, dll_path, -1,
                        dir_utf8, sizeof(dir_utf8) - 1, nullptr, nullptr);
    logf("overlay_window: wrote AMD repair files to %s "
         "(apply=%s undo=%s txt=%s) — user can double-click "
         "farever-fix-amd-overlay.reg + reboot to fix, "
         "farever-undo-amd-overlay-fix.reg to revert later",
         dir_utf8,
         ok_reg  ? "ok" : "FAIL",
         ok_undo ? "ok" : "FAIL",
         ok_txt  ? "ok" : "FAIL");
}

// Returns the first visible top-level HWND owned by our process, or
// nullptr if none. Shared by initial wait + later refresh.
HWND find_game_window() {
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
    return found;
}

// HWND is good iff it is still a window AND GetClientRect succeeds
// with plausible dimensions. The plausibility bound catches the
// stale-HWND case where GetClientRect can return success-with-garbage
// (issue #29 saw client=4294967293x0).
bool hwnd_has_plausible_client_rect(HWND h, UINT* out_w = nullptr,
                                    UINT* out_h = nullptr) {
    if (!h || !IsWindow(h)) return false;
    RECT rc{};
    if (!GetClientRect(h, &rc)) return false;
    LONG w = rc.right - rc.left;
    LONG hgt = rc.bottom - rc.top;
    if (w <= 0 || hgt <= 0 || w > 16384 || hgt > 16384) return false;
    if (out_w) *out_w = static_cast<UINT>(w);
    if (out_h) *out_h = static_cast<UINT>(hgt);
    return true;
}

// v0.4.17.9: find the game's window (must wait for the game to
// create it). Polls for up to ~30 s.
bool wait_for_game_window() {
    for (int i = 0; i < 300; ++i) {
        HWND found = find_game_window();
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

// v0.5.2.4 issue #31 / #29: between wait_for_game_window (boot-time)
// and DCOMP setup (after hero lock stable, often 30+ s later) the
// game can destroy + recreate its window. The cached HWND then comes
// back as IsWindow=false (or GetClientRect=garbage), and the old
// abort-on-0x0 path gave up permanently. This walks the window list
// again, with retries, so a recreated window still gets picked up.
//
// Retries also cover the brief window during which the old HWND is
// gone but the new one has not been shown yet.
bool refresh_game_hwnd_if_stale() {
    UINT w = 0, h = 0;
    if (hwnd_has_plausible_client_rect(g_win.game_hwnd, &w, &h)) return true;

    HWND old = g_win.game_hwnd;
    logf("overlay_window: cached hwnd=%p no longer valid, re-enumerating",
         (void*)old);

    for (int i = 0; i < 50; ++i) {           // ~5 s total
        HWND candidate = find_game_window();
        if (candidate && candidate != old &&
            hwnd_has_plausible_client_rect(candidate, &w, &h)) {
            g_win.game_hwnd = candidate;
            logf("overlay_window: refreshed hwnd=%p (was %p) after %d × 100ms, "
                 "client=%ux%u",
                 (void*)candidate, (void*)old, i, w, h);
            return true;
        }
        // Also accept the same hwnd coming back to life (rare but cheap).
        if (candidate && candidate == old &&
            hwnd_has_plausible_client_rect(candidate, &w, &h)) {
            logf("overlay_window: cached hwnd recovered after %d × 100ms, "
                 "client=%ux%u", i, w, h);
            return true;
        }
        Sleep(100);
    }
    logf("overlay_window: no valid game window after 5 s of retries, giving up");
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

    // v0.5.3 issue #20/#21: log the adapter that the device picked.
    // Two users (feelsmth, kesmese's friend) hit
    // CreateSwapChainForComposition failing with DXGI_ERROR_INVALID_CALL
    // even though the game itself runs fine on DX12. Composition swap
    // chains have stricter driver/Windows-build requirements than plain
    // DX12, so capturing adapter description + driver level here makes
    // diagnosis possible without asking the user for dxdiag.
    {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter) {
            DXGI_ADAPTER_DESC1 ad{};
            if (SUCCEEDED(adapter->GetDesc1(&ad))) {
                char name[128] = {0};
                WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1,
                                    name, sizeof(name) - 1, nullptr, nullptr);
                logf("overlay_window: adapter[0] '%s' vendor=0x%04x "
                     "device=0x%04x vram=%lluMB",
                     name, ad.VendorId, ad.DeviceId,
                     (unsigned long long)(ad.DedicatedVideoMemory >> 20));
            }
            adapter->Release();
        }
    }

    // v0.5.2.4 issue #29 / #31: the cached HWND from boot may be dead
    // by the time we get here (game recreated its window during the
    // hero-lock wait). Validate and refresh before we commit numbers
    // to DXGI, otherwise GetClientRect on a dead handle returns garbage
    // (#29 logged client=4294967293x0) and the composition swap chain
    // rejects everything.
    if (!refresh_game_hwnd_if_stale()) {
        factory->Release();
        return false;
    }

    UINT w = 0, h = 0;
    hwnd_has_plausible_client_rect(g_win.game_hwnd, &w, &h);

    // v0.5.2.3 issue #20/#21/#25/#26: log raw window dimensions +
    // style so we can correlate composition swap chain failures with
    // what the game window looks like at that moment.
    {
        LONG style = GetWindowLongW(g_win.game_hwnd, GWL_STYLE);
        logf("overlay_window: client=%ux%u style=0x%08lx",
             w, h, (unsigned long)style);
    }

    // v0.5.2.1 issue #20/#21: try composition swap chain with several
    // descriptor variants in order. Some drivers (older Intel iGPU,
    // pre-1809 Win10) reject specific format / swap-effect combos with
    // DXGI_ERROR_INVALID_CALL (0x887a0001), and AMD MPO interaction
    // can reject everything with E_INVALIDARG (0x80070057) — for that
    // case the registry workaround (DisableMPO=1 under
    // HKLM\SOFTWARE\Microsoft\Windows\Dwm) is the real fix. Each
    // variant logs its own failure so we can see which combo the
    // driver accepts or rejects.
    struct ChainVariant {
        const char* name;
        DXGI_FORMAT format;
        DXGI_SWAP_EFFECT swap_effect;
        DXGI_ALPHA_MODE alpha;
        DXGI_SCALING scaling;
        UINT buffer_count;
    };
    const ChainVariant variants[] = {
        // A: original (BGRA + premultiplied + flip-discard, stretch).
        {"BGRA8/premul/flip-discard",
         DXGI_FORMAT_B8G8R8A8_UNORM,
         DXGI_SWAP_EFFECT_FLIP_DISCARD,
         DXGI_ALPHA_MODE_PREMULTIPLIED,
         DXGI_SCALING_STRETCH, 2},
        // B: RGBA instead of BGRA. Some Intel iGPU drivers prefer this
        // for composition swap chains even though the docs say BGRA.
        {"RGBA8/premul/flip-discard",
         DXGI_FORMAT_R8G8B8A8_UNORM,
         DXGI_SWAP_EFFECT_FLIP_DISCARD,
         DXGI_ALPHA_MODE_PREMULTIPLIED,
         DXGI_SCALING_STRETCH, 2},
        // C: BGRA + flip-sequential. Sequential keeps a back buffer
        // alive for composition; some older WDDM 1.x drivers needed
        // this before flip-discard was supported for DCOMP chains.
        {"BGRA8/premul/flip-sequential",
         DXGI_FORMAT_B8G8R8A8_UNORM,
         DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
         DXGI_ALPHA_MODE_PREMULTIPLIED,
         DXGI_SCALING_STRETCH, 2},
        // D (v0.5.2.3): scaling=NONE. STRETCH asks the driver to
        // place the swap chain in a hardware scaling plane; some AMD
        // MPO implementations reject that and accept NONE because it
        // skips the hardware overlay path entirely.
        {"BGRA8/premul/flip-discard/scale-none",
         DXGI_FORMAT_B8G8R8A8_UNORM,
         DXGI_SWAP_EFFECT_FLIP_DISCARD,
         DXGI_ALPHA_MODE_PREMULTIPLIED,
         DXGI_SCALING_NONE, 2},
        // E (v0.5.2.3): triple buffering. A handful of WDDM
        // configurations reject 2 buffers on composition swap chains
        // but accept 3 — costs ~50 MB more VRAM but is worth a shot
        // before declaring the system incompatible.
        {"BGRA8/premul/flip-discard/3buf",
         DXGI_FORMAT_B8G8R8A8_UNORM,
         DXGI_SWAP_EFFECT_FLIP_DISCARD,
         DXGI_ALPHA_MODE_PREMULTIPLIED,
         DXGI_SCALING_STRETCH, 3},
    };

    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = E_FAIL;
    const ChainVariant* chosen = nullptr;
    for (const auto& v : variants) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width       = w;
        sd.Height      = h;
        sd.Format      = v.format;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = v.buffer_count;
        sd.Scaling     = v.scaling;
        sd.SwapEffect  = v.swap_effect;
        sd.AlphaMode   = v.alpha;
        sd.Flags       = 0;

        hr = factory->CreateSwapChainForComposition(
            g_win.queue, &sd, nullptr, &sc1);
        if (SUCCEEDED(hr)) {
            chosen = &v;
            logf("overlay_window: composition swap chain ok with '%s'",
                 v.name);
            break;
        }
        logf("overlay_window: variant '%s' rejected hr=0x%08lx, "
             "trying next",
             v.name, (unsigned long)hr);
    }
    if (FAILED(hr) || !sc1) {
        logf("overlay_window: all composition swap chain variants "
             "failed (last hr=0x%08lx). Overlay cannot render on this "
             "system. Likely cause: AMD MPO rejecting DCOMP overlays. "
             "Writing repair files to the Farever folder so the user "
             "can apply the registry workaround without manual edits.",
             (unsigned long)hr);
        write_amd_repair_files();
        factory->Release();
        return false;
    }
    (void)chosen;  // only used in the log line above

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
         (void*)g_win.swap_chain, w, h);
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
    // Steam's notification toasts during character select.
    // v0.5.2: also wait for the lock to stay stable for ~1 s. Some
    // games (potentially Farever in certain states) briefly create a
    // preview Hero during character select / loading that would
    // pass our isMe check; the stability period defers DCOMP setup
    // until we're really in the world.
    int waited_100ms = 0;
    constexpr int kStabilityRequired100ms = 10;   // 1 s stable lock
    int stable_count = 0;
    std::uintptr_t prev_locked = 0;
    while (!g_win.stop.load(std::memory_order_acquire)) {
        std::uintptr_t now_locked = hero_state_locked_ptr();
        if (now_locked && now_locked == prev_locked) {
            if (++stable_count >= kStabilityRequired100ms) break;
        } else {
            stable_count = 0;
        }
        prev_locked = now_locked;
        Sleep(100);
        ++waited_100ms;
    }
    if (g_win.stop.load()) return;
    logf("overlay_window: hero lock stable after %d × 100ms total, "
         "starting DCOMP", waited_100ms);

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
    // v0.5.1: show/hide on F7 (default). v0.5.2: rebindable via the
    // toggle_overlay entry in keybinds.json — we poll
    // overlay_get_toggle_overlay_key() each iteration so a live
    // rebind takes effect on the next press.
    bool last_toggle_key = false;

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

    // v0.5.2 (kesmese #11): track hero-lock state so we can auto-hide
    // the DCOMP visual the moment the hero gets dropped (server AFK
    // kick → character select). Pre-v0.5.2 the last rendered frame
    // stayed visible on character select, covering the game's
    // post-kick UI with our stale minimap. Re-attach the visual when
    // a new hero locks (back in world).
    bool was_hero_locked = false;

    while (!g_win.stop.load(std::memory_order_acquire)) {
        unsigned tk = overlay_get_toggle_overlay_key();
        bool pressed = (GetAsyncKeyState(static_cast<int>(tk)) & 0x8000) != 0;
        if (pressed && !last_toggle_key) {
            bool v = !g_win.visible.load();
            g_win.visible.store(v);
            // v0.5.2 (kesmese #11 follow-up): properly hide via DCOMP
            // SetContent(nullptr). Pre-v0.5.2 we only stopped the
            // render thread, which left the last frame stuck on
            // screen (the swap chain back buffer + DCOMP visual hold
            // the last presented image, so DWM keeps compositing it).
            // Detach the visual's content + commit so DCOMP shows
            // nothing for our layer.
            if (g_win.dcomp_visual && g_win.dcomp_device) {
                g_win.dcomp_visual->SetContent(
                    v ? static_cast<IUnknown*>(g_win.swap_chain)
                      : nullptr);
                g_win.dcomp_device->Commit();
            }
            // Force the smart-hover state false when hiding so any
            // stale "cursor was on a button last frame" doesn't keep
            // eating clicks after the overlay has been hidden.
            if (!v) overlay_set_wants_real_input(false);
            logf("overlay_window: toggle_overlay -> %s",
                 v ? "visible" : "hidden");
        }
        last_toggle_key = pressed;

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

        // v0.5.2: detect hero-lock edge transitions and toggle the
        // DCOMP visual content accordingly. Lost lock → SetContent
        // (null) so the last minimap frame doesn't stay glued on
        // top of post-kick / character-select UI. New lock → re-
        // attach. Also re-SetRoot defensively (kesmese #11 reported
        // overlay disappearing after dungeon-instance entry — not
        // reproducible locally, but a zone transition could in
        // principle invalidate the composition tree; re-rooting at
        // every lock edge is a cheap self-heal).
        bool hero_locked_now = (hero_state_locked_ptr() != 0);
        if (hero_locked_now != was_hero_locked &&
            g_win.dcomp_visual && g_win.dcomp_device) {
            g_win.dcomp_visual->SetContent(
                hero_locked_now && g_win.visible.load()
                    ? static_cast<IUnknown*>(g_win.swap_chain)
                    : nullptr);
            if (hero_locked_now && g_win.dcomp_target) {
                // Defensive re-root in case the dungeon-entry zone
                // transition tore down the composition target's
                // child list. Cheap and idempotent.
                g_win.dcomp_target->SetRoot(g_win.dcomp_visual);
            }
            g_win.dcomp_device->Commit();
            if (!hero_locked_now) overlay_set_wants_real_input(false);
            logf("overlay_window: hero lock %s, visual %s",
                 hero_locked_now ? "acquired" : "lost",
                 hero_locked_now ? "(re-)attached" : "detached");
            was_hero_locked = hero_locked_now;
        }

        if (g_win.visible.load() && hero_locked_now) {
            overlay_on_present(g_win.swap_chain, g_win.queue);

            // v0.5.3 issue #30: Present1 with a tight dirty rect so DWM
            // only re-composites the region where our UI lives. On
            // ultrawide screens the savings are huge — our buffer is
            // game-window-sized (3440×1440 in the reporter's case) but
            // our visible content typically occupies <10% of that.
            // We union with the previous frame's rect so a window
            // moving / closing properly clears its old footprint from
            // DWM's cached composite.
            static RECT s_prev_dirty{0, 0, 0, 0};
            static bool s_prev_valid = false;

            int dx = 0, dy = 0, dw = 0, dh = 0;
            HRESULT hr = E_FAIL;
            if (overlay_get_dirty_rect(&dx, &dy, &dw, &dh)) {
                if (dx + dw > (int)current_w) dw = (int)current_w - dx;
                if (dy + dh > (int)current_h) dh = (int)current_h - dy;
                RECT cur{ dx, dy, dx + dw, dy + dh };
                RECT combined = cur;
                if (s_prev_valid) {
                    if (s_prev_dirty.left   < combined.left)   combined.left   = s_prev_dirty.left;
                    if (s_prev_dirty.top    < combined.top)    combined.top    = s_prev_dirty.top;
                    if (s_prev_dirty.right  > combined.right)  combined.right  = s_prev_dirty.right;
                    if (s_prev_dirty.bottom > combined.bottom) combined.bottom = s_prev_dirty.bottom;
                }
                s_prev_dirty = cur;
                s_prev_valid = true;
                DXGI_PRESENT_PARAMETERS pp{};
                pp.DirtyRectsCount = 1;
                pp.pDirtyRects = &combined;
                hr = g_win.swap_chain->Present1(1, 0, &pp);
            }
            if (FAILED(hr)) {
                g_win.swap_chain->Present(1, 0);
            }
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
