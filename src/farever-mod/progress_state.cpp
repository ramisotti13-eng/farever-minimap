// Walks the Hero → Player → Progress chain so we can read the
// player's collected-elements set. Phase 2A: pure diagnostic logging
// to confirm the offsets work end-to-end. Once verified we'll layer
// on map iteration / lookup.
//
// Chain:
//   ent.Hero @hero_ptr
//     +16  ownerPlayer -> st.Player
//                          +208 progress -> st.player.Progress
//                                            +112 activities -> hxbit.MapData
//                                            +120 elements   -> hxbit.MapData
//                                            +168 zones      -> hxbit.MapData
//                                            ...
// Each MapData wraps:
//   +8  obj  HVIRTUAL[1 field]
//   +16 bit  HI32         (we suspect: live count of map entries)
//   +40 map  HVIRTUAL[9]  (the actual Haxe Map<String, V>)

#include "progress_state.h"
#include "mem_scan.h"
#include "log.h"

#include <atomic>
#include <mutex>
#include <string>

namespace farever {
namespace {

constexpr std::size_t OFF_HERO_OWNERPLAYER  = 16;    // unchanged
constexpr std::size_t OFF_PLAYER_PROGRESS   = 240;   // st.Player.progress (was 216) [v018 +16]
constexpr std::size_t OFF_PROGRESS_ACTIVITIES = 144; // was 136 [v024 +8]
constexpr std::size_t OFF_PROGRESS_ELEMENTS   = 152; // was 144 [v024 +8]
constexpr std::size_t OFF_PROGRESS_ZONES      = 200; // was 192 [v024 +8]
constexpr std::size_t OFF_PROGRESS_ACHIEVS    = 208; // was 200 [v024 +8]

constexpr std::size_t OFF_MAPDATA_BIT = 16;          // hxbit.MapData (unchanged)
constexpr std::size_t OFF_MAPDATA_MAP = 40;          // hxbit.MapData (unchanged)

ProgressSnapshot g_snap;
std::mutex       g_mu;

// Read a pointer field, return 0 on failure / not-userland.
std::uintptr_t read_ptr(std::uintptr_t base, std::size_t off) {
    std::uint64_t v = 0;
    if (!mem_read_u64(base + off, &v)) return 0;
    if (!mem_is_userland((std::uintptr_t)v)) return 0;
    return (std::uintptr_t)v;
}

void log_mapdata(const char* label, std::uintptr_t md) {
    if (!md) { logf("progress: %s = NULL", label); return; }
    std::int32_t bit = 0;
    mem_read_i32(md + OFF_MAPDATA_BIT, &bit);
    std::uintptr_t vvirt = read_ptr(md, OFF_MAPDATA_MAP);
    logf("progress: %-12s MapData @0x%llx  bit=%d  vvirt=0x%llx",
         label,
         (unsigned long long)md,
         bit,
         (unsigned long long)vvirt);

    // vvirtual layout in HashLink:
    //   +0  hl_type*  t
    //   +8  vdynamic* value      <- the wrapped object (StringMap, etc.)
    //   +16 vvirtual* next
    // Followed by inline cached field slots. Reading value gives us
    // the concrete map object (haxe.ds.StringMap etc.).
    if (!vvirt) return;
    std::uintptr_t v_type  = read_ptr(vvirt, 0);
    std::uintptr_t v_value = read_ptr(vvirt, 8);
    logf("progress: %-12s   vvirt.t=0x%llx vvirt.value=0x%llx",
         label,
         (unsigned long long)v_type,
         (unsigned long long)v_value);

    // If value points at a haxe.ds.StringMap, +8 is its `h:
    // hl_bytes_map` - the actual hash table we want to walk.
    if (!v_value) return;
    std::uintptr_t sm_h = read_ptr(v_value, 8);
    logf("progress: %-12s   sm.h(hl_bytes_map)=0x%llx",
         label, (unsigned long long)sm_h);

    // Try the canonical hl_bytes_map header layout: count, capacity,
    // mask, isLocked, hashes, keys, values. Dump count + capacity so
    // we can see if it matches a sane number of map entries.
    if (!sm_h) return;
    std::int32_t bm_count = 0, bm_cap = 0;
    mem_read_i32(sm_h + 0, &bm_count);
    mem_read_i32(sm_h + 4, &bm_cap);
    logf("progress: %-12s   bm.count=%d bm.capacity=%d",
         label, bm_count, bm_cap);
}

}  // namespace

void progress_state_inspect(std::uintptr_t hero_ptr) {
    if (!hero_ptr || !mem_is_userland(hero_ptr)) return;

    std::uintptr_t player   = read_ptr(hero_ptr, OFF_HERO_OWNERPLAYER);
    std::uintptr_t progress = read_ptr(player,   OFF_PLAYER_PROGRESS);
    if (!progress) {
        logf("progress: hero=0x%llx player=0x%llx progress=NULL "
             "(constructor not done?)",
             (unsigned long long)hero_ptr,
             (unsigned long long)player);
        return;
    }

    logf("progress: hero=0x%llx player=0x%llx progress=0x%llx",
         (unsigned long long)hero_ptr,
         (unsigned long long)player,
         (unsigned long long)progress);

    log_mapdata("activities",   read_ptr(progress, OFF_PROGRESS_ACTIVITIES));
    log_mapdata("elements",     read_ptr(progress, OFF_PROGRESS_ELEMENTS));
    log_mapdata("zones",        read_ptr(progress, OFF_PROGRESS_ZONES));
    log_mapdata("achievements", read_ptr(progress, OFF_PROGRESS_ACHIEVS));
}

bool progress_state_is_discovered(const char* poi_id) {
    if (!poi_id || !*poi_id) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_snap.ready) return false;
    return g_snap.discovered_ids.find(poi_id) != g_snap.discovered_ids.end();
}

}  // namespace farever
