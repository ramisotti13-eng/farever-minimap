// v0.6.0 phase 2d: own UID -> pointer registry. See uid_registry.h.

#include "uid_registry.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace farever {
namespace {

// __uid field offset on hxbit.Serializable (inherited by ent.Foe,
// ent.Hero, etc.). Verified from tools/classes_v0152.json:
//   { "name": "__uid", "kind_name": "HI64", "offset": 32, "size": 8 }
constexpr std::size_t OFF_SER_UID = 32;

// Hard cap on pending ring. Old entries get dropped - they presumably
// never got their __uid filled in (rare; mostly happens for objects
// that weren't actually hxbit-deserialized, e.g. local UI elements).
constexpr std::size_t kPendingMax = 512;

// Cap on resolved map. We don't actively GC stale entries; type-tag
// check at lookup time handles individual stale entries. Cap is a
// safety net against runaway growth in pathological cases.
constexpr std::size_t kResolvedMax = 4096;

struct Tracked {
    std::uintptr_t ptr;
    std::uintptr_t type_ptr;
};

std::mutex                                       g_mu;
std::deque<Tracked>                              g_pending;
std::unordered_map<std::uint64_t, Tracked>       g_resolved;

std::atomic<std::uint64_t> g_promoted_count{0};
std::atomic<std::uint64_t> g_lookup_hits{0};
std::atomic<std::uint64_t> g_lookup_evicts{0};

// Per-class type ptrs - captured from hl_hook on first alloc of each
// class. Used to verify type-tag in uid_registry_lookup.
std::atomic<std::uintptr_t> g_type_ent_foe{0};
std::atomic<std::uintptr_t> g_type_ent_hero{0};

// Watcher callbacks. Fire from alloc-context on the game's main/hxbit
// thread (same thread that called hl_alloc_obj). Capture pending and
// run the pump - that way every alloc both registers a fresh entry
// AND advances any older entries waiting for their __uid.
void on_foe_alloc(std::uintptr_t obj_ptr) {
    if (!obj_ptr) return;
    std::uintptr_t t = g_type_ent_foe.load(std::memory_order_acquire);
    if (!t) {
        t = hl_hook_get_type(L"ent.Foe");
        if (t) g_type_ent_foe.store(t, std::memory_order_release);
    }
    uid_registry_track(obj_ptr, t);
    uid_registry_pump_in_alloc_context();
}

void on_hero_alloc(std::uintptr_t obj_ptr) {
    if (!obj_ptr) return;
    std::uintptr_t t = g_type_ent_hero.load(std::memory_order_acquire);
    if (!t) {
        t = hl_hook_get_type(L"ent.Hero");
        if (t) g_type_ent_hero.store(t, std::memory_order_release);
    }
    uid_registry_track(obj_ptr, t);
    uid_registry_pump_in_alloc_context();
}

}  // namespace

bool uid_registry_init(const LibHL& /*libhl*/) {
    hl_hook_register(L"ent.Foe",  on_foe_alloc);
    hl_hook_register(L"ent.Hero", on_hero_alloc);
    logf("uid_registry: watchers registered (ent.Foe, ent.Hero) - "
         "worker mode target tracking armed");
    return true;
}

void uid_registry_track(std::uintptr_t obj_ptr, std::uintptr_t type_ptr) {
    if (!obj_ptr) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending.push_back({obj_ptr, type_ptr});
    while (g_pending.size() > kPendingMax) g_pending.pop_front();
}

void uid_registry_pump_in_alloc_context() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_pending.empty()) return;
    std::size_t promoted = 0;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (!mem_is_userland(it->ptr)) {
            it = g_pending.erase(it);
            continue;
        }
        std::uint64_t uid = 0;
        if (!mem_read_u64(it->ptr + OFF_SER_UID, &uid)) {
            it = g_pending.erase(it);
            continue;
        }
        if (uid == 0) {
            ++it;  // hxbit hasn't deserialized __uid yet, retry next pump
            continue;
        }
        g_resolved[uid] = *it;
        ++promoted;
        it = g_pending.erase(it);
    }
    if (promoted) {
        g_promoted_count.fetch_add(promoted, std::memory_order_relaxed);
    }
    // Cap resolved map - if pathological growth, drop arbitrary entries.
    // In practice we expect <500 simultaneous foes per zone.
    if (g_resolved.size() > kResolvedMax) {
        std::size_t to_drop = g_resolved.size() - kResolvedMax;
        auto it = g_resolved.begin();
        while (to_drop-- > 0 && it != g_resolved.end()) {
            it = g_resolved.erase(it);
        }
    }
}

std::uintptr_t uid_registry_lookup(std::uint64_t uid) {
    if (uid == 0) return 0;
    Tracked t;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_resolved.find(uid);
        if (it == g_resolved.end()) return 0;
        t = it->second;
    }
    // Type-tag verification: stored pointer's *(p+0) hl_type* must match
    // the type_ptr we recorded at track time. Catches GC slot reuse -
    // the Foe died, ptr is now an unrelated object. mem_read_u64 is
    // SEH-protected against freed pages.
    if (!mem_is_userland(t.ptr)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_resolved.erase(uid);
        g_lookup_evicts.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    std::uint64_t actual_type = 0;
    if (!mem_read_u64(t.ptr, &actual_type)) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_resolved.erase(uid);
        g_lookup_evicts.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (t.type_ptr != 0 &&
        static_cast<std::uintptr_t>(actual_type) != t.type_ptr) {
        // Slot reused for a different class. Evict.
        std::lock_guard<std::mutex> lk(g_mu);
        g_resolved.erase(uid);
        g_lookup_evicts.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    g_lookup_hits.fetch_add(1, std::memory_order_relaxed);
    return t.ptr;
}

}  // namespace farever
