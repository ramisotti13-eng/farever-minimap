#pragma once

#include <cstddef>
#include <cstdint>

namespace dpsmeter {

// One damage event, the granularity we deliver to the UI / aggregator.
// `skill` is ASCII, NUL-terminated, max 63 chars (Haxe skill kinds like
// "DS_Bladeleaf_Skill1", "Mage_RayOfSpark").
struct DamageEvent {
    std::uintptr_t dr_ptr;    // DamageResult* — unique ID for dedupe
    double         damage;
    std::int32_t   hit_count;
    std::uint8_t   is_crit;
    std::uint8_t   is_kill;
    char           skill[64];
};

// Start the background scan thread. Idempotent.
//
// Internally: anchors ui.comp.DamageDisplay's hl_type tag (retries
// every 5 s until the world is loaded), then scans the heap ~10 Hz
// with a hot-region cache. New, deduplicated DamageEvents land in an
// internal queue that the UI drains via damage_scan_drain.
void damage_scan_start();

// Stop and join the worker.
void damage_scan_stop();

// True once we have a type tag locked in (i.e. the world is loaded
// and scanning is actually running). The UI can render a "waiting for
// world" message until this flips.
bool damage_scan_ready();

// Pop up to `max` queued events into `out`. Returns number written.
// Lock-protected internally; safe to call from the render thread.
std::size_t damage_scan_drain(DamageEvent* out, std::size_t max);

// Snapshot of internal counters — for the overlay's debug header.
struct ScanStats {
    std::uint64_t poll_count;
    std::uint32_t last_scan_ms;
    std::uint32_t hot_regions;
    std::uint64_t unique_drs;
    std::uintptr_t type_tag;
};
ScanStats damage_scan_stats();

}  // namespace dpsmeter
