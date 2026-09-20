// Tracks every Chest / Gatherable / Obelisk / Orb the game streams
// into the local heap so we can hide the ones the player has already
// dealt with from the minimap.
//
// Each interactible inherits from ent.GameObject and carries:
//   +8   removed     HBOOL
//   +144 posx        HF64
//   +152 posy        HF64
//   +648 stateId     HOBJ:String     ← "Default" / "Opened" / …
//
// We register an hl_alloc_obj watcher per class. Each tick we re-read
// every tracked entity's pos + stateId and stamp positions whose state
// indicates "consumed". The exact non-default state names are
// game-data-driven, so the diagnostic build also logs the first few
// unique stateIds we observe per kind - once we know the strings we
// can tighten the classifier.

#include "entity_state.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

constexpr std::size_t OFF_GO_REMOVED        = 8;     // unchanged
constexpr std::size_t OFF_GO_POSX           = 176;   // was 152 [v018 +16]
constexpr std::size_t OFF_GO_POSY           = 184;   // was 160 [v018 +16]
constexpr std::size_t OFF_INTERACT_STATEID  = 664;   // Chest.stateId was 656 [v023 +8]
constexpr std::size_t OFF_STR_BYTES         = 8;
constexpr std::size_t OFF_STR_LEN           = 16;

struct Tracked {
    std::uintptr_t ptr;
    const char*    kind;          // "chest", "red_orb", ...  (static literal)
    std::uintptr_t expected_type; // hl_type* captured at alloc time
    int            retries;
    char           last_state[64];  // for change detection
};

struct Discovered {
    double x;
    double y;
    const char* kind;
};

std::mutex                                 g_mu;
std::vector<Tracked>                       g_tracked;
std::vector<Discovered>                    g_discovered;

// Diagnostic: log the first ~40 unique state strings we see per kind.
std::mutex                                 g_seen_mu;
std::unordered_set<std::string>            g_seen_states;

bool read_haxe_ascii(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 64) return false;

    std::uint8_t buf[160];
    std::size_t  nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64), buf, nb))
        return false;

    std::size_t w = 0;
    for (int i = 0; i < length && w + 1 < cap; ++i) {
        std::uint16_t c = (std::uint16_t)buf[i * 2] |
                          ((std::uint16_t)buf[i * 2 + 1] << 8);
        if (c >= 128) return false;
        out[w++] = (char)c;
    }
    out[w] = 0;
    return true;
}

void push_tracked(std::uintptr_t obj, const char* kind,
                  const wchar_t* class_name) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Tracked t{};
    t.ptr           = obj;
    t.kind          = kind;
    t.expected_type = hl_hook_get_type(class_name);
    g_tracked.push_back(t);
    if (g_tracked.size() > 4096) {
        g_tracked.erase(g_tracked.begin(),
                        g_tracked.begin() + (g_tracked.size() - 4096));
    }
}

void on_chest_alloc      (std::uintptr_t o) { push_tracked(o, "chest",   L"ent.interactible.Chest"); }
void on_redorb_alloc     (std::uintptr_t o) { push_tracked(o, "red_orb", L"ent.interactible.Gatherable"); }
void on_obelisk_alloc    (std::uintptr_t o) { push_tracked(o, "obelisk", L"ent.interactible.Obelisk"); }
void on_gatherable_alloc (std::uintptr_t o) { push_tracked(o, "gather",  L"ent.interactible.Gatherable"); }

}  // namespace

void entity_state_start() {
    hl_hook_register(L"ent.interactible.Chest",      on_chest_alloc);
    hl_hook_register(L"ent.interactible.Obelisk",    on_obelisk_alloc);
    hl_hook_register(L"ent.interactible.Gatherable", on_gatherable_alloc);
    // RedOrbs are a kind of Gatherable in Farever; the watcher above
    // already covers them. Keeping a separate hook would just dupe.
    logf("entity_state: watchers registered (Chest, Obelisk, Gatherable)");
}

void entity_state_tick() {
    static std::uint64_t s_ticks = 0;
    ++s_ticks;
    // Throttle the main walk to every 4th frame. Per-frame iteration
    // over hundreds of tracked entities adds up to thousands of mem
    // reads per second on the render thread; the actual state we
    // surface (discovered set, used by the minimap) is sampled, not
    // realtime, so quartering the rate is invisible to the user.
    if ((s_ticks & 0x3) != 0) return;

    std::vector<Tracked> work;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        work.swap(g_tracked);
    }
    std::vector<Tracked>    retry;
    std::vector<Discovered> found;
    retry.reserve(work.size());
    found.reserve(work.size());

    for (Tracked t : work) {
        if (!mem_is_userland(t.ptr)) continue;

        // Type-anchor: drop the pointer if its hl_type at +0 doesn't
        // match the class we registered it as. This is the canonical
        // defense against the Boehm-GC slot-reuse pattern we saw in
        // the dungeon crash - without it, a stale chest pointer that
        // got reused for an h3d buffer reads garbage at offset 648
        // ('stateId') and we eventually trip an AV walking the bad
        // String pointer.
        if (!t.expected_type) {
            // Type wasn't cached at alloc time - try to learn it now
            // (the hl_hook dispatcher may have observed an alloc since).
            // No good fallback so don't drop the entry yet.
        } else {
            std::uint64_t actual = 0;
            if (!mem_read_u64(t.ptr, &actual)) continue;
            if (static_cast<std::uintptr_t>(actual) != t.expected_type) {
                // GC slot reuse - entity isn't what we thought it was.
                // Drop it; a fresh alloc-hook event will give us the
                // new (or same) entity on the next encounter.
                continue;
            }
        }

        std::uint8_t removed = 0;
        if (!mem_read_u8(t.ptr + OFF_GO_REMOVED, &removed)) {
            if (++t.retries < 60) retry.push_back(t);
            continue;
        }
        if (removed) continue;   // entity gone, drop

        double pos[2] = {0, 0};
        if (!mem_read_bytes(t.ptr + OFF_GO_POSX, pos, sizeof(pos))) {
            if (++t.retries < 60) retry.push_back(t);
            continue;
        }

        std::uint64_t state_ptr_u64 = 0;
        if (!mem_read_u64(t.ptr + OFF_INTERACT_STATEID, &state_ptr_u64)) {
            retry.push_back(t);
            continue;
        }
        char state[64] = {0};
        if (state_ptr_u64) {
            read_haxe_ascii((std::uintptr_t)state_ptr_u64,
                            state, sizeof(state));
        }

        // Diagnostic: log every (kind, stateId) pair we haven't seen
        // before so we know which strings to treat as "discovered".
        if (state[0]) {
            std::string key = t.kind;
            key += ':';
            key += state;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lk2(g_seen_mu);
                fresh = g_seen_states.insert(key).second;
            }
            if (fresh) {
                logf("entity_state: first sight of %s state = '%s' "
                     "(at %.1f, %.1f, ptr=0x%llx)",
                     t.kind, state, pos[0], pos[1],
                     (unsigned long long)t.ptr);
            }
        }

        // Per-entity transition log - catches state changes on the
        // same entity (e.g. Closed -> Opened when the player loots).
        if (std::strcmp(state, t.last_state) != 0) {
            if (t.last_state[0]) {
                logf("entity_state: %s @0x%llx (%.1f, %.1f): "
                     "'%s' -> '%s'",
                     t.kind, (unsigned long long)t.ptr,
                     pos[0], pos[1], t.last_state, state);
            }
            std::strncpy(t.last_state, state, sizeof(t.last_state) - 1);
            t.last_state[sizeof(t.last_state) - 1] = 0;
        }

        // Classifier - pessimistic until we've confirmed the
        // strings. "Default" / empty / "Idle" / "Closed" / "Hidden"
        // are treated as "not yet collected"; anything else counts.
        bool is_discovered =
            state[0] != 0 &&
            std::strcmp(state, "Default") != 0 &&
            std::strcmp(state, "Idle")    != 0 &&
            std::strcmp(state, "Closed")  != 0 &&
            std::strcmp(state, "Hidden")  != 0 &&
            std::strcmp(state, "Available") != 0;

        if (is_discovered) {
            found.push_back({pos[0], pos[1], t.kind});
        }

        // Periodic-snapshot diagnostic logging removed. It was bursting
        // 100+ fprintf+fflush calls in a single render frame every
        // ~30 s, which under load could collide with the game's own
        // Present submission. The information was diagnostic only --
        // production state is observable via the first-sight log
        // above plus the transition log.

        retry.push_back(t);
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_tracked    = std::move(retry);
        g_discovered = std::move(found);
    }
}

bool entity_state_is_collected_at(double x, double y, double radius_m) {
    double r2 = radius_m * radius_m;
    std::lock_guard<std::mutex> lk(g_mu);
    for (const Discovered& d : g_discovered) {
        double dx = d.x - x;
        double dy = d.y - y;
        if (dx * dx + dy * dy < r2) return true;
    }
    return false;
}

}  // namespace farever
