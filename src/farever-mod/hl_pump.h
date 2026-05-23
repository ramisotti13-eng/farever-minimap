#pragma once

#include "libhl.h"

namespace farever {

// v0.4.17 backport of the v0.6.0 HL-GC-invisible worker thread.
//
// Starts a plain Win32 thread that does NOT call hl_register_thread /
// hl_blocking. It's invisible to the HashLink GC: stop-the-world sync
// on game-managed threads won't wait for it, and the GC won't pause
// it on collection. We use this thread to drive damage_tick and
// hero_state_tick at 20 Hz, off the render thread.
//
// In v0.4.x this is OPT-OUT via data/no_worker.flag. When the flag
// is present we fall back to the v0.4.16 Present-driven path (still
// usable on AMD MPO + old-Windows users where DCOMP isn't possible).
//
// Safety contract: this thread only READS game memory through SEH-
// wrapped mem_read_* accessors. It never calls hl_dyn_getp /
// hl_hash_utf8 — those would race the hxbit deserialiser. The
// deferred skill_resolve path queues such work and drains it from
// the on_dd_alloc context (which is the game's main/hxbit thread).

bool hl_pump_start(const LibHL& libhl);
void hl_pump_stop();
bool hl_pump_is_active();

// Returns the OS thread id of the worker (or 0 if not running). Used
// by skill_resolve_query to detect when it's being called from the
// worker and switch to the deferred path.
unsigned long hl_pump_worker_tid();

}  // namespace farever
