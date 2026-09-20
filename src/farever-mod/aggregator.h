#pragma once

#include <cstddef>
#include <cstdint>

namespace farever {

struct SkillRow {
    char         skill[64];
    std::int32_t hit_count;
    double       total;
    double       max_hit;
    std::int32_t crit_count;
};

constexpr std::size_t kMaxRows = 64;

// One past fight, sealed when combat ends. Kept in a small ring of the
// 10 most-recent so the user can glance at recent pulls.
struct FightLogEntry {
    int          id;            // monotonic; mostly for stable UI keys
    std::int64_t ended_unix_ms; // GetSystemTimeAsFileTime-derived
    double       duration_sec;
    double       total_damage;
    double       dps;
    double       total_heal;    // #57
    double       hps;           // #57
    std::int32_t hit_count;
    char         top_skill[64]; // highest-total skill of the fight
    std::size_t  row_count;     // per-skill breakdown, sorted total-desc
    SkillRow     rows[kMaxRows];
    std::size_t  heal_row_count;       // #57 follow-up: per-skill heal
    SkillRow     heal_rows[kMaxRows];  // sorted by total heal desc
    double       total_shield;         // #70: shield granted over the fight
    double       sps;                  // #70: shield granted per second
    std::size_t  shield_row_count;     // #70
    SkillRow     shield_rows[kMaxRows];// #70 sorted by total shield desc
};

constexpr std::size_t kFightHistoryMax = 10;

// Frame snapshot consumed by the overlay's DPS table.
struct AggSnapshot {
    bool         scanning_ready;   // damage source has cached the DR type tag
    bool         have_fight;       // at least one event has fired
    bool         in_combat;        // damage seen within the idle timeout
    double       elapsed_sec;      // since first damage event of this pull
    double       idle_sec;         // since last damage event (0 while active)
    double       total_damage;
    double       dps;
    double       hps;              // #57
    double       total_heal;       // #57
    std::size_t  row_count;
    SkillRow     rows[kMaxRows];   // sorted by total desc, truncated
    std::size_t  heal_row_count;   // #57
    SkillRow     heal_rows[kMaxRows];  // #57 sorted by total heal desc
    double       total_shield;     // #70
    double       sps;              // #70 shield granted per second
    std::size_t  shield_row_count; // #70
    SkillRow     shield_rows[kMaxRows];  // #70 sorted by total shield desc

    std::size_t   history_count;            // newest first
    FightLogEntry history[kFightHistoryMax];
};

// Drain new damage events, fold them into the current pull, refresh
// the snapshot. Call once per frame from the render thread (after
// damage_tick - order matters: damage_tick produces events, this
// consumes them).
void aggregator_tick();

// Returns a const reference to the live snapshot (no copy). Render-thread
// only - the snapshot is produced and consumed on the render thread.
const AggSnapshot& aggregator_snapshot();

// Lean scalar accessors (no 73KB copy) for callers that need just one value.
double aggregator_dps();
double aggregator_total_damage();
double aggregator_elapsed_sec();
bool   aggregator_in_combat();

// Manual reset (F9). Clears rows and the elapsed timer.
void aggregator_reset();

}  // namespace farever
