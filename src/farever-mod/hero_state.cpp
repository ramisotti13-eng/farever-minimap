// Local-Hero tracker driven by the hl_alloc_obj watcher.
//
// Each ent.Hero allocation pushes the raw pointer into a pending
// list. hero_state_tick() (called per frame from the Present hook,
// after damage_tick) iterates pending candidates, waits until the
// Haxe constructor has populated Hero.ownerPlayer, then verifies:
//   - ownerPlayer is a userland pointer
//   - ownerPlayer.hero == this Hero  (bidirectional integrity)
//   - ownerPlayer.isMe == 1          (we are the local player)
// First candidate that passes is locked. Subsequent allocs that
// arrive before the lock get retried up to a hard cap; the lock is
// re-validated every 64 ticks so dungeon transitions don't freeze
// us on a stale pointer.

#include "hero_state.h"
#include "hl_hook.h"
#include "damage.h"
#include "mem_scan.h"
#include "log.h"
#include "progress_state.h"
#include "plugins.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace farever {
namespace {

// ent.Hero offsets — same as the structural scanner used.
constexpr std::size_t OFF_HERO_OWNERPLAYER  = 16;
constexpr std::size_t OFF_HERO_POSX         = 144;   // f64
constexpr std::size_t OFF_HERO_POSY         = 152;
constexpr std::size_t OFF_HERO_POSZ         = 160;
constexpr std::size_t OFF_HERO_ROTZ         = 168;
constexpr std::size_t OFF_HERO_ISINCOMBAT   = 672;   // u8

constexpr std::size_t OFF_PLAYER_HERO       = 272;   // Player.hero (back-ref)
constexpr std::size_t OFF_PLAYER_ISME       = 280;   // u8

// How many ticks (~ frames) to retry a pending Hero before dropping
// it. Old value 600 = ~10s; bumped to 6000 (~100s) so dungeon-exit
// loading screens (~15s observed) don't silently flush all our
// candidates while we're waiting for the constructor to populate.
constexpr int kMaxPendingRetries = 6000;

struct Pending {
    std::uintptr_t hero_ptr;
    int            retries;
};

std::atomic<bool>           g_active{false};
std::atomic<int>            g_unlocked_alloc_log_left{16};
std::atomic<std::uintptr_t> g_locked_hero{0};
// Cached owner Player. The Player object is the stable session entity
// — it survives zone / dungeon transitions even when the Hero is
// swapped out. After a re-validation failure we follow Player.hero
// to find the fresh Hero without waiting for a new alloc-hook event.
std::atomic<std::uintptr_t> g_locked_player{0};
std::atomic<std::uint64_t>  g_ticks{0};

std::mutex                  g_pending_mu;
std::vector<Pending>        g_pending;

HeroSnapshot                g_snapshot{};

// Plausible world-coordinate ranges (W1 spans ~6 tiles × 256 m).
// Used to reject sync-proxy / template Heroes whose isMe is set but
// whose position is still (0,0) — the structural scan in minimap-dll
// applied the same filter, and skipping it here was the cause of the
// "arrow doesn't track the player" bug.
constexpr double RX_LO = -10000.0, RX_HI = 10000.0;
constexpr double RY_LO = -10000.0, RY_HI = 10000.0;
constexpr double RZ_LO =   -500.0, RZ_HI =  1500.0;

bool position_is_plausible(std::uintptr_t hero_ptr) {
    double pos[4];
    if (!mem_read_bytes(hero_ptr + OFF_HERO_POSX, pos, sizeof(pos)))
        return false;
    double x = pos[0], y = pos[1], z = pos[2];
    if (std::isnan(x) || std::isinf(x)) return false;
    if (std::isnan(y) || std::isinf(y)) return false;
    if (std::isnan(z) || std::isinf(z)) return false;
    if (x < RX_LO || x > RX_HI) return false;
    if (y < RY_LO || y > RY_HI) return false;
    if (z < RZ_LO || z > RZ_HI) return false;
    // (0, 0) is the canonical "uninitialised" pose — reject it
    // explicitly so we don't lock on a template Hero whose isMe was
    // already set by the network deserialiser.
    if (std::fabs(x) < 0.01 && std::fabs(y) < 0.01) return false;
    return true;
}

// Returns true if this Hero pointer is currently the local player's:
//   - ownerPlayer.isMe == 1
//   - bidirectional Player.hero == this Hero
//   - position is a plausible in-world coordinate (not the world
//     origin and not the bogus values a not-yet-streamed Hero shows)
bool is_local_hero(std::uintptr_t hero_ptr) {
    std::uint64_t owner_u64 = 0;
    if (!mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &owner_u64)) return false;
    auto owner = static_cast<std::uintptr_t>(owner_u64);
    if (!mem_is_userland(owner)) return false;

    std::uint64_t player_hero = 0;
    if (!mem_read_u64(owner + OFF_PLAYER_HERO, &player_hero)) return false;
    if (player_hero != hero_ptr) return false;

    std::uint8_t is_me = 0;
    if (!mem_read_u8(owner + OFF_PLAYER_ISME, &is_me)) return false;
    if (is_me != 1) return false;

    return position_is_plausible(hero_ptr);
}

// Verbose debug dump used around lock-lifecycle events. Exists so we
// can trace dungeon entry / exit transitions in detail without
// changing the hot-path is_local_hero() above.
void debug_dump_hero(const char* tag, std::uintptr_t hero_ptr) {
    if (!hero_ptr) {
        logf("hero_state[%s]: hero=NULL", tag);
        return;
    }
    if (!mem_is_userland(hero_ptr)) {
        logf("hero_state[%s]: hero=0x%llx INVALID(not userland)",
             tag, (unsigned long long)hero_ptr);
        return;
    }
    std::uint64_t owner_u64 = 0;
    bool owner_ok = mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &owner_u64);
    std::uint64_t back_hero = 0;
    std::uint8_t  isme      = 255;
    if (owner_ok && mem_is_userland(owner_u64)) {
        mem_read_u64(owner_u64 + OFF_PLAYER_HERO, &back_hero);
        mem_read_u8 (owner_u64 + OFF_PLAYER_ISME, &isme);
    }
    double pos[4] = {0,0,0,0};
    bool pos_ok = mem_read_bytes(hero_ptr + OFF_HERO_POSX, pos, sizeof(pos));
    std::uint8_t in_combat = 255;
    mem_read_u8(hero_ptr + OFF_HERO_ISINCOMBAT, &in_combat);

    logf("hero_state[%s]: hero=0x%llx owner=0x%llx isMe=%u "
         "player.hero=0x%llx (match=%d) pos=(%.1f,%.1f,%.1f%s) "
         "inCombat=%u",
         tag,
         (unsigned long long)hero_ptr,
         (unsigned long long)owner_u64,
         (unsigned)isme,
         (unsigned long long)back_hero,
         (int)(back_hero == hero_ptr),
         pos_ok ? pos[0] : 0.0,
         pos_ok ? pos[1] : 0.0,
         pos_ok ? pos[2] : 0.0,
         pos_ok ? "" : " READFAIL",
         (unsigned)in_combat);
}

void on_hero_alloc(std::uintptr_t obj) {
    if (!obj) return;
    // v0.4.14: when already locked, the pending queue exists only to
    // catch zone-transition re-locks. City scenes with 30+ remote
    // players were piling up hundreds of entries per second, each
    // costing a full is_local_hero check per tick. Tight cap when
    // locked keeps zone-transitions detectable without the churn.
    const bool locked = g_locked_hero.load(std::memory_order_acquire) != 0;
    const std::size_t kMaxPending = locked ? 8 : 256;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.push_back({obj, 0});
        if (g_pending.size() > kMaxPending) {
            g_pending.erase(
                g_pending.begin(),
                g_pending.begin() + (g_pending.size() - kMaxPending));
        }
    }
    // Diagnostic: log every alloc that arrives while we're unlocked.
    // Resets each time the lock drops.
    if (g_unlocked_alloc_log_left.fetch_sub(1) > 0) {
        debug_dump_hero("alloc", obj);
    }
}

// Read ownerPlayer from a known-good Hero. Used right after locking so
// we can chase Player.hero on later re-validation failures.
std::uintptr_t read_owner_player(std::uintptr_t hero_ptr) {
    std::uint64_t v = 0;
    if (!mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &v)) return 0;
    if (!mem_is_userland(static_cast<std::uintptr_t>(v))) return 0;
    return static_cast<std::uintptr_t>(v);
}

// Try to recover a fresh Hero by walking Player.hero. Returns the new
// Hero pointer on success, or 0 if the player back-ref is gone or the
// new Hero doesn't validate.
std::uintptr_t try_relock_via_player() {
    std::uintptr_t player = g_locked_player.load(std::memory_order_acquire);
    if (!player || !mem_is_userland(player)) return 0;
    std::uint64_t hero_u64 = 0;
    if (!mem_read_u64(player + OFF_PLAYER_HERO, &hero_u64)) return 0;
    auto hero = static_cast<std::uintptr_t>(hero_u64);
    if (!hero || !mem_is_userland(hero)) return 0;
    if (!is_local_hero(hero)) return 0;
    return hero;
}

void publish() {
    HeroSnapshot s{};
    std::uintptr_t a = g_locked_hero.load(std::memory_order_acquire);
    if (a == 0) {
        g_snapshot = s;
        return;
    }
    // v0.4.14 (E): type-tag check before dereferencing. Boehm GC reuses
    // dead object slots without unmapping pages, so an invalidated Hero
    // can still memcpy cleanly but reads garbage. Comparing the type
    // pointer at +0 against our learned hl_type for ent.Hero is the
    // cheapest possible guard and matches what hl_hook itself learns
    // when caching the watcher. Mismatch -> drop the lock.
    static std::uintptr_t s_hero_type = 0;
    if (s_hero_type == 0) s_hero_type = hl_hook_get_type(L"ent.Hero");
    if (s_hero_type) {
        std::uint64_t got_type = 0;
        if (!mem_read_u64(a, &got_type) ||
            static_cast<std::uintptr_t>(got_type) != s_hero_type) {
            logf("hero_state: type-tag mismatch on locked Hero @ 0x%llx "
                 "(got 0x%llx, want 0x%llx) — dropping lock",
                 (unsigned long long)a,
                 (unsigned long long)got_type,
                 (unsigned long long)s_hero_type);
            g_locked_hero.store(0);
            g_locked_player.store(0);
            g_unlocked_alloc_log_left.store(16);
            g_snapshot = s;
            return;
        }
    }
    double pos[4]{0,0,0,0};
    if (!mem_read_bytes(a + OFF_HERO_POSX, pos, sizeof(pos))) {
        // Pointer went stale (memory unmapped). Drop the lock and let
        // the next alloc seed a fresh candidate. Player back-ref also
        // becomes meaningless without a valid Hero anchor.
        logf("hero_state: read FAILED on locked Hero @ 0x%llx — "
             "dropping lock",
             (unsigned long long)a);
        g_locked_hero.store(0);
        g_locked_player.store(0);
        g_unlocked_alloc_log_left.store(16);
        g_snapshot = s;
        return;
    }
    // Drop locks that read NaN / inf — that's the signature of a
    // Hero whose memory got overwritten by the GC during a zone
    // transition (we saw owner=garbage, pos=NaN in the logs).
    if (std::isnan(pos[0]) || std::isnan(pos[1]) || std::isnan(pos[2]) ||
        std::isinf(pos[0]) || std::isinf(pos[1]) || std::isinf(pos[2])) {
        logf("hero_state: NaN / inf in pos on locked Hero @ 0x%llx — "
             "dropping lock",
             (unsigned long long)a);
        g_locked_hero.store(0);
        g_locked_player.store(0);
        g_unlocked_alloc_log_left.store(16);
        g_snapshot = s;
        return;
    }
    s.locked = true;
    s.x      = pos[0];
    s.y      = pos[1];
    s.z      = pos[2];
    s.rot_z  = pos[3];

    std::uint8_t in_combat = 0;
    if (mem_read_u8(a + OFF_HERO_ISINCOMBAT, &in_combat)) {
        s.in_combat = (in_combat != 0);
    }
    g_snapshot = s;
}

}  // namespace

void hero_state_start() {
    if (g_active.exchange(true)) return;
    g_locked_hero.store(0);
    g_locked_player.store(0);
    g_ticks.store(0);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.clear();
    }
    g_snapshot = HeroSnapshot{};
    hl_hook_register(L"ent.Hero", on_hero_alloc);
    logf("hero_state: watcher registered (render-thread tick)");
}

void hero_state_stop() {
    g_active.store(false);
}

// v0.4.14 (F): consecutive SEH trips before we auto-disable this whole
// module. Mirrors the auto-disable on the overlay side. Self-healing
// safety net for users whose game state happens to corrupt one of
// our pointer chases despite the type-tag check.
constexpr int               kMaxConsecutiveFailures = 5;
static std::atomic<int>     g_consecutive_failures{0};

// v0.4.15 anticrash mode. Issues #11 and #16 retest on v0.4.14 showed
// the throttled HL-read path still triggers the AV. Even at 1 damage
// alloc / minute (kesmese alt-tabbed) the trampoline overhead through
// hl_alloc_obj on every game allocation accumulates. The only fix is
// to remove the trampoline entirely once the lock is stable.
//
// When armed (data/anticrash.flag at boot, dllmain calls
// hero_state_set_anticrash(true)):
//   1. Watcher-driven lock acquisition runs normally until first lock.
//   2. We count consecutive ticks where the lock holds.
//   3. After kAnticrashStableTicks (5 s at 60 Hz), we call
//      hl_hook_disable_alloc() to surgically remove the alloc-hook
//      trampoline and damage_stop() to drop the damage pipeline.
//   4. From then on, hero_state_tick polls Player.hero each
//      throttled-publish frame to detect zone transitions; no more
//      alloc-hook events ever arrive.
//
// Trade-off: DPS tracking stops. Users opt in via the flag file.
constexpr std::uint64_t           kAnticrashStableTicks = 300;
static std::atomic<bool>          g_anticrash_on{false};
static std::atomic<bool>          g_anticrash_disarmed{false};
static std::atomic<std::uint64_t> g_lock_stable_ticks{0};

// Read Hero pointer via the back-reference from the cached Player.
// Player itself survives zone transitions; only Hero gets swapped.
// Used after anticrash disarm to detect zone-transition re-locks
// without needing the alloc-hook watcher.
std::uintptr_t poll_hero_via_player() {
    std::uintptr_t player = g_locked_player.load(std::memory_order_acquire);
    if (!player || !mem_is_userland(player)) return 0;
    std::uint64_t hero_u64 = 0;
    if (!mem_read_u64(player + OFF_PLAYER_HERO, &hero_u64)) return 0;
    auto hero = static_cast<std::uintptr_t>(hero_u64);
    if (!hero || !mem_is_userland(hero)) return 0;
    return hero;
}

static void hero_state_tick_body(std::uint64_t n, std::uintptr_t locked) {
    // v0.4.15 anticrash post-disarm path: alloc-hook is gone, no more
    // watcher events arrive. Poll Player.hero on the throttled-publish
    // cadence to detect zone-transition re-locks. is_local_hero still
    // re-validates correctness via the position-plausibility check.
    // v0.4.15.1: self-heal addition — when polling loses the lock
    // (zone transition that swapped BOTH Hero and Player, e.g. dungeon
    // instance entry), re-arm the alloc-hook so the watcher catches
    // the new Hero alloc. The next stable lock then disarms again.
    // Self-healing loop: alloc-hook only armed during transitions,
    // not during steady-state play.
    if (g_anticrash_disarmed.load(std::memory_order_acquire)) {
        if ((n & 0x3) == 0) {
            std::uintptr_t fresh = poll_hero_via_player();
            if (fresh && fresh != locked && is_local_hero(fresh)) {
                g_locked_hero.store(fresh, std::memory_order_release);
                logf("hero_state: polling RE-LOCKED via Player.hero "
                     "@ 0x%llx (tick %llu, prev=0x%llx)",
                     (unsigned long long)fresh, (unsigned long long)n,
                     (unsigned long long)locked);
                locked = fresh;
            } else if (locked && !is_local_hero(locked)) {
                logf("hero_state: polling LOST lock at tick %llu — "
                     "self-heal: re-arming alloc-hook",
                     (unsigned long long)n);
                g_locked_hero.store(0);
                g_locked_player.store(0);
                g_unlocked_alloc_log_left.store(16);
                if (hl_hook_re_enable_alloc()) {
                    g_anticrash_disarmed.store(false,
                                               std::memory_order_release);
                    g_lock_stable_ticks.store(0,
                                              std::memory_order_release);
                }
                locked = 0;
                return;   // next tick will use the normal armed branch
            }
            publish();
        }
        if (locked && (n % 600 == 0)) {
            logf("hero_state[anticrash]: pos=(%.1f, %.1f, %.1f) tick %llu",
                 g_snapshot.x, g_snapshot.y, g_snapshot.z,
                 (unsigned long long)n);
        }
        return;
    }

    // v0.4.14 (D): cap drain per tick so a burst of allocs (city scene
    // with many players entering range, dungeon-exit burst of ~16
    // template Heroes) can't run dozens of is_local_hero checks in
    // a single frame.
    constexpr std::size_t kMaxDrainPerTick = 8;
    std::vector<Pending> work;
    std::vector<Pending> remainder;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        std::size_t take = std::min(kMaxDrainPerTick, g_pending.size());
        work.assign(g_pending.begin(), g_pending.begin() + take);
        if (take < g_pending.size()) {
            remainder.assign(g_pending.begin() + take, g_pending.end());
        }
        g_pending.clear();
    }
    std::vector<Pending> retry;
    retry.reserve(work.size());
    std::uintptr_t found = 0;
    for (Pending p : work) {
        if (is_local_hero(p.hero_ptr)) {
            // Prefer the newest match in this batch (the dungeon-exit
            // burst allocates ~16 Heros back-to-back; only the last is
            // the live one — the rest are sync-proxies / templates).
            found = p.hero_ptr;
        } else if (++p.retries < kMaxPendingRetries) {
            retry.push_back(p);
        }
    }
    if (found && found != locked) {
        std::uintptr_t player = read_owner_player(found);
        g_locked_player.store(player, std::memory_order_release);
        g_locked_hero.store(found, std::memory_order_release);
        const char* verb = (locked == 0) ? "LOCKED" : "SWITCHED";
        logf("hero_state: %s Hero @ 0x%llx (player=0x%llx, tick %llu, "
             "prev=0x%llx)",
             verb,
             (unsigned long long)found,
             (unsigned long long)player,
             (unsigned long long)n,
             (unsigned long long)locked);
        debug_dump_hero(verb, found);
        plugins_emit_hero_locked();
        progress_state_inspect(found);   // Phase 2A diagnostic
        locked = found;
    } else if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.insert(g_pending.end(), retry.begin(), retry.end());
    }
    // v0.4.14 (D): push back whatever we left untouched this tick.
    if (!remainder.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.insert(g_pending.begin(),
                         remainder.begin(), remainder.end());
    }

    // Re-validate the current lock every 4 ticks (~70 ms) and drop if
    // dead. Dropping is the safety net — the preempting switch above
    // handles the common case where a fresh Hero appears before the
    // old one is invalidated.
    if (locked && (n & 0x3) == 0) {
        if (!is_local_hero(locked)) {
            logf("hero_state: re-validation FAILED at tick %llu",
                 (unsigned long long)n);
            debug_dump_hero("invalid_locked", locked);

            std::uintptr_t fresh = try_relock_via_player();
            if (fresh) {
                g_locked_hero.store(fresh, std::memory_order_release);
                logf("hero_state: RE-LOCKED via Player.hero "
                     "@ 0x%llx (tick %llu)",
                     (unsigned long long)fresh, (unsigned long long)n);
            } else {
                g_locked_hero.store(0);
                g_locked_player.store(0);
                g_unlocked_alloc_log_left.store(16);
                logf("hero_state: DROPPED lock (no fresh Hero "
                     "available)");
            }
        }
    }

    // v0.4.14 (B): throttle the locked-Hero reads. When locked we ran
    // publish() every frame (60 Hz) — 8 mem_reads per tick for pos +
    // rot + inCombat. Cap to every 4th frame (15 Hz position update)
    // which is still completely smooth on the compass. When NOT locked
    // we still publish every tick to clear the snapshot quickly after
    // a lock drop.
    if (!locked || (n & 0x3) == 0) {
        publish();
    }

    // v0.4.15 anticrash arm/disarm. If the flag was set at boot and we
    // currently hold a lock, count consecutive stable ticks. After 5 s
    // (300 ticks) of uninterrupted lock, surgically remove the
    // hl_alloc_obj trampoline and stop damage tracking. From the next
    // tick onward the body's anticrash-post-disarm short-circuit at the
    // top kicks in and we poll Player.hero instead of watcher events.
    if (g_anticrash_on.load(std::memory_order_acquire) &&
        !g_anticrash_disarmed.load(std::memory_order_acquire)) {
        if (locked) {
            std::uint64_t stable = g_lock_stable_ticks.fetch_add(
                                       1, std::memory_order_relaxed) + 1;
            if (stable >= kAnticrashStableTicks) {
                logf("hero_state: anticrash trigger at tick %llu — "
                     "removing hl_alloc_obj hook and damage pipeline",
                     (unsigned long long)n);
                damage_stop();
                hl_hook_disable_alloc();
                g_anticrash_disarmed.store(true,
                                           std::memory_order_release);
            }
        } else {
            // Lost the lock before we got to disarm. Reset counter so
            // we wait another 5 s after the next stable lock.
            g_lock_stable_ticks.store(0, std::memory_order_relaxed);
        }
    }

    // Heartbeat: log current position every ~10 s so we can sanity-
    // check that the locked Hero is actually moving with the player.
    if (locked && (n % 600 == 0)) {
        logf("hero_state: pos=(%.1f, %.1f, %.1f) rot=%.3f (tick %llu)",
             g_snapshot.x, g_snapshot.y, g_snapshot.z, g_snapshot.rot_z,
             static_cast<unsigned long long>(n));
    }
}

void hero_state_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uintptr_t locked = g_locked_hero.load(std::memory_order_acquire);
    // v0.4.14 (F): SEH-wrap the body. If any of our mem_reads or
    // dyn-followed pointer chases trips an AV the structured exception
    // catches it; we count consecutive failures and auto-disable the
    // whole module after a threshold so the game stays alive even if
    // our reads keep tripping. The body cannot itself contain C++
    // objects with destructors directly, hence the separate function.
    __try {
        hero_state_tick_body(n, locked);
        g_consecutive_failures.store(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int fails = g_consecutive_failures.fetch_add(1) + 1;
        logf("hero_state: SEH trip #%d in tick %llu (code 0x%08lx)",
             fails, (unsigned long long)n,
             GetExceptionCode());
        if (fails >= kMaxConsecutiveFailures) {
            g_active.store(false);
            g_locked_hero.store(0);
            g_locked_player.store(0);
            logf("hero_state: %d consecutive SEH trips — auto-disabling "
                 "module to keep the game alive. Restart to re-enable.",
                 fails);
        }
    }
}

HeroSnapshot hero_state_read() { return g_snapshot; }

std::uintptr_t hero_state_locked_ptr() {
    return g_locked_hero.load(std::memory_order_acquire);
}

void hero_state_set_anticrash(bool on) {
    g_anticrash_on.store(on);
    if (!on) g_lock_stable_ticks.store(0);
}

bool hero_state_anticrash_armed()    { return g_anticrash_on.load(); }
bool hero_state_anticrash_disarmed() { return g_anticrash_disarmed.load(); }

}  // namespace farever
