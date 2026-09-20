#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace farever {

// #96: a skill's effective vs. base cooldown, captured on the resolve thread.
struct SkillCooldown {
    std::string kind;        // st.skill.BaseSkill.kind id
    double      effective;   // Skill.cooldownDuration@456 (all reductions applied)
    double      base;        // inf.cooldown (CDB config value)
    double      charges;     // Skill.charges@416
};

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
// Must be called on a HashLink-registered thread (the render thread is
// fine - same constraint as damage_decode).
bool skill_resolve_query(std::uintptr_t base_skill_ptr, SkillGfx* out);

// String-keyed cache. skill_resolve_query results are stashed under the
// skill's `kind` string for the overlay to pull at render time without
// touching HashLink memory again.
void skill_resolve_cache(const char* skill_kind, const SkillGfx& gfx);

// Display lookup. #105: when a "<skill>_Status" / "_Buff" / "_Shield" / "_Proc"
// kind has no artwork of its own (the game gives most of them none), this falls
// back to the granting skill's icon, which is what a buff bar wants and what
// the game's own UI shows. The granting thing can also be an equipped item
// rather than a skill (trinkets: "PurifiedHeart" -> "PurifiedHeart_Status"),
// which resolves the same way because item icons share this cache.
bool skill_resolve_lookup(const char* skill_kind, SkillGfx* out);

// Strict lookup, no status fallback. Use this to decide whether a kind still
// needs resolving: the fallback above would otherwise mask a status that DOES
// have its own icon and stop it from ever being requested.
bool skill_resolve_lookup_exact(const char* skill_kind, SkillGfx* out);

// v0.6.0 phase 2c: when skill_resolve_query bails from the worker
// (worker-thread guard), call this to queue a deferred resolve. The
// pending request is drained from alloc-context by
// skill_resolve_pump_in_alloc_context, which runs the actual
// hl_dyn_getp chain safely on the game's main/hxbit thread.
void skill_resolve_request_deferred(const char* skill_kind,
                                    std::uintptr_t base_skill_ptr);

// #105: same for an st.Item pointer. Items carry the identical CDB record
// shape (inf.gfx), and equipped items are what grant the trinket statuses that
// have no artwork of their own - see skill_resolve_lookup's fallback. The
// result lands in the same cache under the item's kind.
void skill_resolve_request_item(const char* item_kind,
                                std::uintptr_t item_ptr);

// #104: the skill kinds that currently have a resolved icon, sorted. Lets a
// plugin discover the exact ids skill_resolve_lookup answers for (they are the
// game's internal BaseSkill.kind strings, not display names). Pure cache read,
// safe from any thread.
std::vector<std::string> skill_resolve_cached_kinds();

// Drains pending deferred requests (a few per call). Caller must NOT be the
// hl_pump worker; any thread the game itself dispatches on is fine. Two such
// call sites exist:
//   - the ui.comp.DamageDisplay alloc watcher (the thread that called
//     hl_alloc_obj, i.e. the game's main/hxbit thread), and
//   - damage_tick, which runs from the Present hook and already drives
//     skill_resolve_query on every damage event.
// #104: the alloc watcher alone was not enough - it only fires in combat, so
// icons for skills you had not just hit something with never resolved.
void skill_resolve_pump_on_game_thread();

// #107: the two CDB-record reads above, exposed for callers that key their own
// cache instead of the skill cache. `inf_offset` is where the object stores its
// CDB record (st.Activity.inf, st.Item.inf, ...). Both are guarded the same way
// (the record must structurally be an hl virtual, and the dyn calls run under
// SEH), and both refuse to run on the hl_pump worker - call them from an
// hl_alloc_obj watcher callback.
// `field` should be one whose hash is precomputed in skill_resolve_init
// ("inherit", "id") - anything else hashes on the fly, which can allocate and
// is therefore not safe to call from alloc-watcher context.
bool cdb_gfx_at(std::uintptr_t obj, std::size_t inf_offset, SkillGfx* out);
bool cdb_inf_string(std::uintptr_t obj, std::size_t inf_offset,
                    const char* field, char* out, std::size_t cap);

// #96: global (gear) cooldown-reduction estimate + per-skill cooldown snapshot,
// both populated as skills resolve. Safe from any thread (pure cache reads).
double skill_global_cooldown_reduction();
std::vector<SkillCooldown> skill_cooldown_snapshot();

}  // namespace farever
