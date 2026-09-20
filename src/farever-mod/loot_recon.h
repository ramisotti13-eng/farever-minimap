#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

// Atlas Loot, Stage 1: recon-only.
//
// Hooks a handful of named boss classes (ent.boss.*). For each Unit
// allocation we wait a few seconds for the constructor to settle,
// then walk the CDB chain:
//
//   foe + 640         -> inf      (HVIRTUAL[21], the CDB Unit row) [v023;
//                                NOTE: NOT the same offset as Chest.inf@632]
//     dyn_getp("id")        -> Haxe String      (boss id)
//     dyn_getp("props")     -> HVIRTUAL[14]     (loot pointers)
//       dyn_getp("lootTable")     -> LootTable obj
//         dyn_getp("id")             -> Haxe String   (lt id)
//         dyn_getp("loot")           -> hl.types.ArrayObj
//           length @ +8, varray @ +16, data @ varray+24
//           each entry: vvirtual* with proba (f64), item (vvirtual*),
//                       itemMin (i32), itemMax (i32)
//
// The first successful walk per boss-id is logged and the boss is
// removed from further tracking. Steady-state cost is one
// unordered_set lookup per allocation.

void loot_recon_start(const LibHL& libhl);
void loot_recon_stop();

// Drive the settling / decode logic. Called once per Present from the
// render thread (same thread as damage_tick / hero_state_tick).
void loot_recon_tick();

}  // namespace farever
