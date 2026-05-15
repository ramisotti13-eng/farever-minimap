#pragma once

namespace farever {

// Resolve %SystemRoot%\System32\dinput8.dll once at DLL load. Returns
// false (and logs) if the system DLL can't be loaded — in that case
// every exported forwarder returns E_NOTIMPL and the game's input
// will be broken, but the rest of our mod still works.
bool dinput8_proxy_load();

// Drop the handle to the real dinput8.dll. Called on DLL_PROCESS_DETACH.
void dinput8_proxy_unload();

}  // namespace farever
