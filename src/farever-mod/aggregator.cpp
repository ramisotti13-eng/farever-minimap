// DamageEvent → SkillRow aggregator. Single-threaded; everything runs
// on the render thread alongside damage_tick. No combat-state tracking
// in this build — fight starts at the first damage event and runs
// until F9. (Hero pointer tracking via the alloc-hook landed in a
// follow-up commit; once we read Hero.combat_id we can auto-segment.)

#include "aggregator.h"
#include "damage.h"
#include "hero_state.h"
#include "log.h"
#include "plugins.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

namespace farever {
namespace {

struct Fight {
    bool   have_first_damage = false;
    DWORD  first_damage_tick = 0;
    DWORD  last_damage_tick  = 0;
    std::unordered_map<std::string, SkillRow> rows;
};

// We're out of combat once *both* signals say so for this long:
//   - no damage event for kDamageIdleMs
//   - Hero.isInCombat is false (or no hero lock)
// Either signal alone keeps the fight alive. That covers blocking /
// dodging without dealing damage, AoE pulls with kill-then-kite, etc.
constexpr DWORD kDamageIdleMs       = 5000;
constexpr DWORD kOutOfCombatGraceMs = 2000;

Fight                     g_fight;
AggSnapshot               g_snapshot{};
std::deque<FightLogEntry> g_history;   // newest at front
int                       g_next_fight_id = 1;
DWORD                     g_out_of_combat_since = 0;  // 0 = currently in combat (or never observed yet)

std::int64_t now_unix_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    std::int64_t t = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) |
                      ft.dwLowDateTime;
    // FILETIME is 100-ns ticks since 1601-01-01; convert to ms since
    // Unix epoch (1970-01-01).
    return (t - 116444736000000000LL) / 10000LL;
}

// Build a FightLogEntry from the active fight and push it to the
// history ring. No-op if the fight had no events. We snapshot every
// SkillRow into the entry so the history-detail view can replay the
// full table later without re-running the aggregator.
void seal_active_fight(double elapsed_sec) {
    if (!g_fight.have_first_damage || g_fight.rows.empty()) return;

    FightLogEntry e{};
    e.id            = g_next_fight_id++;
    e.ended_unix_ms = now_unix_ms();
    e.duration_sec  = elapsed_sec > 0.001 ? elapsed_sec : 0.001;
    e.total_damage  = 0.0;
    e.hit_count     = 0;

    // Pull every skill row out, sort by total desc, copy into the
    // entry. SkillRow array is fixed-size so no allocations here.
    SkillRow temp[kMaxRows];
    std::size_t n = 0;
    for (const auto& kv : g_fight.rows) {
        if (n >= kMaxRows) break;
        temp[n++] = kv.second;
    }
    std::sort(temp, temp + n, [](const SkillRow& a, const SkillRow& b) {
        return a.total > b.total;
    });
    e.row_count = n;
    for (std::size_t i = 0; i < n; ++i) {
        e.rows[i]       = temp[i];
        e.total_damage += temp[i].total;
        e.hit_count    += temp[i].hit_count;
    }
    e.dps = e.total_damage / e.duration_sec;
    if (n > 0) std::strncpy(e.top_skill, temp[0].skill,
                            sizeof(e.top_skill) - 1);

    g_history.push_front(e);
    if (g_history.size() > kFightHistoryMax) g_history.pop_back();

    logf("aggregator: sealed fight #%d (%.1fs %.0f dmg %.1f DPS, "
         "top=%s, %zu skills)",
         e.id, e.duration_sec, e.total_damage, e.dps, e.top_skill,
         e.row_count);

    plugins_emit_fight_end(e.id, e.duration_sec, e.total_damage,
                           e.dps, e.top_skill);
}

void clear_active_fight() {
    g_fight.rows.clear();
    g_fight.have_first_damage = false;
    g_fight.first_damage_tick = 0;
    g_fight.last_damage_tick  = 0;
    g_out_of_combat_since     = 0;
}

// Base-attack chain collapse: "DS_Base_Attack_1/2/3/4" → "DS_Base_Attack".
// Only triggers on names containing "Base_Attack" to leave genuine combo
// skills (e.g. DS_Bladeleaf_Skill1) alone.
void normalise_skill(char* skill) {
    if (!std::strstr(skill, "Base_Attack")) return;
    std::size_t len = std::strlen(skill);
    while (len > 0 && skill[len - 1] >= '0' && skill[len - 1] <= '9') {
        skill[--len] = 0;
    }
    if (len > 0 && skill[len - 1] == '_') skill[--len] = 0;
}

void record(const DamageEvent& ev) {
    char skill[64];
    std::memcpy(skill, ev.skill, sizeof(skill));
    skill[sizeof(skill) - 1] = 0;
    normalise_skill(skill);

    DWORD now = GetTickCount();
    if (!g_fight.have_first_damage) {
        g_fight.have_first_damage = true;
        g_fight.first_damage_tick = now;
        logf("aggregator: combat START (%s)", skill);
        plugins_emit_fight_start(g_next_fight_id);
    }
    g_fight.last_damage_tick = now;

    plugins_emit_damage_dealt(skill, ev.damage,
                              ev.is_crit != 0, ev.is_kill != 0);

    // One DamageEvent == one impact. ev.hit_count is the tick ordinal
    // (channels increment it 1, 2, 3 ...) so we never sum it; we just
    // count each event as a single hit.
    std::string key(skill);
    auto it = g_fight.rows.find(key);
    if (it == g_fight.rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, skill, sizeof(r.skill) - 1);
        r.hit_count  = 1;
        r.total      = ev.damage;
        r.max_hit    = ev.damage;
        r.crit_count = ev.is_crit ? 1 : 0;
        g_fight.rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += 1;
        r.total     += ev.damage;
        if (ev.damage > r.max_hit) r.max_hit = ev.damage;
        if (ev.is_crit) r.crit_count += 1;
    }
}

void publish() {
    AggSnapshot s{};
    s.have_fight      = g_fight.have_first_damage;
    s.scanning_ready  = true;   // hook source has no warm-up to gate on

    DWORD  now     = GetTickCount();
    double elapsed = 0.0;
    double idle    = 0.0;
    if (g_fight.have_first_damage) {
        elapsed = (now - g_fight.first_damage_tick) / 1000.0;
        if (elapsed < 0.001) elapsed = 0.001;
        idle = (now - g_fight.last_damage_tick) / 1000.0;
        if (idle < 0.0) idle = 0.0;
    }

    // Combat-state badge mirrors the seal logic: we're "in combat" if
    // either signal says so.
    {
        HeroSnapshot hs           = hero_state_read();
        bool         damage_recent =
            g_fight.have_first_damage &&
            (now - g_fight.last_damage_tick) < kDamageIdleMs;
        bool         hero_active  = hs.locked && hs.in_combat;
        s.in_combat = damage_recent || hero_active;
    }

    double total = 0.0;
    for (const auto& kv : g_fight.rows) total += kv.second.total;

    s.elapsed_sec  = elapsed;
    s.idle_sec     = idle;
    s.total_damage = total;
    s.dps          = (elapsed > 0.001) ? (total / elapsed) : 0.0;

    SkillRow temp[kMaxRows];
    std::size_t n = 0;
    for (const auto& kv : g_fight.rows) {
        if (n >= kMaxRows) break;
        temp[n++] = kv.second;
    }
    std::sort(temp, temp + n, [](const SkillRow& a, const SkillRow& b) {
        return a.total > b.total;
    });
    s.row_count = n;
    for (std::size_t i = 0; i < n; ++i) s.rows[i] = temp[i];

    s.history_count = g_history.size();
    for (std::size_t i = 0; i < s.history_count; ++i) s.history[i] = g_history[i];

    g_snapshot = s;
}

}  // namespace

void aggregator_tick() {
    constexpr std::size_t kBatch = 64;
    DamageEvent batch[kBatch];
    while (true) {
        std::size_t n = damage_drain(batch, kBatch);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) record(batch[i]);
        if (n < kBatch) break;
    }

    // Auto-seal once we've been *both* damage-idle AND
    // Hero.isInCombat=false continuously for the grace window.
    // Either signal alone keeps the fight open, so we don't drop a
    // legitimate fight just because the player dodged for 5 seconds
    // without dealing damage, and we don't drop it just because the
    // hero flag flickers off briefly during a target swap.
    if (g_fight.have_first_damage) {
        DWORD        now           = GetTickCount();
        HeroSnapshot hs            = hero_state_read();
        bool         damage_idle   =
            (now - g_fight.last_damage_tick) >= kDamageIdleMs;
        bool         hero_idle     = !hs.locked || !hs.in_combat;

        if (!damage_idle || !hero_idle) {
            g_out_of_combat_since = 0;
        } else {
            if (g_out_of_combat_since == 0) g_out_of_combat_since = now;
            if (now - g_out_of_combat_since >= kOutOfCombatGraceMs) {
                double elapsed =
                    (g_fight.last_damage_tick - g_fight.first_damage_tick) / 1000.0;
                logf("aggregator: combat END (damage-idle %ums, "
                     "hero %s) — sealing fight",
                     (unsigned)(now - g_fight.last_damage_tick),
                     hs.locked ? (hs.in_combat ? "in-combat" : "out") : "no-lock");
                seal_active_fight(elapsed);
                clear_active_fight();
            }
        }
    }

    publish();
}

AggSnapshot aggregator_snapshot() { return g_snapshot; }

void aggregator_reset() {
    // F9 — manual reset. If there is data, push it to history so it
    // isn't lost; then clear the live fight.
    if (g_fight.have_first_damage) {
        double elapsed =
            (GetTickCount() - g_fight.first_damage_tick) / 1000.0;
        seal_active_fight(elapsed);
    }
    clear_active_fight();
    publish();
    logf("aggregator: manual reset");
}

}  // namespace farever
