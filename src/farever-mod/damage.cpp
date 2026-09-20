// Damage-event source, render-thread variant. The hl_alloc_obj
// watcher fires on whatever thread called the HashLink allocator
// (most often the main game thread) and only pushes the raw DR ptr
// into a queue - no field reads at hook time. The Present hook
// (d3d12_hook.cpp) calls damage_tick() once per frame on the render
// thread, which HashLink has already registered with its GC. That's
// the safe place to read the fresh DamageResult's fields without
// racing hxbit's deserialiser.
//
// History: an earlier build ran the pump on a background worker.
// Both unregistered and hl_register_thread-registered variants
// destabilised the engine.

#include "damage.h"
#include "hero_state.h"
#include "skill_resolve.h"
#include "boss_timer.h"
#include "boss_names.h"
#include "boss_target.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"
#include "overlay.h"   // overlay_is_dps_tracking_paused (issue #13)

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

// ui.comp.DamageDisplay layout (the UI element). DD is created for
// ANY floating damage number the game shows - outgoing hits AND
// incoming hits (bleeds, mob hits, AoE on us). The target filter
// below drops events whose target is our own Hero (= incoming).
constexpr std::size_t OFF_DD_DMG_PTR = 1176;   // *DamageResult (was 1176) [v018 -16]

// st.skill.DamageResult layout:
constexpr std::size_t OFF_DR_BASESKILL    = 8;
constexpr std::size_t OFF_DR_TARGET       = 48;   // *ent.GameObject (victim)
constexpr std::size_t OFF_DR_AMOUNT       = 88;
constexpr std::size_t OFF_DR_HITCOUNT     = 96;
constexpr std::size_t OFF_DR_BLOCK        = 104;   // #87 _block (absorbed/blocked)
constexpr std::size_t OFF_DR_KILL         = 112;
constexpr std::size_t OFF_DR_CRITICAL     = 113;

constexpr std::size_t OFF_SKILL_KIND = 184;   // BaseSkill.kind (was 160) [v018 +16]
constexpr std::size_t OFF_UNIT_KIND  = 600;  // ent.Unit.kind (String) - #87 was 592 [v023 +8]
constexpr std::size_t OFF_STR_BYTES  = 8;
constexpr std::size_t OFF_STR_LEN    = 16;

// Pending entries are retried for ~1 second (60 frames @ 60 Hz) before
// being dropped as "constructor never finished" - usually means the
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

// Read an ASCII Haxe String (bytes @8 UTF-16, len @16) into `out`. Non-ASCII
// codepoints become '?'. Returns false on a bad pointer / empty string.
bool read_ascii_string(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    if (cap == 0) return false;
    out[0] = 0;
    if (!mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0) return false;
    if (static_cast<std::size_t>(length) > cap - 1)
        length = static_cast<std::int32_t>(cap - 1);
    std::uint8_t buf[256];
    if (static_cast<std::size_t>(length) * 2 > sizeof(buf))
        length = static_cast<std::int32_t>(sizeof(buf) / 2);
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64), buf,
                        static_cast<std::size_t>(length) * 2))
        return false;
    int o = 0;
    for (int i = 0; i < length; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c == 0) break;
        out[o++] = (c < 128) ? static_cast<char>(c) : '?';
    }
    out[o] = 0;
    return o > 0;
}

// #87: resolve a target unit's display name the exact way farever.target.name()
// does - ent.Unit.kind@592, falling back to the stripped HashLink class name
// for bosses whose kind String is null (boss_display_name maps the few that
// differ). Empty `out` on failure. So a FareverLogs plugin sees the same name
// the target API would give, captured at the moment of the hit.
void resolve_target_name(std::uintptr_t tgt, char out[64]) {
    out[0] = 0;
    if (!mem_is_userland(tgt)) return;
    std::uint64_t kind_u64 = 0;
    if (mem_read_u64(tgt + OFF_UNIT_KIND, &kind_u64))
        read_ascii_string(static_cast<std::uintptr_t>(kind_u64), out, 64);
    if (out[0] == 0) {
        char cls[64] = {0};
        if (hl_hook_class_name(tgt, cls, sizeof cls)) {
            std::string disp = boss_display_name(cls);
            std::strncpy(out, disp.c_str(), 63);
            out[63] = 0;
        }
    }
}

// True = settled, false = not yet populated (retry next tick).
// `dd_ptr` is the DamageDisplay we observed at alloc time; we chase
// its dmgPtr field to find the underlying DamageResult, then decode
// that. Using DD as the trigger gives us automatic filtering - DDs
// only get created when the game's UI layer decides to render the
// hit number to the local player.
bool try_decode(std::uintptr_t dd_ptr, DamageEvent* out) {
    std::uint64_t dr_u64 = 0;
    if (!mem_read_u64(dd_ptr + OFF_DD_DMG_PTR, &dr_u64)) return false;
    if (dr_u64 == 0) return false;  // DD constructor not done yet
    auto dr_ptr = static_cast<std::uintptr_t>(dr_u64);
    if (!mem_is_userland(dr_ptr)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // _hitCount on DamageResult is a running tick ordinal for the
    // owning BaseSkill, not the number of hits in this DR. Channeled
    // skills tick it 1, 2, 3, ... per hit. We use it only as a
    // signedness sanity-check + uninitialised filter; the aggregator
    // counts each event as exactly one hit.
    std::int32_t hits = 0;
    if (!mem_read_i32(dr_ptr + OFF_DR_HITCOUNT, &hits)) return false;
    if (hits == 0) return false;
    if (hits < 1 || hits > 10000) {
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

    // Target filter: DD fires for ANY floating damage number, including
    // hits ON the player (bleeds, mob attacks, ground AoE). If the
    // DR's target is our own Hero, that's INCOMING - drop. (We tried
    // matching serverSource against the Hero pointer first; it never
    // matches because the game stores a network proxy / weak ref
    // there, not the raw Hero address.)
    std::uintptr_t my_hero = hero_state_locked_ptr();
    if (my_hero) {
        std::uint64_t tgt_u64 = 0;
        if (mem_read_u64(dr_ptr + OFF_DR_TARGET, &tgt_u64) &&
            tgt_u64 == (std::uint64_t)my_hero) {
            g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    // Boss detection on an outgoing hit whose target is an ent.boss.*. This
    // feeds two consumers: the boss-target cache that farever.target falls back
    // to for boss-only-dungeon bosses (#69), and the opt-in speedrun timer.
    // A 1-entry cache keyed on the target pointer skips the class-name read on
    // repeated hits against the same target, which is the common case in a
    // boss fight, so this stays cheap even with the speedrun mode off.
    {
        std::uint64_t tgt2 = 0;
        if (mem_read_u64(dr_ptr + OFF_DR_TARGET, &tgt2) &&
            mem_is_userland(tgt2)) {
            std::uintptr_t tp = static_cast<std::uintptr_t>(tgt2);
            static std::uintptr_t s_last_tgt   = 0;
            static bool           s_last_boss  = false;
            static char           s_last_cls[64] = {0};
            bool is_boss;
            if (tp == s_last_tgt) {
                is_boss = s_last_boss;
            } else {
                char cls[64];
                is_boss = hl_hook_class_name(tp, cls, sizeof(cls)) &&
                          is_boss_type(cls);
                s_last_tgt  = tp;
                s_last_boss = is_boss;
                if (is_boss) std::memcpy(s_last_cls, cls, sizeof(s_last_cls));
            }
            if (is_boss) {
                boss_target_note_hit(tp, s_last_cls);
                if (boss_timer_enabled()) {
                    boss_timer_on_boss_hit(tp, s_last_cls, kill != 0);
                }
            }
        }
    }

    char skill[64];
    if (!decode_skill_name(dr_ptr, skill)) {
        g_dropped_garbage.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Resolve the BaseSkill -> CDB record once per kind. Failure is
    // expected for early ticks (libhl exports may not have resolved
    // yet) and for skills whose `inf` virtual hasn't been initialised
    // yet - the next event for the same kind will retry.
    SkillGfx gfx{};
    if (!skill_resolve_lookup_exact(skill, &gfx)) {
        std::uint64_t bs_u64 = 0;
        if (mem_read_u64(dr_ptr + OFF_DR_BASESKILL, &bs_u64)) {
            if (skill_resolve_query(
                    static_cast<std::uintptr_t>(bs_u64), &gfx)) {
                skill_resolve_cache(skill, gfx);
            } else {
                // v0.6.0 phase 2c: on worker mode the query bails out
                // (hxbit-unsafe). Queue the resolve for the alloc-
                // context pump which fires from on_dd_alloc on the
                // game's main thread.
                skill_resolve_request_deferred(skill,
                    static_cast<std::uintptr_t>(bs_u64));
            }
        }
    }

    // #87: enrich with the victim name (== farever.target.name()) and the
    // absorbed/blocked amount so a FareverLogs plugin can build schema-complete
    // entries without a separate, racy target read.
    char target_name[64] = {0};
    {
        std::uint64_t tgt_u64 = 0;
        if (mem_read_u64(dr_ptr + OFF_DR_TARGET, &tgt_u64))
            resolve_target_name(static_cast<std::uintptr_t>(tgt_u64),
                                target_name);
    }
    double blocked = 0.0;
    mem_read_f64(dr_ptr + OFF_DR_BLOCK, &blocked);
    if (!(blocked >= 0.0 && blocked < 1e8)) blocked = 0.0;

    // One-shot diagnostic so the _block semantics + name resolution are
    // verifiable from a normal farever-mod.log without a probe build.
    static bool s_diag_logged = false;
    if (!s_diag_logged) {
        s_diag_logged = true;
        logf("damage: event enrich (#87) - skill=%s amount=%.1f block=%.1f "
             "target=\"%s\" crit=%d kill=%d",
             skill, damage, blocked, target_name, crit ? 1 : 0,
             kill ? 1 : 0);
    }

    out->dr_ptr    = dr_ptr;         // dedupe by DR so multiple DDs on
                                     // the same DR (crit + main, etc.)
                                     // only emit once
    out->damage    = damage;
    out->hit_count = hits;
    out->is_crit   = crit ? 1 : 0;
    out->is_kill   = kill ? 1 : 0;
    out->blocked   = blocked;
    std::memcpy(out->skill, skill, sizeof(out->skill));
    std::memcpy(out->target_name, target_name, sizeof(out->target_name));
    return true;
}

void on_dd_alloc(std::uintptr_t dd_ptr) {
    if (!dd_ptr) return;
    // v0.4.15.1: if the module is stopped (anticrash disarm called
    // damage_stop), skip the queue push entirely. Matters during
    // anticrash self-heal: the alloc-hook is re-armed for the new
    // Hero allocation, but damage stays off - without this guard
    // pending would accumulate DamageDisplay events nobody drains.
    if (!g_active.load(std::memory_order_acquire)) return;
    // Issue #13: when the user paused DPS tracking, bail out before
    // the queue push so we don't accumulate work that damage_tick
    // would just discard. The MinHook trampoline overhead still runs
    // (few ns/alloc) but no further per-event work happens.
    if (overlay_is_dps_tracking_paused()) return;
    g_allocs_seen.fetch_add(1, std::memory_order_relaxed);
    // Hard cap so a heavy combat burst can't pile up an unbounded
    // backlog. Old pending entries get dropped on overflow -- minor
    // DPS-tracking accuracy loss vs. an unbounded queue and the
    // associated GC / render-thread risk on long sessions.
    constexpr std::size_t kMaxPending = 256;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.push_back({dd_ptr, 0});
        while (g_pending.size() > kMaxPending) g_pending.pop_front();
    }
    // v0.6.0 phase 2c: alloc-context pump for skill_resolve. We're on
    // the thread that called hl_alloc_obj - for ui.comp.DamageDisplay
    // that's the game's main/hxbit thread, safe to dispatch through
    // hl_dyn_getp. The pump drains deferred requests queued by the
    // worker (skill_resolve_request_deferred, when its query bailed
    // out) and caches the results for the next damage_tick.
    skill_resolve_pump_on_game_thread();
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
    hl_hook_register(L"ui.comp.DamageDisplay", on_dd_alloc);
    logf("damage: watcher registered on ui.comp.DamageDisplay "
         "(render-thread tick) - UI-layer filter for local-player damage");
}

void damage_stop() {
    g_active.store(false);
}

// v0.4.14 (F): SEH auto-disable mirror of hero_state's.
constexpr int               kMaxConsecutiveFailures = 5;
static std::atomic<int>     g_consecutive_failures{0};

static void damage_tick_body(std::uint64_t n) {
    // Throttle: process at most kMaxPerTick events per frame. A heavy
    // combat burst (observed ~65 DamageDisplay allocs in a single
    // game-loop scheduler slot during a Mage_Conduit_Projectile
    // channel) used to drag the render thread for ~10 ms straight,
    // which correlated with the recurring DX12Driver.present AV.
    // Spreading the work over multiple frames keeps the render-thread
    // budget bounded; the DPS-meter readout is sampled, not realtime,
    // so a one-frame delay per batch is invisible.
    constexpr std::size_t kMaxPerTick = 4;
    std::vector<Pending> work;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        std::size_t take = g_pending.size();
        if (take > kMaxPerTick) take = kMaxPerTick;
        for (std::size_t i = 0; i < take; ++i) {
            work.push_back(g_pending.front());
            g_pending.pop_front();
        }
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

void damage_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    // Issue #13: if the user paused DPS tracking, short-circuit before
    // the heartbeat / pending-walk so the render thread doesn't do any
    // hl_dyn_getp work at all. Resumes cleanly when unpaused.
    if (overlay_is_dps_tracking_paused()) return;

    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        // NB: this tick is DRIVEN by the Present hook but it executes on the
        // hl_pump worker thread (same tid as "hl_pump: worker live"), not on
        // a separate render thread. That is why skill_resolve_query bails out
        // here and defers instead - and why this is NOT a usable drain point
        // for the deferred queue. The drain lives in alloc-watcher context
        // only (see skill_resolve_pump_on_game_thread callers).
        logf("damage: first tick - present-hook driven tick is alive "
             "(tid=%lu, worker-thread context)", GetCurrentThreadId());
    }

    // Stats heartbeat every 600 ticks (~10 s at 60 Hz). Fires regardless
    // of throttling below so log timing stays consistent.
    if (n % 600 == 0) {
        logf("damage: tick %llu - allocs=%llu, events=%llu, "
             "pending=%zu, dropped(uninit)=%llu, dropped(garbage)=%llu",
             static_cast<unsigned long long>(n),
             static_cast<unsigned long long>(g_allocs_seen.load()),
             static_cast<unsigned long long>(g_events_emitted.load()),
             [] { std::lock_guard<std::mutex> lk(g_pending_mu);
                  return g_pending.size(); }(),
             static_cast<unsigned long long>(g_dropped_uninit.load()),
             static_cast<unsigned long long>(g_dropped_garbage.load()));
    }

    // v0.4.14 (C): only drain pending on even ticks. Halves the
    // per-frame hl_dyn_getp pressure from this module. DamageDisplay's
    // retry buffer makes the one-frame staggered delay invisible.
    if (n & 1) return;

    // v0.4.14 (F): SEH-wrap the drain. mem_reads into freshly-allocated
    // DamageResults that haven't finished construction can trip an AV
    // despite the userland-pointer guards; auto-disable kicks in if it
    // happens repeatedly so the game stays alive.
    __try {
        damage_tick_body(n);
        g_consecutive_failures.store(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int fails = g_consecutive_failures.fetch_add(1) + 1;
        logf("damage: SEH trip #%d in tick %llu (code 0x%08lx)",
             fails, (unsigned long long)n,
             GetExceptionCode());
        if (fails >= kMaxConsecutiveFailures) {
            g_active.store(false);
            logf("damage: %d consecutive SEH trips - auto-disabling "
                 "module to keep the game alive. Restart to re-enable.",
                 fails);
        }
    }
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
