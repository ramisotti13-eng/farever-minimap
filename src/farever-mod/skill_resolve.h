#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

struct SkillGfx {
    char atlas_filename[96];   // basename only, e.g. "atlas_class_Mage_96PX.png"
    int  x;        // cell index, not pixels (pixel = x * size)
    int  y;
    int  size;     // cell edge length in px (48 / 96 / ...)
    int  width;    // cell count horizontally (default 1)
    int  height;   // cell count vertically   (default 1)
};

void skill_resolve_init(const LibHL& libhl);

// Read BaseSkill.inf.gfx from a HashLink BaseSkill pointer, fill out.
// Returns false if any link in the chain is null / not HL-allocated.
//
// v0.4.17: when called from the hl_pump worker thread we bail out
// (return false) without doing any hl_dyn_getp dispatch — that work
// is not safe off the hxbit thread. damage_tick is expected to call
// skill_resolve_request_deferred in that case and the next
// on_dd_alloc invocation will drain it via
// skill_resolve_pump_in_alloc_context.
bool skill_resolve_query(std::uintptr_t base_skill_ptr, SkillGfx* out);

// Queue a deferred resolve. Single-slot (overwrite-latest) — combat
// produces enough alloc-hook traffic that the pump always catches up.
void skill_resolve_request_deferred(const char* skill_kind,
                                    std::uintptr_t base_skill_ptr);

// Drain the single deferred slot. MUST be called from an alloc-hook
// callback (we know that's a hxbit-safe context). Cheap when nothing
// is queued.
void skill_resolve_pump_in_alloc_context();

// String-keyed cache. skill_resolve_query results are stashed under the
// skill's `kind` string for the overlay to pull at render time without
// touching HashLink memory again.
void skill_resolve_cache(const char* skill_kind, const SkillGfx& gfx);
bool skill_resolve_lookup(const char* skill_kind, SkillGfx* out);

}  // namespace farever
