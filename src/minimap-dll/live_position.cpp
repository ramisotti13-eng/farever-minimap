#include "live_position.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace farevermod {

namespace {

const wchar_t* LIVE_PATH = L"D:\\farevermod\\research\\live_position.json";

std::atomic<bool>       g_running{false};
std::thread             g_thread;
std::mutex              g_pos_mu;
LivePosition            g_pos;

// Trivial JSON value scraper — looks for "<key>": <number>. We don't
// pull a JSON library into the DLL for a 6-field flat object.
bool extract_double(const std::string& json, const char* key, double& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    char* end = nullptr;
    double v = strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

void poll_loop() {
    using namespace std::chrono_literals;
    while (g_running.load()) {
        std::ifstream f(LIVE_PATH);
        if (f) {
            std::string text((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            LivePosition p;
            p.valid =
                extract_double(text, "x",     p.x) &
                extract_double(text, "y",     p.y) &
                extract_double(text, "z",     p.z);
            extract_double(text, "rot_z", p.rot_z);
            extract_double(text, "ts",    p.ts);
            if (p.valid) {
                std::lock_guard<std::mutex> g(g_pos_mu);
                g_pos = p;
            }
        }
        std::this_thread::sleep_for(33ms);  // ~30 Hz, well above writer rate
    }
}

}  // namespace

void live_position_start() {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(poll_loop);
    log_line("live_position: poll thread started");
}

void live_position_stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    log_line("live_position: poll thread stopped");
}

LivePosition live_position_get() {
    std::lock_guard<std::mutex> g(g_pos_mu);
    return g_pos;
}

}  // namespace farevermod
