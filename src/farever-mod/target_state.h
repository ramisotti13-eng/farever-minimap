#pragma once

#include "libhl.h"

#include <cstdint>
#include <string>

namespace farever {

// Slice 1 (v0.5.4 prep): tracks ent.Hero.target each frame and
// exposes its identity to plugins. We chase Hero.target (a single
// pointer), type-anchor it as ent.Unit (currently ent.Hero or
// ent.Foe; ent.vehicle.Vehicle is intentionally not in the anchor
// set yet) and read its `kind` string. Localized display name comes
// later via the `inf` virtual; for now `kind` is the internal ID.
//
// Read on the render thread, mutex-published like HeroSnapshot.
struct TargetSnapshot {
    bool        exists;          // Hero.target chased + type-anchored OK
    std::string kind;            // ent.Unit.kind (internal id, may be empty)

    // Slice 2: world position + level + HP. Same UnitAttributes block
    // we use for Hero — health at idx 25, max_health at idx 26.
    double      x, y, z;
    int         level;
    bool        attr_ok;         // chased to Unit.attr successfully
    double      health;
    double      max_health;
    // Slice 4 (v0.5.6 damage planner): target's defense surface.
    // UnitAttributes.armor @ idx 21 (offset 216 -> idx (216-48)/8=21),
    // magic_armor @ idx 22 (224), magic_reduction @ idx 23 (232).
    double      armor;
    double      magic_armor;
    double      magic_reduction;

    // Slice 3: active-skill cast. is_casting is true while the
    // target has a non-null runningCtx on a non-auto-attack skill.
    // cast_elapsed_sec is measured by our own wall clock from the
    // moment we first observed the cast — for foes the engine
    // doesn't fill castProgress / stopTime so we can't read them
    // directly. cast_total_sec is populated from a per-skill
    // duration cache (filled the first time we see the same skill
    // complete a cast) and is 0 until then.
    bool        is_casting;
    std::string cast_skill;
    double      cast_elapsed_sec;
    double      cast_total_sec;
    double      cast_remaining_sec;
    double      cast_progress;       // elapsed / total, 0 when total unknown
};

// Resolve hl_int64_map_get from libhl. Must be called once before
// target_state_tick is invoked; without it the autoTarget/lockedTarget
// UID fields cannot be resolved to object pointers.
void target_state_init(const LibHL& libhl);

void target_state_tick();        // Present-hook driver
TargetSnapshot target_state_read();

// Raw pointer to the validated target Unit (0 if none). Slices 2+
// will use this from the same render thread to chase attr/skills.
std::uintptr_t target_state_locked_ptr();

}  // namespace farever
