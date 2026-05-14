// Inject minimap.dll into a running Farever.exe.
//
// Usage:
//   inject.exe [<dll_path>]
//   - With no arg: looks for minimap.dll next to inject.exe.
//   - With an arg: loads that DLL.
//
// Strategy: classic CreateRemoteThread(LoadLibraryW). Requires the
// caller to have PROCESS_CREATE_THREAD + VM_WRITE access to Farever
// — run elevated if Farever is elevated. No anti-cheat in Farever
// as of M0; if that changes, this approach must be reconsidered.

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

constexpr wchar_t TARGET[] = L"Farever.exe";

std::optional<DWORD> find_pid(const wchar_t* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exe_name) == 0) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found ? std::optional<DWORD>{found} : std::nullopt;
}

bool inject(DWORD pid, const std::wstring& dll_full_path) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        fwprintf(stderr, L"OpenProcess failed: %lu\n", GetLastError());
        return false;
    }

    // Allocate memory in target for the DLL path string.
    const size_t bytes = (dll_full_path.size() + 1) * sizeof(wchar_t);
    LPVOID remote_str = VirtualAllocEx(proc, nullptr, bytes,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remote_str) {
        fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, remote_str, dll_full_path.c_str(),
                            bytes, nullptr)) {
        fwprintf(stderr, L"WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote_str, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    auto load_lib_w = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!load_lib_w) {
        fwprintf(stderr, L"GetProcAddress(LoadLibraryW) failed\n");
        VirtualFreeEx(proc, remote_str, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, load_lib_w,
                                       remote_str, 0, nullptr);
    if (!thread) {
        fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(proc, remote_str, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, 5000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote_str, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (exit_code == 0) {
        fwprintf(stderr, L"LoadLibraryW returned 0 — DLL failed to load.\n");
        return false;
    }
    wprintf(L"Loaded DLL at HMODULE 0x%llx\n",
            static_cast<unsigned long long>(exit_code));
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring dll_path;
    if (argc >= 2) {
        dll_path = argv[1];
    } else {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dll_path = std::filesystem::path(self).parent_path() / L"minimap.dll";
    }
    dll_path = std::filesystem::absolute(dll_path).wstring();

    if (!std::filesystem::exists(dll_path)) {
        fwprintf(stderr, L"DLL not found: %ls\n", dll_path.c_str());
        return 2;
    }

    auto pid = find_pid(TARGET);
    if (!pid) {
        fwprintf(stderr, L"%ls is not running.\n", TARGET);
        return 3;
    }
    wprintf(L"Injecting %ls into PID %lu (%ls)\n",
            dll_path.c_str(), *pid, TARGET);
    return inject(*pid, dll_path) ? 0 : 4;
}
