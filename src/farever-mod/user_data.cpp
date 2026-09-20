#include "user_data.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>     // SHGetKnownFolderPath, SHCreateDirectoryExW

namespace farever {
namespace {

// Directory of THIS DLL, no trailing slash. "" on failure. Same self-locating
// pattern the other modules use (find ourselves by a local function address).
std::wstring self_dir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&self_dir), &self))
        return L"";
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";
    s.resize(pos);
    return s;
}

// Resolve the user-data directory WITHOUT creating it.
std::wstring resolve_user_data_dir() {
    // FAREVER_DATA_DIR override, sized dynamically so even long paths are
    // honoured (a fixed MAX_PATH buffer would silently drop a longer value).
    // `need` counts the null, so need <= 1 means unset or empty -> use default.
    DWORD need = GetEnvironmentVariableW(L"FAREVER_DATA_DIR", nullptr, 0);
    if (need > 1) {
        std::wstring ov(need - 1, L'\0');
        GetEnvironmentVariableW(L"FAREVER_DATA_DIR", ov.data(), need);
        return ov;
    }

    PWSTR kf = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &kf))
            && kf) {
        std::wstring s(kf);
        CoTaskMemFree(kf);
        return s + L"\\farever-minimap";
    }
    return self_dir() + L"\\data";   // degrade to legacy behaviour, never crash
}

}  // namespace

std::wstring user_data_dir() {
    static const std::wstring dir = [] {
        std::wstring d = resolve_user_data_dir();
        // Create the directory and any parents; ALREADY_EXISTS is success.
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::wstring user_data_path(const wchar_t* relative) {
    return user_data_dir() + L"\\" + relative;
}

const std::vector<std::wstring>& writable_data_names() {
    static const std::vector<std::wstring> v = {
        L"ui_state.json",
        L"keybinds.json",
        L"poi_done.json",
        L"poi_done.legacy-migrated.json",
        L"render_mode.txt",
        L"acknowledged_builds.txt",
        L"fight_history.json",
        L"farever_layout.ini",
    };
    return v;
}

const std::vector<std::wstring>& writable_data_globs() {
    static const std::vector<std::wstring> v = {
        L"poi_done__*.json",
        L"boss_times__*.json",
        L"user_waypoints_*.json",
    };
    return v;
}

int user_data_migrate_from(const std::wstring& src_dir,
                           const std::wstring& dst_dir) {
    int copied = 0;
    auto copy_one = [&](const std::wstring& name) {
        std::wstring s = src_dir + L"\\" + name;
        std::wstring d = dst_dir + L"\\" + name;
        // TRUE = fail if the destination exists -> never clobber newer data.
        if (CopyFileW(s.c_str(), d.c_str(), TRUE)) ++copied;
    };
    for (const auto& name : writable_data_names()) copy_one(name);
    for (const auto& glob : writable_data_globs()) {
        std::wstring pattern = src_dir + L"\\" + glob;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                copy_one(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return copied;
}

void user_data_migrate() {
    const std::wstring dst = user_data_dir();
    const std::wstring marker = dst + L"\\.migrated";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    const std::wstring src = self_dir() + L"\\data";
    int copied = user_data_migrate_from(src, dst);

    HANDLE mh = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (mh != INVALID_HANDLE_VALUE) {
        CloseHandle(mh);
    } else {
        logf("user_data: WARNING could not write .migrated marker (err %lu); "
             "migration will re-run next launch (harmless, copies are "
             "fail-if-exists)", GetLastError());
    }

    logf("user_data: migration copied %d file(s) from \"%ls\" into \"%ls\"",
         copied, src.c_str(), dst.c_str());
}

}  // namespace farever
