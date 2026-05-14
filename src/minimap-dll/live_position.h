#pragma once

namespace farevermod {

// Live player position from research/live_position.json (written every
// 100 ms by tools/find_hero.py loop). Updated on a background thread
// inside the DLL.
struct LivePosition {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double rot_z = 0.0;
    double ts = 0.0;
    bool   valid = false;
};

// Start the background poller. Returns immediately.
void live_position_start();
void live_position_stop();

// Thread-safe snapshot of the most recent reading.
LivePosition live_position_get();

}  // namespace farevermod
