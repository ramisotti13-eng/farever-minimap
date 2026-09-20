#pragma once

// #107: learns which atlas cell the game itself uses for each world-activity
// type, instead of the mod guessing.
//
// The minimap draws activities from research/pois_*.json, whose `subkind` is
// the CDB `inherit` value ("FightStone", "WorldElite", ...). Which icon goes
// with which subkind used to be a hand-written table in pois.cpp, and it was
// wrong for several of them (FightStone drew the treasure chest, ChestOrb the
// wagon). The right answer is in the game: st.Activity carries its CDB record
// at `inf`, and `inf.gfx` is exactly {file, x, y, size} into UI/icons/
// activities.png - the same record the game's own map marker reads.
//
// So we read it off live activities as the world streams in, cache it by
// subkind, and persist the cache so a later session has the right icon from
// the first frame. pois.cpp falls back to its built-in table for any subkind
// not learned yet.

#include "libhl.h"

#include <cstddef>

namespace farever {

// Registers the per-activity-class watchers and loads the persisted cache.
void activity_icons_init(const LibHL& libhl);

// Atlas cell for a POI subkind. False if we have not seen one live yet and
// nothing was persisted, in which case the caller keeps its own default.
bool activity_icon_cell(const char* subkind, int* col, int* row);

// Writes the learned cells if any changed since the last write. Cheap no-op
// otherwise; called from the worker tick.
void activity_icons_flush();

}  // namespace farever
