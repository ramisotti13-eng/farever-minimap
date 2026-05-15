#pragma once

#include <cstdint>

namespace dpsmeter {

// Snapshot of the combat-relevant Hero fields. Filled by hero_state_read.
struct HeroSnapshot {
    bool         valid;          // false until the scan has locked
    std::int32_t combat_id;
    double       combat_start;   // engine time when fight began
    std::uint8_t is_in_combat;
};

// Lifecycle. The scan runs on a background worker (parallel to the
// damage scan); both share the same SEH-wrapped read helpers.
void hero_state_start();
void hero_state_stop();

// Fast read once locked. Returns {valid=false} until the structural
// + isMe + bidirectional check passes. Validation re-runs every 64
// reads — required to survive dungeon transitions (see feedback memory
// "Hero lock needs active re-validation").
HeroSnapshot hero_state_read();

bool hero_state_locked();
bool hero_state_failed();

}  // namespace dpsmeter
