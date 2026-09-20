#include "poi_progress.h"
#include "pois.h"
#include "poi_profile_key.h"
#include "log.h"
#include "user_data.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace farever {
namespace {

std::mutex                       g_mu;
std::unordered_set<std::string>  g_done;
std::string                      g_profile_key;   // "" until a character locks

std::wstring legacy_path()   { return user_data_path(L"poi_done.json"); }
std::wstring migrated_path() { return user_data_path(L"poi_done.legacy-migrated.json"); }

// key is ASCII (sanitized) -> simple widen.
std::wstring profile_path(const std::string& key) {
    std::wstring w;
    for (char c : key)
        w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    std::wstring rel = L"poi_done__" + w + L".json";
    return user_data_path(rel.c_str());
}

bool file_exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Parse the tiny { "ids": ["...", ...] } file. Caller owns the set / lock.
// Returns the parsed set; empty on missing/malformed.
std::unordered_set<std::string> read_done_set(const std::wstring& path) {
    std::unordered_set<std::string> done;
    std::ifstream f(path);
    if (!f) return done;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    auto arr_start = text.find('[');
    auto arr_end   = text.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) return done;
    std::size_t i = arr_start + 1;
    while (i < arr_end) {
        auto q1 = text.find('"', i);
        if (q1 == std::string::npos || q1 >= arr_end) break;
        auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > arr_end) break;
        done.insert(text.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return done;
}

// Write the set to `path`. Caller holds g_mu.
void write_done_set(const std::wstring& path,
                    const std::unordered_set<std::string>& done) {
    std::ofstream f(path);
    if (!f) {
        logf("poi_progress: failed to open done file for write");
        return;
    }
    f << "{\n  \"ids\": [\n";
    bool first = true;
    for (const auto& id : done) {
        if (!first) f << ",\n";
        f << "    \"" << id << "\"";
        first = false;
    }
    f << "\n  ]\n}\n";
}

}  // namespace

void poi_progress_set_profile(const char* key_c) {
    if (!key_c || !*key_c) return;
    std::string key(key_c);

    std::lock_guard<std::mutex> lk(g_mu);
    if (key == g_profile_key) return;            // already active

    // Persist the outgoing character's progress.
    if (!g_profile_key.empty())
        write_done_set(profile_path(g_profile_key), g_done);

    g_profile_key = key;
    std::wstring path = profile_path(key);

    if (file_exists(path)) {
        g_done = read_done_set(path);
        logf("poi_progress: profile = %s (loaded %zu done IDs)",
             key.c_str(), g_done.size());
    } else if (file_exists(legacy_path())) {
        // First character after the update inherits the old global progress,
        // then the legacy file is consumed so later characters start fresh.
        g_done = read_done_set(legacy_path());
        write_done_set(path, g_done);
        MoveFileExW(legacy_path().c_str(), migrated_path().c_str(),
                    MOVEFILE_REPLACE_EXISTING);
        logf("poi_progress: profile = %s (migrated %zu IDs from legacy)",
             key.c_str(), g_done.size());
    } else {
        g_done.clear();
        logf("poi_progress: profile = %s (fresh)", key.c_str());
    }
}

void poi_progress_save() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_profile_key.empty()) return;           // no character yet -> nothing to save
    write_done_set(profile_path(g_profile_key), g_done);
}

void poi_progress_toggle(const char* poi_id) {
    if (!poi_id || !*poi_id) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_done.find(poi_id);
        if (it == g_done.end()) g_done.insert(poi_id);
        else                    g_done.erase(it);
    }
    poi_progress_save();
}

void poi_progress_set_ids(const char* const* ids, int count, bool done) {
    if (!ids || count <= 0) return;
    int changed = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (int i = 0; i < count; ++i) {
            const char* id = ids[i];
            if (!id || !*id) continue;
            if (done) { if (g_done.insert(id).second) ++changed; }
            else      { changed += static_cast<int>(g_done.erase(id)); }
        }
    }
    poi_progress_save();
    logf("poi_progress: set-ids count=%d done=%d changed=%d",
         count, done ? 1 : 0, changed);
}

void poi_progress_mark_all(const char* kind, bool done) {
    if (!kind || !*kind) return;
    const auto& pois = pois_get();
    int changed = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (const auto& p : pois) {
            if (std::strcmp(p.kind, kind) != 0) continue;
            if (!p.id[0]) continue;
            if (done) { if (g_done.insert(p.id).second) ++changed; }
            else      { changed += static_cast<int>(g_done.erase(p.id)); }
        }
    }
    poi_progress_save();
    logf("poi_progress: mark-all kind=%s done=%d changed=%d",
         kind, done ? 1 : 0, changed);
}

bool poi_progress_is_done(const char* poi_id) {
    if (!poi_id || !*poi_id) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_done.find(poi_id) != g_done.end();
}

void poi_progress_counts(const char* kind, int* done_out, int* total_out) {
    int total = 0;
    int done  = 0;
    const auto& pois = pois_get();
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& p : pois) {
        if (std::strcmp(p.kind, kind) != 0) continue;
        ++total;
        if (p.id[0] && g_done.find(p.id) != g_done.end()) ++done;
    }
    if (done_out)  *done_out  = done;
    if (total_out) *total_out = total;
}

}  // namespace farever
