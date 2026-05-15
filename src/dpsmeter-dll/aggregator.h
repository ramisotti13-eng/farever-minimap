#pragma once

#include <cstddef>
#include <cstdint>

namespace dpsmeter {

// One row in the live DPS table, grouped by skill name.
struct SkillRow {
    char         skill[64];
    std::int32_t hit_count;     // sum of DR.hitCount across all events
    double       total;         // sum of damage
    double       max_hit;       // largest single event
    std::int32_t crit_count;
};

constexpr std::size_t kMaxRows = 64;

// Frame snapshot for the overlay. Callable cheaply from the render
// thread; the aggregator's running state is updated by an internal
// tick on the same thread (single-threaded model — render thread is
// the only consumer of damage events and the only writer of state).
struct AggSnapshot {
    bool         locked;          // hero locked (combat fields readable)
    bool         in_combat;
    bool         scanning_ready;  // damage_scan has a type tag
    std::int32_t fight_id;
    double       elapsed_sec;     // wall-clock since fight start
    double       total_damage;
    double       dps;
    std::size_t  row_count;
    SkillRow     rows[kMaxRows];  // sorted by total desc, truncated
};

// Drain pending damage events, fold them into the current fight,
// honour combatId rollover (new fight resets the breakdown). Should be
// called once per frame from the Present hook. Cheap.
void aggregator_tick();

// Lock-free read of the current snapshot. Returns the latest fully-
// populated frame snapshot.
AggSnapshot aggregator_snapshot();

// Manual reset (e.g. user pressed a key in the overlay).
void aggregator_reset();

}  // namespace dpsmeter
