// Boss speedrun timer. Worker-thread state machine fed by the damage pipeline
// (an outgoing hit on an ent.boss.* target) and the boss entity's combat flags.

#include "boss_timer.h"
#include "boss_names.h"
#include "hero_state.h"
#include "mem_scan.h"
#include "log.h"
#include "user_data.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace farever {
namespace {

// ent.Foe / ent.Unit offsets (tools/classes_v017.json).
constexpr std::size_t OFF_FOE_REMOVED      = 8;     // u8 (removed, unchanged)
constexpr std::size_t OFF_FOE_DYING        = 560;   // u8 (ent.Unit.dying, was 552 [v023 +8])
constexpr std::size_t OFF_FOE_ISINCOMBAT   = 688;   // u8 was 680 [v023 +8]
constexpr std::size_t OFF_FOE_COMBAT_START = 704;   // f64 was 696 [v023 +8]
constexpr std::size_t OFF_FOE_COMBAT_END   = 712;   // f64 was 704 [v023 +8]
constexpr std::size_t OFF_FOE_DEATHREQ     = 1112;  // u8 deathRequested was 1104 [v023 +8]

constexpr double kRunStaleSeconds = 600.0;          // safety abort cap
constexpr double kMaxPlausibleRun = 3600.0;         // game-clock sanity bound

std::atomic<bool> g_enabled{false};

std::mutex g_mu;   // guards everything below

// pending hit handed over from the damage path
std::uintptr_t g_pending_ptr = 0;
std::string    g_pending_name;          // display name
std::uintptr_t g_pending_kill = 0;      // boss ptr whose hit carried is_kill

// active run
BossRunStatus  g_status   = BossRunStatus::Idle;
std::uintptr_t g_boss_ptr  = 0;
std::uintptr_t g_boss_type = 0;          // hl_type tag captured at engage
std::string    g_boss_name;              // display name
bool           g_player_died = false;    // latched: hero died during this run (#65)
std::chrono::steady_clock::time_point g_t0_wall;
double         g_t0_game  = 0.0;         // combatStartTime at engage
double         g_elapsed  = 0.0;

// last clear (for the idle result line)
std::string    g_last_boss;
double         g_last_clear = 0.0;
bool           g_last_was_pb = false;

// session counters (RAM only, process lifetime) + PB-highlight timing
int            g_session_kills = 0;
double         g_session_fastest_s = 0.0;
std::chrono::steady_clock::time_point g_last_clear_wall;
bool           g_have_last_clear = false;

// per-character records
std::map<std::string, BossRecord> g_records;
std::string    g_profile_key;
std::wstring   g_path;                    // boss_times__<key>.json (empty = none)

void persist_locked() {
    if (g_path.empty()) return;
    std::ofstream f(g_path, std::ios::binary | std::ios::trunc);
    if (!f) { logf("boss_timer: cannot write record file"); return; }
    // One JSON object per line (NDJSON), trivial to parse back.
    for (const auto& kv : g_records) {
        const BossRecord& r = kv.second;
        f << "{\"name\":\"" << r.name << "\",\"best_s\":" << r.best_s
          << ",\"kills\":" << r.kills << ",\"deaths\":" << r.deaths
          << ",\"sum_s\":" << r.sum_s << ",\"worst_s\":" << r.worst_s
          << ",\"last_s\":" << r.last_s << ",\"recent\":[";
        for (std::size_t i = 0; i < r.recent.size(); ++i) {
            if (i) f << ",";
            f << r.recent[i];
        }
        f << "]}\n";
    }
}

void load_locked() {
    g_records.clear();
    if (g_path.empty()) return;
    std::ifstream f(g_path, std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto np = line.find("\"name\":\"");
        if (np == std::string::npos) continue;
        np += 8;
        auto ne = line.find('"', np);
        if (ne == std::string::npos) continue;
        std::string name = line.substr(np, ne - np);
        if (name.empty()) continue;
        BossRecord r;
        r.name = name;
        auto bp = line.find("\"best_s\":");
        if (bp != std::string::npos) r.best_s = std::atof(line.c_str() + bp + 9);
        auto kp = line.find("\"kills\":");
        if (kp != std::string::npos) r.kills = std::atoi(line.c_str() + kp + 8);
        auto dp = line.find("\"deaths\":");
        if (dp != std::string::npos) r.deaths = std::atoi(line.c_str() + dp + 9);
        auto sp = line.find("\"sum_s\":");
        if (sp != std::string::npos) r.sum_s = std::atof(line.c_str() + sp + 8);
        auto wp = line.find("\"worst_s\":");
        if (wp != std::string::npos) r.worst_s = std::atof(line.c_str() + wp + 10);
        auto lp = line.find("\"last_s\":");
        if (lp != std::string::npos) r.last_s = std::atof(line.c_str() + lp + 9);
        auto rp = line.find("\"recent\":[");
        if (rp != std::string::npos) {
            std::size_t b = rp + 10;
            std::size_t e = line.find(']', b);
            if (e != std::string::npos) {
                std::string arr = line.substr(b, e - b);
                std::size_t pos = 0;
                while (pos < arr.size()) {
                    std::size_t comma = arr.find(',', pos);
                    std::string tok = (comma == std::string::npos)
                        ? arr.substr(pos)
                        : arr.substr(pos, comma - pos);
                    if (!tok.empty()) r.recent.push_back(std::atof(tok.c_str()));
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        }
        g_records[name] = r;
    }
}

double wall_now_minus(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

void boss_timer_start() {
    logf("boss_timer: ready (opt-in; fed by the damage pipeline)");
}

void boss_timer_set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_release);
    if (!on) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = BossRunStatus::Idle;
        g_boss_ptr = 0;
        g_pending_ptr = 0;
        g_pending_kill = 0;
    }
}

bool boss_timer_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void boss_timer_on_boss_hit(std::uintptr_t boss_ptr, const char* class_name,
                            bool is_kill) {
    if (!boss_timer_enabled() || !boss_ptr || !class_name) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending_ptr  = boss_ptr;
    g_pending_name = boss_display_name(class_name);
    if (is_kill) g_pending_kill = boss_ptr;
}

void boss_timer_set_profile(const char* key) {
    if (!key) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_profile_key == key) return;
    g_profile_key = key;
    std::wstring keyw(key, key + std::strlen(key));   // key is ASCII-safe
    std::wstring rel = L"boss_times__" + keyw + L".json";
    g_path = user_data_path(rel.c_str());
    load_locked();
    // Character switched mid-run: drop any active run, it isn't this char's.
    g_status = BossRunStatus::Idle;
    g_boss_ptr = 0;
    g_pending_ptr = 0;
    g_pending_kill = 0;
    logf("boss_timer: profile '%s' (%zu records)", key, g_records.size());
}

void boss_timer_tick() {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_mu);

    // 1. Engage: consume a pending boss hit while idle.
    if (g_status == BossRunStatus::Idle && g_pending_ptr) {
        std::uintptr_t bp = g_pending_ptr;
        g_pending_ptr = 0;
        std::uint8_t incombat = 0, dying = 0;
        double cstart = 0.0;
        std::uint64_t type = 0;
        if (mem_is_userland(bp) &&
            mem_read_u64(bp, &type) &&
            mem_read_u8(bp + OFF_FOE_ISINCOMBAT, &incombat) &&
            mem_read_u8(bp + OFF_FOE_DYING, &dying) &&
            mem_read_bytes(bp + OFF_FOE_COMBAT_START, &cstart, sizeof(cstart)) &&
            incombat && !dying) {
            g_boss_ptr  = bp;
            g_boss_type = static_cast<std::uintptr_t>(type);
            g_boss_name = g_pending_name;
            g_t0_wall   = std::chrono::steady_clock::now();
            g_t0_game   = cstart;
            g_elapsed   = 0.0;
            g_player_died = false;
            g_status    = BossRunStatus::Running;
            logf("boss_timer: engaged %s", g_boss_name.c_str());
        }
    }

    if (g_status != BossRunStatus::Running) return;

    // 2. Live elapsed (wall clock).
    g_elapsed = wall_now_minus(g_t0_wall);

    // 3. Player abort: died / lock lost / stalled -> discard, no record.
    HeroSnapshot h = hero_state_read();
    // A real death drops the hero to 0 HP. The `dying`@544 flag only covers the
    // downed / bleeding-out state, not an outright death (Crabgantua kills you
    // clean, dying stays 0), so HP is the signal that matters. v1.1.3 also
    // required max_health > 0, but the live diagnostic (#65) showed Hero.attr
    // `health` is live and correctly hits 0 on death while `maxHealth` reads 0
    // from the inline attr block (max health is a computed stat that lives in
    // the attributes map, not inline). That bogus guard nullified the whole
    // check, so a clean death fell through to the boss-removed path and was
    // miscounted as a kill + PB. `attr_ok` already gates out the unresolved-attr
    // case, so health <= 0 with attr_ok is a sound death signal on its own.
    // Latched for the run so a death-then-respawn before the boss tears down
    // still blocks the kill credit.
    bool player_dead = h.dying || (h.attr_ok && h.health <= 0.0);
    if (player_dead) g_player_died = true;
    if (!h.locked || player_dead || g_elapsed > kRunStaleSeconds) {
        if (h.locked && player_dead && !g_boss_name.empty()) {
            // Died to the boss: a real attempt, count the death. Gated on
            // h.locked so a zone transition (lock lost, possibly stale flags)
            // is not miscounted as a death.
            BossRecord& r = g_records[g_boss_name];
            r.name = g_boss_name;
            r.deaths += 1;
            persist_locked();
            logf("boss_timer: death to %s (deaths=%d)",
                 g_boss_name.c_str(), r.deaths);
        } else {
            logf("boss_timer: run discarded (locked=%d dying=%d hp=%.0f "
                 "elapsed=%.1f)",
                 (int)h.locked, (int)h.dying, h.health, g_elapsed);
        }
        g_status = BossRunStatus::Idle;
        g_boss_ptr = 0;
        return;
    }

    // 4. Validate the boss pointer (type-tag; zone change / GC reuse).
    std::uint64_t type = 0;
    if (!mem_read_u64(g_boss_ptr, &type) ||
        static_cast<std::uintptr_t>(type) != g_boss_type) {
        g_status = BossRunStatus::Idle;
        g_boss_ptr = 0;
        return;
    }

    // 5. Read combat flags + death candidates and drive the outcome.
    std::uint8_t incombat = 0, dying = 0, deathreq = 0, removed = 0;
    double cend = 0.0;
    mem_read_u8(g_boss_ptr + OFF_FOE_ISINCOMBAT, &incombat);
    mem_read_u8(g_boss_ptr + OFF_FOE_DYING, &dying);
    mem_read_u8(g_boss_ptr + OFF_FOE_DEATHREQ, &deathreq);
    mem_read_u8(g_boss_ptr + OFF_FOE_REMOVED, &removed);
    mem_read_bytes(g_boss_ptr + OFF_FOE_COMBAT_END, &cend, sizeof(cend));

    // Death signal: the damage-pipeline kill flag (reliable when we land the
    // killing blow) OR the boss entity's own death flags. removed@8 is the one
    // confirmed to flip on a boss death regardless of who lands the final hit
    // (verified live: kill=1 dying=0 deathReq=0 removed=1), which is what makes
    // the "counts if you participated" group case work.
    bool pending_kill = (g_pending_kill == g_boss_ptr);
    g_pending_kill = 0;                       // consume
    // pending_kill (we landed the finishing blow) is unambiguous. The boss
    // entity flags (removed/dying/deathReq) ALSO fire when the encounter is
    // torn down because we died, so they only count as a kill when the player
    // did not die this run (#65: dying on a boss was logged as a kill + PB).
    bool boss_gone = (removed != 0) || (dying != 0) || (deathreq != 0);
    bool killed    = pending_kill || (boss_gone && !g_player_died);

    if (killed) {
        double clear_s = cend - g_t0_game;
        if (!(clear_s > 0.0 && clear_s < kMaxPlausibleRun)) clear_s = g_elapsed;
        BossRecord& r = g_records[g_boss_name];
        r.name = g_boss_name;
        r.kills += 1;
        bool pb = (r.best_s <= 0.0) || (clear_s < r.best_s);
        if (pb) r.best_s = clear_s;
        r.sum_s += clear_s;
        r.last_s = clear_s;
        if (clear_s > r.worst_s) r.worst_s = clear_s;
        r.recent.push_back(clear_s);
        if (r.recent.size() > 5) r.recent.erase(r.recent.begin());
        g_session_kills += 1;
        if (g_session_fastest_s <= 0.0 || clear_s < g_session_fastest_s)
            g_session_fastest_s = clear_s;
        g_last_clear_wall = std::chrono::steady_clock::now();
        g_have_last_clear = true;
        g_last_boss   = g_boss_name;
        g_last_clear  = clear_s;
        g_last_was_pb = pb;
        persist_locked();
        logf("boss_timer: CLEAR %s %.2fs%s (kills=%d) "
             "[kill=%d dying=%u deathReq=%u removed=%u]",
             g_boss_name.c_str(), clear_s, pb ? " NEW PB" : "", r.kills,
             (int)pending_kill, dying, deathreq, removed);
        g_status = BossRunStatus::Idle;
        g_boss_ptr = 0;
        return;
    }
    if (!incombat) {                       // combat ended without a death signal
        logf("boss_timer: run discarded (combat end) %s "
             "[dying=%u deathReq=%u removed=%u]",
             g_boss_name.c_str(), dying, deathreq, removed);
        g_status = BossRunStatus::Idle;
        g_boss_ptr = 0;
        return;
    }
    // still Running: g_elapsed already updated.
}

BossTimerSnapshot boss_timer_snapshot() {
    BossTimerSnapshot s;
    s.enabled = g_enabled.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(g_mu);
    s.status       = g_status;
    s.current_boss = g_boss_name;
    s.elapsed_s    = g_elapsed;
    if (!g_boss_name.empty()) {
        auto it = g_records.find(g_boss_name);
        if (it != g_records.end() && it->second.best_s > 0.0) {
            s.current_pb_s = it->second.best_s;
            s.has_pb = true;
        }
    }
    s.last_boss    = g_last_boss;
    s.last_clear_s = g_last_clear;
    s.last_was_pb  = g_last_was_pb;
    s.records.reserve(g_records.size());
    for (const auto& kv : g_records) s.records.push_back(kv.second);
    s.session_kills     = g_session_kills;
    s.session_fastest_s = g_session_fastest_s;
    s.last_clear_age_s  = g_have_last_clear ? wall_now_minus(g_last_clear_wall)
                                            : 1e9;
    return s;
}

void boss_timer_reset(const char* display_name) {
    if (!display_name) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_records.erase(display_name);
    if (g_last_boss == display_name) {
        g_last_boss.clear();
        g_last_clear = 0.0;
        g_last_was_pb = false;
        g_have_last_clear = false;
    }
    persist_locked();
    logf("boss_timer: reset record '%s'", display_name);
}

void boss_timer_reset_all() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_records.clear();
    g_last_boss.clear();
    g_last_clear = 0.0;
    g_last_was_pb = false;
    g_have_last_clear = false;
    g_session_kills = 0;
    g_session_fastest_s = 0.0;
    persist_locked();
    logf("boss_timer: reset ALL records");
}

}  // namespace farever
