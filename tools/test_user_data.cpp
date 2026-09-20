// Standalone test for the user_data path + migration helpers.
// Build + run (MSVC, from the repo root, in a VS developer prompt):
//   cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_user_data.cpp src\farever-mod\user_data.cpp /Fe:test_user_data.exe shell32.lib ole32.lib && test_user_data.exe'
#include "user_data.h"

#include <windows.h>
#include <shlobj.h>     // SHCreateDirectoryExW

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

// logf stub so user_data.cpp links without log.cpp.
namespace farever { void logf(const char*, ...) {} }

using namespace farever;

static void write_file(const std::wstring& path, const char* body) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
}

static bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

int main() {
    // --- the move list contains the writable files and excludes shipped /
    //     user-placed ones ---
    const auto& names = writable_data_names();
    auto has = [&](const wchar_t* n) {
        for (const auto& x : names) if (x == n) return true; return false; };
    assert(has(L"ui_state.json"));
    assert(has(L"keybinds.json"));
    assert(has(L"poi_done.json"));
    assert(has(L"poi_done.legacy-migrated.json"));
    assert(has(L"render_mode.txt"));
    assert(has(L"acknowledged_builds.txt"));
    assert(has(L"fight_history.json"));
    assert(has(L"farever_layout.ini"));
    assert(!has(L"minimap_calibration.json"));   // shipped read-only
    assert(!has(L"verified_builds.json"));        // shipped
    assert(!has(L"force_dcomp.flag"));            // user-placed flag

    const auto& globs = writable_data_globs();
    auto hasg = [&](const wchar_t* n) {
        for (const auto& x : globs) if (x == n) return true; return false; };
    assert(hasg(L"poi_done__*.json"));
    assert(hasg(L"boss_times__*.json"));
    assert(hasg(L"user_waypoints_*.json"));

    // --- FAREVER_DATA_DIR override + user_data_path join ---
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring base = std::wstring(tmp) + L"fv_ud_test";
    const std::wstring src  = base + L"\\src";
    const std::wstring dst  = base + L"\\dst";

    // Deterministic start: remove the specific files a prior run created.
    DeleteFileW((src + L"\\ui_state.json").c_str());
    DeleteFileW((src + L"\\poi_done__abc12345.json").c_str());
    DeleteFileW((dst + L"\\ui_state.json").c_str());
    DeleteFileW((dst + L"\\poi_done__abc12345.json").c_str());
    SHCreateDirectoryExW(nullptr, src.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);

    SetEnvironmentVariableW(L"FAREVER_DATA_DIR", dst.c_str());
    assert(user_data_path(L"ui_state.json") == dst + L"\\ui_state.json");

    // --- migration copies writable files, fixed + glob ---
    write_file(src + L"\\ui_state.json", "{}");
    write_file(src + L"\\poi_done__abc12345.json", "{\"ids\":[]}");
    write_file(src + L"\\minimap_calibration.json", "{}");   // must NOT migrate

    int copied = user_data_migrate_from(src, dst);
    assert(copied == 2);
    assert(exists(dst + L"\\ui_state.json"));
    assert(exists(dst + L"\\poi_done__abc12345.json"));
    assert(!exists(dst + L"\\minimap_calibration.json"));

    // --- second run is a no-op (fail-if-exists, no clobber) ---
    int copied2 = user_data_migrate_from(src, dst);
    assert(copied2 == 0);

    std::printf("user_data: all tests passed\n");
    return 0;
}
