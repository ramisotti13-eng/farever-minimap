#pragma once

#include "libhl.h"

#include <cstdint>

namespace farever {

// v0.6.0 phase 2d: hxbit-safe UID -> pointer registry. Built entirely
// from alloc-hook callbacks (game's main/hxbit thread), read by anyone.
// Replaces hl_hi64get for worker-mode target resolution - see
// [[hxbit-uid-resolution]] for why hl_hi64get from a worker races with
// hxbit deserialise.
//
// Strategy mirrors Companion's farever-hero-registry / farever-skill-
// registry: maintain our own (uid -> ptr) map populated as entities
// allocate, look up from worker thread without touching the game's
// NetworkSerializer.refs hl_int64_map.
//
// hxbit fills in __uid (offset 32, HI64) AFTER hl_alloc_obj returns,
// during deserialize. So we capture {ptr, type_ptr} at alloc time into
// a pending ring, and re-read __uid on subsequent alloc-hook firings.
// Once __uid is non-zero we promote to the resolved map. The pump
// runs from on_foe_alloc / on_dd_alloc - both fire on the game's main
// thread, safe to read object memory there.

// Register alloc-hook watchers for ent.Foe (and chain into ent.Hero
// tracking). Call once from dllmain after hl_hook_install. Returns
// false only on bad libhl resolution.
bool uid_registry_init(const LibHL& libhl);

// Call from any hl_alloc_obj watcher callback for an entity whose
// __uid we want to track. Captures (ptr, type_ptr) for later __uid
// fill-in via the pump.
void uid_registry_track(std::uintptr_t obj_ptr, std::uintptr_t type_ptr);

// Drain pending ring: walk pending entries, re-read __uid, promote
// to resolved if filled. Cheap (~10us per call typical). Must be
// called from alloc-hook context for safety (game's main/hxbit
// thread, sequential with hxbit writes to __uid).
void uid_registry_pump_in_alloc_context();

// Look up a UID. Safe to call from any thread including the hl_pump
// worker. Returns 0 if not registered, or if the stored pointer fails
// type-tag verification (GC slot reuse - the Foe died and the slot
// now holds something else). Auto-evicts stale entries on lookup.
std::uintptr_t uid_registry_lookup(std::uint64_t uid);

}  // namespace farever
