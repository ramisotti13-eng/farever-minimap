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
#include "damage.h"
#include "hero_state.h"
#include "skill_resolve.h"
#include "entity_state.h"
#include "d3d12_hook.h"
#include "overlay.h"

#include <windows.h>
#include <cstdio>

namespace fv = farever;

namespace {

HMODULE   g_self = nullptr;
fv::LibHL g_libhl{};

// v0.4.13 kill switches for bisecting the persistent
// DX12Driver.present line 3306 AV reported in issues #12 / #16.
// The v0.4.12 log showed the crash hitting while we had submitted=0
// frames (the overlay was paused), which rules out our render
// pipeline. The remaining always-on work is hl_alloc_obj watching
// plus damage_tick + hero_state_tick on the render thread. These
// switches let an affected user disable each path independently so
// we can tell which side is the trigger.
//
//   no_overlay  -> overlay_on_present returns immediately, ImGui-DX12
//                  never initialised, no command list submitted to the
//                  game queue. HL reads (damage + hero_state) still run.
//
//   no_hl_tick  -> damage + hero_state watchers never register,
//                  damage_tick and hero_state_tick are skipped in the
//                  Present hook. hl_alloc_obj is still trampolined but
//                  the dispatcher has no callbacks to fire. Overlay
//                  still runs.
//
// Two ways to engage each switch. v0.4.13.1 added the file-flag form
// because Steam launches the game with its own pre-existing
// environment block, so an env var set in a .bat before
// `start steam://run/<appid>` never reaches Farever.exe (Steam was
// already running when the .bat ran).
//
//   ENV var   FAREVER_NO_OVERLAY=1 / FAREVER_NO_HL_TICK=1
//   FILE flag data/no_overlay.flag  / data/no_hl_tick.flag
//             (contents irrelevant; presence is the signal. Path is
//              relative to the directory dinput8.dll lives in, i.e.
//              the Farever folder.)
bool env_flag(const char* name) {
    char buf[8] = {};
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return buf[0] == '1';
}

// Path of "<dll-dir>\data\<name>". Returns true iff the file exists
// (zero-byte is fine — we don't read the contents). Wraps GetModuleFileName
// for our own HMODULE so we look relative to dinput8.dll, not the EXE
// CWD or the user profile.
bool file_flag(const char* name) {
    char dll_path[MAX_PATH] = {};
    if (g_self == nullptr) return false;
    DWORD n = GetModuleFileNameA(g_self, dll_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    // Strip the dinput8.dll filename to get the directory.
    for (DWORD i = n; i > 0; --i) {
        if (dll_path[i - 1] == '\\' || dll_path[i - 1] == '/') {
            dll_path[i] = 0;
            break;
        }
    }
    char full[MAX_PATH];
    int w = std::snprintf(full, MAX_PATH, "%sdata\\%s", dll_path, name);
    if (w <= 0 || w >= MAX_PATH) return false;
    DWORD attr = GetFileAttributesA(full);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Combined: switch engaged if EITHER an env var is set or the
// matching flag file exists. File form is needed for Steam launch
// because Steam doesn't pick up envs set after it started.
bool kill_switch(const char* env_name, const char* file_name) {
    return env_flag(env_name) || file_flag(file_name);
}

DWORD WINAPI worker_thread(LPVOID) {
    fv::logf("worker: started");

    const bool kill_overlay = kill_switch("FAREVER_NO_OVERLAY",
                                          "no_overlay.flag");
    const bool kill_hl_tick = kill_switch("FAREVER_NO_HL_TICK",
                                          "no_hl_tick.flag");
    if (kill_overlay) fv::overlay_kill();
    fv::overlay_set_kill_switch_state(kill_overlay, kill_hl_tick);
    fv::logf("worker: kill switches overlay=%s hl_tick=%s",
             kill_overlay ? "OFF" : "ON",
             kill_hl_tick ? "OFF" : "ON");

    if (!fv::libhl_wait_and_resolve(&g_libhl)) {
        fv::logf("worker: libhl resolution failed — aborting mod startup");
        return 1;
    }
    // Each module registers its own hl_alloc_obj watcher. Registration
    // must happen BEFORE hl_hook_install so we don't miss the first
    // burst of allocations on the way out of probe_init.
    fv::skill_resolve_init(g_libhl);
    if (!kill_hl_tick) {
        fv::damage_start(g_libhl);
        fv::hero_state_start();
    }
    // entity_state disabled in v0.4.4 -- the type-anchor wasn't enough
    // to keep stateId-field reads off garbage strings (shader uniforms,
    // FMOD event paths, prefab paths) on long sessions, and that path
    // is the likeliest remaining source of the DX12Driver.present AVs
    // since stripping the Atlas. The minimap loses automatic
    // collectibles-discovered classification; right-click toggle still
    // works.
    // fv::entity_state_start();

    if (!fv::hl_hook_install(g_libhl)) {
        fv::logf("worker: hl_hook_install failed");
        return 2;
    }
    if (!fv::d3d12_hook_install()) {
        fv::logf("worker: d3d12_hook_install failed — render-thread "
                 "ticks won't fire");
    }
    fv::logf("worker: live — alloc-hook + d3d12 hook armed; "
             "damage_tick + hero_state_tick drive from Present");
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
                fv::logf("DllMain: dinput8 proxy load failed — input may "
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
            fv::hero_state_stop();
            fv::damage_stop();
            // Intentionally NOT calling overlay_shutdown,
            // d3d12_hook_uninstall, hl_hook_uninstall,
            // dinput8_proxy_unload -- they were the unsafe steps.
            fv::log_line("DllMain DETACH (lean shutdown)");
            fv::log_close();
            break;
    }
    return TRUE;
}
