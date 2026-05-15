// Entry point for the unified Farever mod (ships as dinput8.dll).
//
// Boot order:
//   1. Game starts. Windows resolves dinput8.dll from the EXE dir and
//      loads us (because we're sitting next to Farever.exe).
//   2. DllMain ATTACH: open the log, load the real dinput8 from
//      System32 and stash its exports so our forwarders work, then
//      spawn a worker thread.
//   3. Worker thread waits for libhl.dll to appear (~milliseconds in
//      a normal startup), resolves hl_alloc_obj + friends, then
//      installs the hook (phase 1 = stub; real hook is a follow-up).
//
// We deliberately do nothing heavy on the loader-lock thread —
// LoadLibrary, MinHook setup, type anchoring all happen on the worker.

#include "log.h"
#include "dinput8_proxy.h"
#include "libhl.h"
#include "hl_hook.h"
#include "damage.h"
#include "d3d12_hook.h"

#include <windows.h>
#include <atomic>
#include <thread>
#include <chrono>

namespace fv = farever;

namespace {

HMODULE   g_self = nullptr;
fv::LibHL g_libhl{};

// Smoke-test watcher: counts allocations of ent.Hero.
std::atomic<std::uint64_t> g_hero_alloc_count{0};
std::atomic<std::uintptr_t> g_first_hero{0};

void on_hero_alloc(std::uintptr_t obj) {
    std::uint64_t n = ++g_hero_alloc_count;
    if (n == 1) {
        g_first_hero.store(obj);
        fv::logf("watcher[ent.Hero]: first alloc at 0x%llx",
                 static_cast<unsigned long long>(obj));
    } else if (n == 10 || n == 100 || n == 1000 || (n % 5000) == 0) {
        fv::logf("watcher[ent.Hero]: %llu allocs so far, latest 0x%llx",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(obj));
    }
}


DWORD WINAPI worker_thread(LPVOID) {
    fv::logf("worker: started");
    if (!fv::libhl_wait_and_resolve(&g_libhl)) {
        fv::logf("worker: libhl resolution failed — aborting mod startup");
        return 1;
    }
    // Register watchers BEFORE installing the hook so we don't miss
    // the very first allocations on the way out of probe_init.
    fv::hl_hook_register(L"ent.Hero", on_hero_alloc);
    fv::damage_start(g_libhl);   // queue only; drain happens on Present
    if (!fv::hl_hook_install(g_libhl)) {
        fv::logf("worker: hl_hook_install failed");
        return 2;
    }
    if (!fv::d3d12_hook_install()) {
        fv::logf("worker: d3d12_hook_install failed — damage events "
                 "will queue but never drain");
    }
    fv::logf("worker: live — hl_alloc_obj hooked (ent.Hero + "
             "DamageResult), D3D12 Present hook drives damage_tick");
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
            fv::d3d12_hook_uninstall();
            fv::damage_stop();
            fv::hl_hook_uninstall();
            fv::dinput8_proxy_unload();
            fv::log_line("DllMain DETACH");
            fv::log_close();
            break;
    }
    return TRUE;
}
