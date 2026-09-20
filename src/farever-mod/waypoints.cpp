#include "waypoints.h"
#include "user_data.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>

namespace farever {
namespace {

// Must match the world token in overlay.cpp's pois_<world>.json path.
// No runtime multi-world detection yet (the game ships one world).
const wchar_t* const kWorldToken = L"W1_Siagarta";

std::mutex            g_mu;
std::vector<Waypoint> g_wps;
int                   g_next_id = 1;
int                   g_primary_id = 0;

std::wstring wp_json_path() {
    std::wstring rel = std::wstring(L"user_waypoints_") + kWorldToken + L".json";
    return user_data_path(rel.c_str());
}

// Caller must hold g_mu.
void save_locked() {
    std::ofstream f(wp_json_path());
    if (!f) { logf("waypoints: failed to open file for write"); return; }
    f << waypoints_serialize(g_wps, g_primary_id);
}

}  // namespace

void waypoints_load() {
    std::ifstream f(wp_json_path());
    std::lock_guard<std::mutex> lk(g_mu);
    g_wps.clear();
    g_next_id = 1;
    if (!f) {
        logf("waypoints: no file yet, starting fresh");
        return;
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    int loaded_primary = 0;
    g_wps = waypoints_parse(text, &loaded_primary);
    for (const auto& w : g_wps)
        if (w.id >= g_next_id) g_next_id = w.id + 1;
    // Validate primary still points to an existing waypoint.
    g_primary_id = 0;
    for (const auto& w : g_wps)
        if (w.id == loaded_primary) { g_primary_id = loaded_primary; break; }
    if (g_primary_id) {
        logf("waypoints: loaded %zu waypoint(s), primary id=%d",
             g_wps.size(), g_primary_id);
    } else {
        logf("waypoints: loaded %zu waypoint(s)", g_wps.size());
    }
}

int waypoints_add(float x, float y, float z, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_wps.size() >= kMaxWaypoints) {
        logf("waypoints: limit reached (%zu), add rejected", kMaxWaypoints);
        return 0;
    }
    Waypoint w{};
    w.id = g_next_id++;
    w.x = x; w.y = y; w.z = z;
    const char* nm = (name && *name) ? name : "Waypoint";
    wpjson::copy_into(w.name, sizeof(w.name), nm);
    g_wps.push_back(w);
    save_locked();
    logf("waypoints: added id=%d '%s' at (%.1f, %.1f, %.1f)",
         w.id, w.name, x, y, z);
    return w.id;
}

bool waypoints_remove(int id) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = std::find_if(g_wps.begin(), g_wps.end(),
                           [id](const Waypoint& w) { return w.id == id; });
    if (it == g_wps.end()) return false;
    g_wps.erase(it);
    if (g_primary_id == id) g_primary_id = 0;
    save_locked();
    logf("waypoints: removed id=%d", id);
    return true;
}

bool waypoints_rename(int id, const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = std::find_if(g_wps.begin(), g_wps.end(),
                           [id](const Waypoint& w) { return w.id == id; });
    if (it == g_wps.end()) return false;
    const char* nm = (name && *name) ? name : "Waypoint";
    wpjson::copy_into(it->name, sizeof(it->name), nm);
    save_locked();
    return true;
}

bool waypoints_set_style(int id, uint8_t color, uint8_t icon) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = std::find_if(g_wps.begin(), g_wps.end(),
                           [id](const Waypoint& w) { return w.id == id; });
    if (it == g_wps.end()) return false;
    it->color = color < 8 ? color : 0;
    it->icon  = icon  < 6 ? icon  : 0;
    save_locked();
    return true;
}

bool waypoints_set_primary(int id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (id == 0) {
        if (g_primary_id == 0) return true;   // no-op
        g_primary_id = 0;
        save_locked();
        return true;
    }
    auto it = std::find_if(g_wps.begin(), g_wps.end(),
                           [id](const Waypoint& w) { return w.id == id; });
    if (it == g_wps.end()) return false;
    g_primary_id = id;
    save_locked();
    return true;
}

int waypoints_get_primary() { return g_primary_id; }

const std::vector<Waypoint>& waypoints_get() { return g_wps; }

}  // namespace farever
