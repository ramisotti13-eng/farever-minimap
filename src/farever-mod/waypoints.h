#pragma once

// Per-world user waypoint store. Mirrors poi_progress.cpp: mutex-guarded
// in-memory state, saved to data/user_waypoints_<world>.json on every
// change. The pure model + JSON live in waypoints_json.h.

#include "waypoints_json.h"

namespace farever {

// Load data/user_waypoints_<world>.json into memory. Missing/malformed ->
// starts empty (logged). Call once at overlay init.
void waypoints_load();

// Add at world (x,y,z). Returns the new id, or 0 if the store is full
// (kMaxWaypoints). name is copied + truncated to 63 chars; nullptr/empty
// becomes "Waypoint". Saves on success.
int waypoints_add(float x, float y, float z, const char* name);

// Remove by id. Returns true if a waypoint was removed. Saves on success.
bool waypoints_remove(int id);

// Rename by id. Returns true if found. name truncated to 63 chars. Saves.
bool waypoints_rename(int id, const char* name);

// Set color + icon by id. Values >= the palette/icon-set size clamp to 0.
// Returns true if a waypoint with that id exists. Saves on success.
bool waypoints_set_style(int id, uint8_t color, uint8_t icon);

// Mark a waypoint as the active "target" the minimap/compass should
// pulse/highlight. id=0 clears. Returns true on success (clear always
// succeeds; setting fails silently if no waypoint has that id).
bool waypoints_set_primary(int id);

// Returns the primary waypoint id (0 if none).
int  waypoints_get_primary();

// Render-thread read of the live vector (no copy). The store is only
// mutated on the render thread, so iterating the returned reference within
// a frame is safe.
const std::vector<Waypoint>& waypoints_get();

}  // namespace farever
