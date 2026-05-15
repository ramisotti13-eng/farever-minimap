// dinput8.dll proxy. Windows resolves a non-system DLL name from the
// executable's directory before %SystemRoot%\System32, so dropping our
// dinput8.dll next to Farever.exe loads us instead of the real one —
// no injector necessary. We forward all five DirectInput8 exports to
// the actual system DLL so the game's input pipeline still works.
//
// This is the standard "drop-in proxy" pattern for Windows game mods.

#include "dinput8_proxy.h"
#include "log.h"

#include <windows.h>

// We deliberately do NOT include <unknwn.h> / <combaseapi.h>. Those
// headers declare `DllCanUnloadNow` / `DllGetClassObject` with C++
// linkage, which clashes with the `extern "C"` exports below. We only
// need REFIID (from windows.h / guiddef.h) and an opaque IUnknown
// pointer type — forward-declared here.
struct IUnknown;
using LPUNKNOWN = IUnknown*;

namespace farever {
namespace {

using PFN_DirectInput8Create  = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID,
                                                  LPVOID*, LPUNKNOWN);
using PFN_DllCanUnloadNow     = HRESULT(WINAPI*)();
using PFN_DllGetClassObject   = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using PFN_DllRegisterServer   = HRESULT(WINAPI*)();
using PFN_DllUnregisterServer = HRESULT(WINAPI*)();

HMODULE g_real_dinput8 = nullptr;

PFN_DirectInput8Create  g_pDirectInput8Create  = nullptr;
PFN_DllCanUnloadNow     g_pDllCanUnloadNow     = nullptr;
PFN_DllGetClassObject   g_pDllGetClassObject   = nullptr;
PFN_DllRegisterServer   g_pDllRegisterServer   = nullptr;
PFN_DllUnregisterServer g_pDllUnregisterServer = nullptr;

}  // namespace

bool dinput8_proxy_load() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) {
        logf("dinput8_proxy: GetSystemDirectoryW failed (n=%u, last=%lu)",
             n, GetLastError());
        return false;
    }
    wcscat_s(path, MAX_PATH, L"\\dinput8.dll");
    g_real_dinput8 = LoadLibraryW(path);
    if (!g_real_dinput8) {
        logf("dinput8_proxy: LoadLibraryW(%ls) failed (last=%lu)",
             path, GetLastError());
        return false;
    }
    g_pDirectInput8Create  = reinterpret_cast<PFN_DirectInput8Create>(
        GetProcAddress(g_real_dinput8, "DirectInput8Create"));
    g_pDllCanUnloadNow     = reinterpret_cast<PFN_DllCanUnloadNow>(
        GetProcAddress(g_real_dinput8, "DllCanUnloadNow"));
    g_pDllGetClassObject   = reinterpret_cast<PFN_DllGetClassObject>(
        GetProcAddress(g_real_dinput8, "DllGetClassObject"));
    g_pDllRegisterServer   = reinterpret_cast<PFN_DllRegisterServer>(
        GetProcAddress(g_real_dinput8, "DllRegisterServer"));
    g_pDllUnregisterServer = reinterpret_cast<PFN_DllUnregisterServer>(
        GetProcAddress(g_real_dinput8, "DllUnregisterServer"));
    logf("dinput8_proxy: real dinput8 at %p, "
         "DirectInput8Create=%p CanUnload=%p GetClassObject=%p "
         "Register=%p Unregister=%p",
         static_cast<void*>(g_real_dinput8),
         reinterpret_cast<void*>(g_pDirectInput8Create),
         reinterpret_cast<void*>(g_pDllCanUnloadNow),
         reinterpret_cast<void*>(g_pDllGetClassObject),
         reinterpret_cast<void*>(g_pDllRegisterServer),
         reinterpret_cast<void*>(g_pDllUnregisterServer));
    return true;
}

void dinput8_proxy_unload() {
    if (g_real_dinput8) {
        FreeLibrary(g_real_dinput8);
        g_real_dinput8 = nullptr;
        g_pDirectInput8Create  = nullptr;
        g_pDllCanUnloadNow     = nullptr;
        g_pDllGetClassObject   = nullptr;
        g_pDllRegisterServer   = nullptr;
        g_pDllUnregisterServer = nullptr;
    }
}

}  // namespace farever

// --- The five exports the OS / game calls into ----------------------
// Implemented at file scope (not in the namespace) and given the exact
// undecorated names + C linkage that dinput8.dll's importers expect.

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN punk) {
    if (!farever::g_pDirectInput8Create) return E_NOTIMPL;
    return farever::g_pDirectInput8Create(hinst, ver, riid, out, punk);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
    if (!farever::g_pDllCanUnloadNow) return S_FALSE;
    return farever::g_pDllCanUnloadNow();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(
    REFCLSID clsid, REFIID iid, LPVOID* out) {
    if (!farever::g_pDllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return farever::g_pDllGetClassObject(clsid, iid, out);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllRegisterServer() {
    if (!farever::g_pDllRegisterServer) return E_NOTIMPL;
    return farever::g_pDllRegisterServer();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllUnregisterServer() {
    if (!farever::g_pDllUnregisterServer) return E_NOTIMPL;
    return farever::g_pDllUnregisterServer();
}
