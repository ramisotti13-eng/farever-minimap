// Entry point for the unified Farever mod (ships as dinput8.dll).
//
// Boot order:
//   1. Game starts. Windows resolves dinput8.dll from the EXE dir and
//      loads us (because we're sitting next to Farever.exe).
//   2. DllMain ATTACH: open the log, load the real dinput8 proxy,
//      spawn a worker thread.
//   3. Worker thread waits for libhl.dll, resolves hl_alloc_obj,
//      registers all module watchers (damage source + hero state),
//      installs the hooks. The render thread then drives every
//      module's tick from the D3D12 Present callback.

#include "log.h"
#include "dinput8_proxy.h"
#include "libhl.h"
#include "hl_hook.h"
#include "hl_pump.h"
#include "damage.h"
#include "heal.h"
#include "hero_state.h"
#include "party_state.h"
#include "camera_state.h"
#include "boss_timer.h"
#include "mob_watch.h"
#include "activity_icons.h"
#include "target_state.h"
#include "skill_resolve.h"
#include "uid_registry.h"
#include "codex_state.h"
#include "entity_state.h"
#include "d3d12_hook.h"
#include "anticheat.h"
#include "render_mode.h"
#include "user_data.h"
#include "overlay.h"
#include "overlay_window.h"
#include "plugins.h"
#include "textures.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace fv = farever;

namespace {

HMODULE   g_self = nullptr;
fv::LibHL g_libhl{};

// v0.5.3.3 diagnostic kill-switch helpers. Returns true if
// data/<name> exists next to the loaded DLL. The flags themselves are
// documented in foe_state.h / plugins.h.
bool flag_file_present(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return false;
    s.resize(pos);
    s += L"\\data\\";
    s += name;
    return GetFileAttributesW(s.c_str()) != INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI worker_thread(LPVOID) {
    fv::logf("worker: started");

    // #65: relocate user-written state to %LOCALAPPDATA%\farever-minimap and
    // migrate any existing files out of the game folder. Must run before
    // anticheat_gate (reads acknowledged_builds.txt) and render_mode_resolve
    // (reads render_mode.txt). Needs no libhl, only Win32 file APIs.
    fv::user_data_migrate();

    // Kick off the minimap mosaic decode NOW (own thread, pure WIC, no
    // device needed) so its multi-second decode overlaps the game's
    // loading screen. The overlay then only uploads the ready pixels and
    // the map shows up immediately instead of being black for seconds.
    {
        wchar_t mp[MAX_PATH];
        DWORD n = GetModuleFileNameW(g_self, mp, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring s(mp);
            auto pos = s.find_last_of(L'\\');
            if (pos != std::wstring::npos) {
                s.resize(pos);
                s += L"\\data\\maps\\W1_Siagarta.preview.png";
                fv::textures_preload_mosaic(s.c_str());
                fv::logf("worker: mosaic pre-decode started");
            }
        }
    }

    if (!fv::libhl_wait_and_resolve(&g_libhl)) {
        fv::logf("worker: libhl resolution failed, aborting mod startup");
        return 1;
    }

    // Issue #54: anti-cheat gate. If Farever ships an anti-cheat or runs an
    // unverified build, refuse to inject at all - no watchers, no hooks, no
    // overlay, no hl_pump. The user already saw the MessageBox inside the
    // gate; we just exit the worker cleanly.
    if (!fv::anticheat_gate(g_self)) {
        fv::logf("worker: anti-cheat gate tripped - mod disabled, not "
                 "injecting");
        return 0;
    }
    // v0.5.3.2 diagnostic kill switch for plugins. Set BEFORE the
    // module starts so the plugin scan can be skipped if needed.
    if (flag_file_present(L"no_plugins.flag")) {
        fv::logf("worker: no_plugins.flag found - plugins disabled");
        fv::plugins_set_disabled(true);
    }
    // #226 render-mode chooser. Resolve the backend before we install any
    // hooks. On a first run this shows the chooser, saves the pick, and
    // returns Defer; we then render NOTHING this session (the user restarts).
    // Deferring here avoids the issue #60 boot-modal race that stops the
    // game-swapchain backend from capturing its command queue.
    fv::RenderBackend backend = fv::RenderBackend::GameSwapchain;
    switch (fv::render_mode_resolve()) {
        case fv::RenderModeResolution::Defer:
            fv::logf("worker: render mode not set yet - overlay disabled this "
                     "session, restart the game to apply your choice");
            return 0;
        case fv::RenderModeResolution::UseDcomp:
            backend = fv::RenderBackend::Dcomp;
            break;
        case fv::RenderModeResolution::UseGameSwapchain:
            backend = fv::RenderBackend::GameSwapchain;
            break;
    }

    // Each module registers its own hl_alloc_obj watcher. Registration
    // must happen BEFORE hl_hook_install so we don't miss the first
    // burst of allocations on the way out of probe_init.
    fv::skill_resolve_init(g_libhl);
    fv::target_state_init(g_libhl);
    fv::uid_registry_init(g_libhl);
    fv::codex_state_init(g_libhl);   // issue #44 phase-1 diagnostic tracer
    fv::damage_start(g_libhl);
    fv::heal_start();         // issue #57 - local-player heal source
    fv::hero_state_start();
    fv::party_state_start();
    fv::camera_state_start();   // #65 camera-relative compass
    fv::boss_timer_start();      // boss speedrun mode (opt-in)
    fv::mob_watch_start();       // #65 rare/sparkling-mob proximity alert
    fv::activity_icons_init(g_libhl);   // #107 map icons from the game's CDB

    if (!fv::hl_hook_install(g_libhl)) {
        fv::logf("worker: hl_hook_install failed");
        return 2;
    }

    // Render backend selection. Must be set BEFORE d3d12_hook_install (which
    // installs a different hook set per backend) and before any overlay init.
    // The actual choice was resolved above by render_mode_resolve(); here we
    // just apply it. game-swapchain renders into the game's own swap chain
    // (no compositor blend / ultrawide #50, works under Proton/DXVK #45,
    // dodges AMD MPO). DCOMP renders into our own composition swap chain.
    const bool want_game_swapchain =
        (backend == fv::RenderBackend::GameSwapchain);
    fv::overlay_set_backend(backend);
    if (want_game_swapchain) {
        // The overlay subclasses the game's own window for input.
        fv::overlay_set_standalone_window(false);
        fv::logf("worker: render backend = GAME-SWAPCHAIN");
    } else {
        fv::logf("worker: render backend = DCOMP");
    }

    if (!fv::d3d12_hook_install()) {
        fv::logf("worker: d3d12_hook_install failed, render-thread "
                 "ticks won't fire");
    }
    // v0.6.0: background HL worker is default-on. Reads game state at
    // 20 Hz from a thread the HL GC doesn't know about - see
    // [[hashlink-pump-thread]] memory + RELEASE_NOTES_v0.6.0.md for why.
    // Opt-out via data/no_worker.flag falls back to the v0.5.6.1
    // Present-driven path (same instability profile as v0.5.6.1).
    if (flag_file_present(L"no_worker.flag")) {
        fv::logf("worker: no_worker.flag found - falling back to "
                 "Present-driven ticks (v0.5.6.1 behaviour)");
    } else {
        if (!fv::hl_pump_start(g_libhl)) {
            fv::logf("worker: hl_pump_start failed, falling back to "
                     "Present-driven ticks");
        }
    }
    // DCOMP drives the overlay from its own render thread + composition
    // swap chain. The game-swapchain backend instead renders from the
    // Present hook into the game's own back buffer, so it starts no
    // render thread here.
    if (!want_game_swapchain) {
        if (!fv::overlay_window_start()) {
            fv::logf("worker: overlay_window_start failed");
        }
    }

    // Issue #54: keep watching for an AC that loads shortly after start.
    fv::anticheat_start_rescans(g_self);

    fv::logf("worker: live; tick driver = %s; overlay backend = %s",
             fv::hl_pump_is_active() ? "background worker (hl_pump)"
                                     : "Present hook",
             want_game_swapchain ? "game-swapchain (Present hook submit)"
                                 : "own window via DCOMP");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_self = module;
            DisableThreadLibraryCalls(module);
            fv::log_open();
            fv::logf("DllMain ATTACH, PID=%lu", GetCurrentProcessId());
            if (!fv::dinput8_proxy_load()) {
                fv::logf("DllMain: dinput8 proxy load failed - input may "
                         "be broken, continuing anyway");
            }
            HANDLE t = CreateThread(nullptr, 0, worker_thread, nullptr, 0,
                                    nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Issue #8: doing a full MinHook uninstall during process
            // teardown raced the game's own DX12 / DXGI shutdown and
            // produced an exit-time error dialog for some users. The
            // process is dying anyway; let Windows reclaim our hook
            // pages with the rest of the address space rather than
            // re-poking the game's vtable mid-tear-down. We still
            // stop the render ticks so they don't keep firing into
            // freed state if the teardown takes longer than expected.
            fv::hl_pump_stop();
            fv::hero_state_stop();
            fv::damage_stop();
            // v0.5.1 (issue #18 audio choppy on quit): cleanly stop
            // the overlay-window render thread + free its D3D12 +
            // DCOMP resources before the process dies. Pre-v0.5.1 we
            // let the thread get force-killed at process exit, which
            // released the GPU resources hard and stuttered the
            // system audio driver for 5-10 s.
            fv::overlay_window_stop();
            // game-swapchain has no DCOMP render thread, so
            // overlay_window_stop didn't run overlay_shutdown - do it
            // here to release our command lists / heaps / fence cleanly.
            // (On process exit the OS terminates all other threads before
            // DETACH, so no game Present races this.)
            if (fv::overlay_backend() == fv::RenderBackend::GameSwapchain) {
                fv::overlay_shutdown();
            }
            // Intentionally NOT calling d3d12_hook_uninstall,
            // hl_hook_uninstall, dinput8_proxy_unload - those were
            // the unsafe steps from issue #8.
            fv::log_line("DllMain DETACH (lean shutdown)");
            fv::log_close();
            break;
    }
    return TRUE;
}
