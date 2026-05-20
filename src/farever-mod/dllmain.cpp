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
#include "overlay_window.h"
#include "plugins.h"

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

    if (!fv::libhl_wait_and_resolve(&g_libhl)) {
        fv::logf("worker: libhl resolution failed, aborting mod startup");
        return 1;
    }
    // v0.5.3.2 diagnostic kill switch for plugins. Set BEFORE the
    // module starts so the plugin scan can be skipped if needed.
    if (flag_file_present(L"no_plugins.flag")) {
        fv::logf("worker: no_plugins.flag found — plugins disabled");
        fv::plugins_set_disabled(true);
    }
    // Each module registers its own hl_alloc_obj watcher. Registration
    // must happen BEFORE hl_hook_install so we don't miss the first
    // burst of allocations on the way out of probe_init.
    fv::skill_resolve_init(g_libhl);
    fv::damage_start(g_libhl);
    fv::hero_state_start();

    if (!fv::hl_hook_install(g_libhl)) {
        fv::logf("worker: hl_hook_install failed");
        return 2;
    }
    if (!fv::d3d12_hook_install()) {
        fv::logf("worker: d3d12_hook_install failed, render-thread "
                 "ticks won't fire");
    }
    if (!fv::overlay_window_start()) {
        fv::logf("worker: overlay_window_start failed");
    }

    fv::logf("worker: live; Present-only tick driver hooked; "
             "overlay renders in own window via DCOMP");
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
            // v0.5.1 (issue #18 audio choppy on quit): cleanly stop
            // the overlay-window render thread + free its D3D12 +
            // DCOMP resources before the process dies. Pre-v0.5.1 we
            // let the thread get force-killed at process exit, which
            // released the GPU resources hard and stuttered the
            // system audio driver for 5-10 s.
            fv::overlay_window_stop();
            // Intentionally NOT calling d3d12_hook_uninstall,
            // hl_hook_uninstall, dinput8_proxy_unload — those were
            // the unsafe steps from issue #8.
            fv::log_line("DllMain DETACH (lean shutdown)");
            fv::log_close();
            break;
    }
    return TRUE;
}
