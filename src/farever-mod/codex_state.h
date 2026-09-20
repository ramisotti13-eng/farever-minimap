#pragma once

#include "libhl.h"

#include <cstdint>
#include <vector>

// Issue #44 - player codex read. Surfaces, for a given Unit.kind id
// (exactly what farever.target.name() returns, e.g. "Wolf_Z2W"), the
// player's codex completion for that monster.
//
// How it works (all confirmed by the phase-1 diagnostic, see git
// history of this file):
//   data.CodexData is a STATIC class - its statics live on a module
//   singleton we locate once by heap-scanning for the data.CodexData
//   hl_type* and taking the object whose __type__ slot holds it (the
//   global_value path is null in this HL build). The singleton holds
//   `unitNodes`, a haxe.ds.StringMap<ArrayObj<CodexNode>> keyed by the
//   internal unit id. Lookup chain:
//     unitNodes[kind] -> ArrayObj (len 1) -> [0] = data.CodexNode
//   CodexNode carries completionProgress / maxProgress / completed.
//
// All reads are SEH-wrapped pointer chases of a static singleton that
// is built once at character load and thereafter only mutates int
// progress fields, so lookups are safe from any thread.

namespace farever {

struct CodexEntry {
    int  completion = 0;        // completionProgress (running counter)
    int  max        = 0;        // maxProgress
    bool completed  = false;    // authoritative "entry complete" flag
    const char* state = "unknown";  // "unknown" | "partial" | "complete"
    char kind[128] = {0};       // Unit.kind id, the map key (#105; empty from
                                // codex_state_lookup, which was given the id)
    char name[128] = {0};       // localized display name (e.g. "Wolf")
    char path[256] = {0};       // fullPath in the codex tree
};

// Arms the finder. Call before hl_hook_install (ordering parity with
// the other *_init functions, though this module no longer relies on
// the alloc hook).
void codex_state_init(const LibHL& libhl);

// Worker-tick entry: locates + caches the codex statics singleton, and
// re-finds it if a character reload invalidated the cached pointer.
// Cheap no-op once cached and still valid.
void codex_state_tick();

// Look up a Unit.kind id. Returns false if the codex isn't loaded yet
// or the id isn't a codex monster (caller should surface nil). On true,
// `out` is filled with live progress read fresh from the node.
bool codex_state_lookup(const char* kind, CodexEntry* out);

// #105: every bestiary entry in one pass, sorted by `kind` so the order is
// stable between calls. Empty while the codex has not been located yet. This
// walks the whole StringMap, so treat it as a periodic refresh rather than a
// per-frame call.
std::vector<CodexEntry> codex_state_all();

}  // namespace farever
