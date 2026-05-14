// Entry point for minimap.dll
//
// On load (DLL_PROCESS_ATTACH):
//   - open the log file
//   - start the live-position poll thread
//   - TODO: install the D3D12 Present hook
//
// On unload:
//   - stop the poll thread
//   - TODO: remove the hook
//
// For now this stage validates that the DLL can be injected into
// Farever.exe and starts pumping data without crashing. The actual
// in-game overlay rendering goes into a separate translation unit.

#include "log.h"
#include "live_position.h"
#include "d3d12_hook.h"
#include "overlay.h"

#include <windows.h>

namespace fmv = farevermod;

// Install the hook off the loader thread — D3D12CreateDevice +
// CreateSwapChainForHwnd inside d3d12_hook_install() can deadlock if
// driver DLLs run their own DllMain while we still hold the loader lock.
DWORD WINAPI hook_install_thread(LPVOID) {
    fmv::d3d12_hook_install();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            fmv::log_open();
            fmv::logf("DllMain ATTACH, PID=%lu", GetCurrentProcessId());
            fmv::live_position_start();
            HANDLE t = CreateThread(nullptr, 0, hook_install_thread,
                                    nullptr, 0, nullptr);
            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH:
            fmv::overlay_shutdown();
            fmv::d3d12_hook_uninstall();
            fmv::live_position_stop();
            fmv::log_line("DllMain DETACH");
            fmv::log_close();
            break;
    }
    return TRUE;
}
