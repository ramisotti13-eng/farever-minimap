// Damage-event source, render-thread variant. The hl_alloc_obj
// watcher fires on whatever thread called the HashLink allocator
// (most often the main game thread) and only pushes the raw DR ptr
// into a queue — no field reads at hook time. The Present hook
// (d3d12_hook.cpp) calls damage_tick() once per frame on the render
// thread, which HashLink has already registered with its GC. That's
// the safe place to read the fresh DamageResult's fields without
// racing hxbit's deserialiser.
//
// History: an earlier build ran the pump on a background worker.
// Both unregistered and hl_register_thread-registered variants
// destabilised the engine (see feedback_hashlink_pump_thread.md).

#include "damage.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

// st.skill.DamageResult layout:
constexpr std::size_t OFF_DR_BASESKILL = 8;
constexpr std::size_t OFF_DR_AMOUNT    = 80;
constexpr std::size_t OFF_DR_HITCOUNT  = 88;
constexpr std::size_t OFF_DR_KILL      = 104;
constexpr std::size_t OFF_DR_CRITICAL  = 105;

constexpr std::size_t OFF_SKILL_KIND = 152;
constexpr std::size_t OFF_STR_BYTES  = 8;
constexpr std::size_t OFF_STR_LEN    = 16;

// Pending entries are retried for ~1 second (60 frames @ 60 Hz) before
// being dropped as "constructor never finished" — usually means the
// object was reused or a hxbit deserialise was abandoned.
constexpr int kMaxPendingRetries = 60;

constexpr std::size_t kEventRingMax = 4096;

struct Pending {
    std::uintptr_t dr_ptr;
    int            retries;
};

std::atomic<bool>          g_active{false};
std::atomic<std::uint64_t> g_allocs_seen{0};
std::atomic<std::uint64_t> g_events_emitted{0};
std::atomic<std::uint64_t> g_dropped_uninit{0};
std::atomic<std::uint64_t> g_dropped_garbage{0};
std::atomic<std::uint64_t> g_ticks{0};

std::mutex                          g_pending_mu;
std::deque<Pending>                 g_pending;

std::mutex                          g_events_mu;
std::deque<DamageEvent>             g_events;
std::unordered_set<std::uintptr_t>  g_seen_dr;

bool decode_skill_name(std::uintptr_t dr_ptr, char out[64]) {
    out[0] = 0;
    std::uint64_t bs_u64 = 0;
    if (!mem_read_u64(dr_ptr + OFF_DR_BASESKILL, &bs_u64)) return false;
    auto bs = static_cast<std::uintptr_t>(bs_u64);
    if (!mem_is_userland(bs)) return false;

    std::uint64_t skind_u64 = 0;
    if (!mem_read_u64(bs + OFF_SKILL_KIND, &skind_u64)) return false;
    auto skind = static_cast<std::uintptr_t>(skind_u64);
    if (!mem_is_userland(skind)) return false;

    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(skind + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(skind + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 63) return false;

    std::uint8_t buf[128];
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64),
                        buf, static_cast<std::size_t>(length) * 2))
        return false;

    for (int i = 0; i < length; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 128) return false;
        char ch = static_cast<char>(c);
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '.';
        if (!ok) return false;
        out[i] = ch;
    }
    out[length] = 0;
    return true;
}

// True = settled (either a valid event, or filtered garbage — caller
// drops it). False = not yet populated, retry on next tick.
bool try_decode(std::uintptr_t dr_ptr, DamageEvent* out) {
    std::int32_t hits = 0;
    if (!mem_read_i32(dr_ptr + OFF_DR_HITCOUNT, &hits)) return false;
    if (hits == 0) return false;
    if (hits < 1 || hits > 50) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    double damage = 0;
    if (!mem_read_f64(dr_ptr + OFF_DR_AMOUNT, &damage)) return false;
    if (!(damage > 0.0 && damage < 1e8)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::uint8_t crit = 0;
    std::uint8_t kill = 0;
    mem_read_u8(dr_ptr + OFF_DR_CRITICAL, &crit);
    mem_read_u8(dr_ptr + OFF_DR_KILL,     &kill);

    char skill[64];
    if (!decode_skill_name(dr_ptr, skill)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    out->dr_ptr    = dr_ptr;
    out->damage    = damage;
    out->hit_count = hits;
    out->is_crit   = crit ? 1 : 0;
    out->is_kill   = kill ? 1 : 0;
    std::memcpy(out->skill, skill, sizeof(out->skill));
    return true;
}

void on_dr_alloc(std::uintptr_t dr_ptr) {
    if (!dr_ptr) return;
    g_allocs_seen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.push_back({dr_ptr, 0});
}

}  // namespace

void damage_start(const LibHL& /*libhl*/) {
    if (g_active.exchange(true)) return;
    g_allocs_seen.store(0);
    g_events_emitted.store(0);
    g_dropped_uninit.store(0);
    g_dropped_garbage.store(0);
    g_ticks.store(0);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_events_mu);
        g_events.clear();
        g_seen_dr.clear();
    }
    hl_hook_register(L"st.skill.DamageResult", on_dr_alloc);
    logf("damage: watcher registered (render-thread tick)");
}

void damage_stop() {
    g_active.store(false);
}

void damage_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;

    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        logf("damage: first tick — render thread present hook is alive");
    }
    // Stats heartbeat every 600 ticks (~10 s at 60 Hz). Must run
    // OUTSIDE the work.empty() shortcut so we still observe periods of
    // no damage activity.
    if (n % 600 == 0) {
        logf("damage: tick %llu — allocs=%llu, events=%llu, "
             "pending=%zu, dropped(uninit)=%llu, dropped(garbage)=%llu",
             static_cast<unsigned long long>(n),
             static_cast<unsigned long long>(g_allocs_seen.load()),
             static_cast<unsigned long long>(g_events_emitted.load()),
             [] { std::lock_guard<std::mutex> lk(g_pending_mu);
                  return g_pending.size(); }(),
             static_cast<unsigned long long>(g_dropped_uninit.load()),
             static_cast<unsigned long long>(g_dropped_garbage.load()));
    }

    std::vector<Pending> work;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        work.assign(g_pending.begin(), g_pending.end());
        g_pending.clear();
    }
    if (work.empty()) return;

    std::vector<Pending>     retry;
    std::vector<DamageEvent> emit;
    retry.reserve(work.size());
    emit.reserve(work.size());

    for (Pending p : work) {
        DamageEvent ev{};
        if (try_decode(p.dr_ptr, &ev)) {
            if (ev.hit_count > 0) emit.push_back(ev);
        } else {
            if (++p.retries < kMaxPendingRetries) {
                retry.push_back(p);
            } else {
                g_dropped_uninit.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        for (auto it = retry.rbegin(); it != retry.rend(); ++it) {
            g_pending.push_front(*it);
        }
    }
    if (!emit.empty()) {
        std::lock_guard<std::mutex> lk(g_events_mu);
        for (const DamageEvent& ev : emit) {
            if (!g_seen_dr.insert(ev.dr_ptr).second) continue;
            g_events.push_back(ev);
            g_events_emitted.fetch_add(1, std::memory_order_relaxed);
            while (g_events.size() > kEventRingMax) g_events.pop_front();
        }
    }

    // (heartbeat moved to the top of the function so it fires even on
    // ticks where there's no pending work.)
}

std::size_t damage_drain(DamageEvent* out, std::size_t max) {
    if (!out || max == 0) return 0;
    std::lock_guard<std::mutex> lk(g_events_mu);
    std::size_t n = 0;
    while (n < max && !g_events.empty()) {
        out[n++] = g_events.front();
        g_events.pop_front();
    }
    return n;
}

DamageStats damage_stats() {
    DamageStats s{};
    s.allocs_seen      = g_allocs_seen.load(std::memory_order_relaxed);
    s.events_emitted   = g_events_emitted.load(std::memory_order_relaxed);
    s.dropped_uninit   = g_dropped_uninit.load(std::memory_order_relaxed);
    s.dropped_garbage  = g_dropped_garbage.load(std::memory_order_relaxed);
    s.damage_result_tag = 0;
    return s;
}

}  // namespace farever
