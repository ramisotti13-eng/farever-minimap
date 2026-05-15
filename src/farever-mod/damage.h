#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

struct DamageEvent {
    std::uintptr_t dr_ptr;        // unique ID for dedupe
    double         damage;
    std::int32_t   hit_count;
    std::uint8_t   is_crit;
    std::uint8_t   is_kill;
    char           skill[64];     // ASCII, NUL-terminated
};

// Register the hl_alloc_obj watcher. The watcher pushes freshly-
// allocated DamageResult pointers into a pending queue at alloc time
// (their fields are still zero — the Haxe constructor runs AFTER
// hl_alloc_obj returns).
//
// `damage_tick()` must then be called once per frame from a
// HashLink-known thread (the d3d12 Present hook): it drains the
// queue, reads damage / hit / crit / skill from each settled DR,
// retries the ones whose constructor hasn't run yet, and emits
// DamageEvents.
void damage_start(const LibHL& libhl);
void damage_stop();

// Drive one pass of the pending → event pipeline. MUST be called from
// the render thread (Present hook) — driving it from a background
// worker thread crashed hxbit's deserialiser in earlier builds. See
// feedback_hashlink_pump_thread.md for the post-mortem.
void damage_tick();

// Pull queued events for the aggregator (called from the render
// thread). Returns count written.
std::size_t damage_drain(DamageEvent* out, std::size_t max);

// Diagnostic counters.
struct DamageStats {
    std::uint64_t allocs_seen;
    std::uint64_t events_emitted;
    std::uint64_t dropped_uninit;     // pending entries that never settled
    std::uint64_t dropped_garbage;    // filtered post-init (bad hitcount/skill)
    std::uintptr_t damage_result_tag;
};
DamageStats damage_stats();

}  // namespace farever
