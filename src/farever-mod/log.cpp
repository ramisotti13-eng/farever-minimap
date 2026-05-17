#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>
#include <string>

namespace farever {
namespace {

FILE* g_log = nullptr;
std::mutex g_log_mu;

std::wstring resolve_log_path() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&resolve_log_path),
        &hmod);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"farever-mod.log";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"farever-mod.log";
    s.resize(pos);
    s += L"\\farever-mod.log";
    return s;
}

void timestamp(char* buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

}  // namespace

// v0.5.2: rotate older log files at boot so the previous session's
// log isn't lost when the user reproduces an issue and uploads
// after a restart. Keeps farever-mod.log (current) plus .1 and .2.
void rotate_logs(const std::wstring& base) {
    std::wstring p1 = base + L".1";
    std::wstring p2 = base + L".2";
    // .1 → .2 (overwrite any existing .2)
    if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(p2.c_str());
        MoveFileW(p1.c_str(), p2.c_str());
    }
    // current → .1
    if (GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(p1.c_str());
        MoveFileW(base.c_str(), p1.c_str());
    }
}

void log_open() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) return;
    std::wstring log_path = resolve_log_path();
    rotate_logs(log_path);
    g_log = _wfsopen(log_path.c_str(), L"w", _SH_DENYWR);
    if (g_log) {
        fputs("# farever-mod (dinput8.dll) loaded\n", g_log);
        fflush(g_log);
    }
}

void log_close() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) {
        fputs("# farever-mod unloading\n", g_log);
        fclose(g_log);
        g_log = nullptr;
    }
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (!g_log) return;
    char ts[32];
    timestamp(ts, sizeof(ts));
    fprintf(g_log, "[%s] %s\n", ts, s.c_str());
    fflush(g_log);
}

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(buf);
}

}  // namespace farever
