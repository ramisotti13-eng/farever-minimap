#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <share.h>

namespace farevermod {

namespace {
FILE* g_log = nullptr;
std::mutex g_log_mu;

const wchar_t* LOG_PATH = L"D:\\farevermod\\minimap.log";

void timestamp(char* buf, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}
}  // namespace

void log_open() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) return;
    // _wfsopen with _SH_DENYWR so other processes can still read the
    // file while the DLL holds it open. fopen "w" defaults to exclusive
    // on Windows which made live tailing impossible.
    g_log = _wfsopen(LOG_PATH, L"w", _SH_DENYWR);
    if (g_log) {
        fputs("# minimap.dll loaded\n", g_log);
        fflush(g_log);
    }
}

void log_close() {
    std::lock_guard<std::mutex> g(g_log_mu);
    if (g_log) {
        fputs("# minimap.dll unloading\n", g_log);
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

}  // namespace farevermod
