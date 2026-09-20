#pragma once

#include <cstdint>

namespace farever {

// User-managed "I've already done this one" state for the minimap,
// scoped per character (#62). Right-clicking a POI toggles it; persisted
// to data/poi_done__<key>.json so each character keeps its own progress.

// Switch the active character profile. `key` comes from
// poi_sanitize_profile_key(character name). Saves the current set to the
// current file, then loads the new character's file (migrating the legacy
// global poi_done.json into the FIRST character that locks after the
// update). No-op if `key` is already active. Called from the hl_pump tick.
void poi_progress_set_profile(const char* key);

void poi_progress_save();

void poi_progress_toggle(const char* poi_id);
bool poi_progress_is_done(const char* poi_id);

// Set a batch of POI ids to done/not-done in one shot, persisting once. Used
// to mark a whole cluster of overlapping collectibles with a single right-
// click when they sit on top of each other and can't be targeted
// individually (#86).
void poi_progress_set_ids(const char* const* ids, int count, bool done);

// Bulk-set every loaded POI of `kind` to done (`done=true`) or not-done
// (`done=false`) for the active character, then persist once. (#65 "mark all
// collected".) Only affects POIs in the currently loaded world.
void poi_progress_mark_all(const char* kind, bool done);

// Count how many POIs of `kind` are marked done and how many exist
// total in the loaded POI list. Both out-pointers may be null.
void poi_progress_counts(const char* kind, int* done_out, int* total_out);

}  // namespace farever
