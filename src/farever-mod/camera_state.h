#pragma once

#include <cstdint>

namespace farever {

// Worker-thread reader for the live client.GameCamera yaw (#65). Anchors the
// camera via the hl_alloc_obj watcher (newest alloc wins, so zone-transition
// re-creates are picked up) and publishes the smoothed azimuth (curDirection)
// to atomics the overlay can read without touching HashLink memory.

// Register the client.GameCamera watcher. Call BEFORE hl_hook_install, boot-
// ordering parity with the other state modules.
void camera_state_start();

// Read + publish the current camera yaw. Drive from the hl_pump worker tick.
void camera_state_tick();

// Load the last published camera yaw (radians). Returns false when no valid
// GameCamera is currently anchored, so the overlay falls back to the hero
// heading. No game-memory access; safe to call from the render thread.
bool camera_state_yaw(double& out);

}  // namespace farever
