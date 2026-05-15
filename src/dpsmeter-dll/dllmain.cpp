// Entry point for dpsmeter.dll.
//
// Scaffold stage: install the D3D12 Present hook so we know the DLL
// got injected and the game's render loop is reaching us. No damage
// scan, no UI yet — that lands once we port dpsmeter_live.py into
// damage_scan.cpp.

#include "log.h"
#include "d3d12_hook.h"
#include "overlay.h"
#include "damage_scan.h"
#include "hero_state.h"

#include <windows.h>

namespace dps = dpsmeter;

DWORD WINAPI hook_install_thread(LPVOID) {
    dps::d3d12_hook_install();
    dps::damage_scan_start();
    dps::hero_state_start();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            dps::log_open();
            dps::logf("DllMain ATTACH, PID=%lu", GetCurrentProcessId());
            HANDLE t = CreateThread(nullptr, 0, hook_install_thread,
                                    nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            dps::hero_state_stop();
            dps::damage_scan_stop();
            dps::overlay_shutdown();
            dps::d3d12_hook_uninstall();
            dps::log_line("DllMain DETACH");
            dps::log_close();
            break;
    }
    return TRUE;
}
