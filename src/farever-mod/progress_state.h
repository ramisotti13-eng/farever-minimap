#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace farever {

// Snapshot of the player's progress for the collectibles filter. Read
// from st.Player.progress.elements (and friends) once we have a stable
// Hero lock, cached, refreshed on a slow timer.
struct ProgressSnapshot {
    bool                          ready = false;     // set once we've successfully populated
    std::unordered_set<std::string> discovered_ids;  // element IDs the player has interacted with
};

// Called once when hero_state locks. Reads the Player → Progress
// chain and logs each pointer so we can verify the offsets line up.
void progress_state_inspect(std::uintptr_t hero_ptr);

// Render-time query: is this POI id in the player's discovered set?
// Always returns false until the snapshot is populated.
bool progress_state_is_discovered(const char* poi_id);

}  // namespace farever
