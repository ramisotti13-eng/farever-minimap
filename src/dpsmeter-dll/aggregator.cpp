// DamageEvent → SkillRow aggregator. Single-threaded; lives entirely
// on the render thread (Present hook calls aggregator_tick() per
// frame). That keeps the data path lock-free aside from the tiny
// drain() that damage_scan provides.
//
// Encounter detection: hero.combat_id is a monotonic counter the
// engine bumps on every fight start. We watch it; when it changes, we
// snapshot the previous fight to the log and reset the breakdown.

#include "aggregator.h"
#include "damage_scan.h"
#include "hero_state.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace dpsmeter {
namespace {

// Per-fight state. All mutation happens on the render thread.
struct Fight {
    std::int32_t                                  id        = -1;
    DWORD                                         start_tick = 0;
    bool                                          have_start = false;
    std::unordered_map<std::string, SkillRow>     rows;
};

Fight        g_fight;
AggSnapshot  g_snapshot{};

// Convert g_fight into a sorted, capped AggSnapshot.
void publish() {
    AggSnapshot s{};
    HeroSnapshot hs = hero_state_read();
    s.locked         = hs.valid;
    s.in_combat      = hs.valid && hs.is_in_combat != 0;
    s.scanning_ready = damage_scan_ready();
    s.fight_id       = g_fight.id;

    double elapsed = 0.0;
    if (g_fight.have_start) {
        elapsed = (GetTickCount() - g_fight.start_tick) / 1000.0;
        if (elapsed < 0.001) elapsed = 0.001;
    }

    double total = 0.0;
    for (const auto& kv : g_fight.rows) total += kv.second.total;
    s.total_damage = total;
    s.elapsed_sec  = elapsed;
    s.dps          = (elapsed > 0.001) ? (total / elapsed) : 0.0;

    // Copy + sort by total desc.
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

    g_snapshot = s;
}

void start_fight(std::int32_t id) {
    if (!g_fight.rows.empty()) {
        // Log the previous fight's summary before resetting.
        double total = 0.0;
        for (const auto& kv : g_fight.rows) total += kv.second.total;
        double elapsed = g_fight.have_start
            ? (GetTickCount() - g_fight.start_tick) / 1000.0 : 0.0;
        logf("aggregator: fight #%d ended — total=%.1f, elapsed=%.1fs, "
             "DPS=%.1f, %zu skills",
             g_fight.id, total, elapsed,
             elapsed > 0.001 ? total / elapsed : 0.0,
             g_fight.rows.size());
    }
    g_fight.id         = id;
    g_fight.start_tick = GetTickCount();
    g_fight.have_start = true;
    g_fight.rows.clear();
    logf("aggregator: new fight #%d", id);
}

void record(const DamageEvent& ev) {
    std::string key(ev.skill);
    auto it = g_fight.rows.find(key);
    if (it == g_fight.rows.end()) {
        SkillRow r{};
        std::strncpy(r.skill, ev.skill, sizeof(r.skill) - 1);
        r.hit_count  = (ev.hit_count > 0) ? ev.hit_count : 1;
        r.total      = ev.damage;
        r.max_hit    = ev.damage;
        r.crit_count = ev.is_crit ? 1 : 0;
        g_fight.rows.emplace(std::move(key), r);
    } else {
        SkillRow& r = it->second;
        r.hit_count += (ev.hit_count > 0) ? ev.hit_count : 1;
        r.total     += ev.damage;
        if (ev.damage > r.max_hit) r.max_hit = ev.damage;
        if (ev.is_crit) r.crit_count += 1;
    }
}

}  // namespace

void aggregator_tick() {
    // Encounter detection from hero combatId.
    HeroSnapshot hs = hero_state_read();
    if (hs.valid && hs.combat_id != g_fight.id) {
        start_fight(hs.combat_id);
    }

    // Drain damage events in batches.
    constexpr std::size_t kBatch = 64;
    DamageEvent batch[kBatch];
    while (true) {
        std::size_t n = damage_scan_drain(batch, kBatch);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) record(batch[i]);
        if (n < kBatch) break;
    }

    publish();
}

AggSnapshot aggregator_snapshot() { return g_snapshot; }

void aggregator_reset() {
    g_fight.rows.clear();
    g_fight.start_tick = GetTickCount();
    g_fight.have_start = true;
    publish();
}

}  // namespace dpsmeter
