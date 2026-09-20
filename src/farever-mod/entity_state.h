#pragma once

#include <cstdint>

namespace farever {

// Tracks live in-world interactible entities (Chests, Gatherables,
// Obelisks, …) so the POI filter can hide ones the player has
// already opened / collected. State is sourced from the entity's
// own `stateId` field - same value the game uses to decide which
// visual to render.
void entity_state_start();

// Drained once per frame from the Present hook (same thread as
// damage_tick / hero_state_tick). Re-reads stateId + pos on every
// tracked entity.
void entity_state_tick();

// True if any tracked entity within `radius_m` meters of (x, y) is
// in a "consumed" state. POI filter uses this to dim / hide the
// matching world-map marker.
bool entity_state_is_collected_at(double x, double y,
                                  double radius_m = 5.0);

}  // namespace farever
