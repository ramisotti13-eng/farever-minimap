#pragma once
#include <windows.h>

namespace farever {

// Run the full anti-cheat gate once, before any injection. `self` is this
// DLL's module handle (used to locate data/ next to the DLL). Returns true if
// it is safe to inject; false if a signal tripped - on a trip it has already
// shown the MessageBox, logged the reason, and set the disabled latch.
bool anticheat_gate(HMODULE self);

// Spawn the early signature re-scan worker: a detached thread that re-runs the
// signature scan a few times over the first ~60 s (an AC client can load
// slightly after game start). Call AFTER injection is set up. If a later scan
// trips it sets the latch + shows the box once; it never uninstalls hooks
// (issue #8) - the render/read paths simply go inert.
void anticheat_start_rescans(HMODULE self);

// The kill-latch. overlay_render() and the hl_pump tick query this and become
// no-ops when it is true.
bool anticheat_is_disabled();

}  // namespace farever
