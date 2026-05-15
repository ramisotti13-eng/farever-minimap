// Local-Hero locator, slimmed-down port of minimap-dll/hero_scan.cpp.
// We don't need pos / rot here; we only read combat fields. Structural
// fingerprint + Player.isMe + bidirectional Player.hero == Hero check
// is identical — that's the only reliable way to pick the local player
// out of the dozens of Hero instances the client keeps live.

#include "hero_state.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

namespace dpsmeter {
namespace {

// --- ent.Hero offsets (from minimap research; same as Python's) -----

constexpr std::size_t kHeroSize       = 1584;
constexpr std::size_t OFF_HLTYPE      = 0;
constexpr std::size_t OFF_REMOVED     = 8;
constexpr std::size_t OFF_OWNERPLAYER = 16;
constexpr std::size_t OFF_HOST        = 48;
constexpr std::size_t OFF_POSX        = 144;
constexpr std::size_t OFF_POSITION    = 176;
constexpr std::size_t OFF_HERO_IN_PLAYER = 272;
constexpr std::size_t OFF_ISME        = 280;

// Combat fields — what the aggregator actually consumes.
constexpr std::size_t OFF_IS_IN_COMBAT = 672;
constexpr std::size_t OFF_COMBAT_START = 688;
constexpr std::size_t OFF_COMBAT_ID    = 712;

// Plausible world-coordinate ranges (W1 ≈ 6 tiles × 256 m).
constexpr double RX_LO   = -10000.0, RX_HI   = 10000.0;
constexpr double RY_LO   = -10000.0, RY_HI   = 10000.0;
constexpr double RZ_LO   =   -500.0, RZ_HI   =  1500.0;
constexpr double RROT_LO =     -7.0, RROT_HI =     7.0;

bool in_range(double v, double lo, double hi) {
    return v >= lo && v <= hi && !std::isnan(v) && !std::isinf(v);
}

bool looks_like_hero(std::uintptr_t hero_addr) {
    double pos4[4];
    if (!mem_read_bytes(hero_addr + OFF_POSX, pos4, sizeof(pos4))) return false;
    double x = pos4[0], y = pos4[1], z = pos4[2], r = pos4[3];
    if (!in_range(x, RX_LO,   RX_HI))   return false;
    if (!in_range(y, RY_LO,   RY_HI))   return false;
    if (!in_range(z, RZ_LO,   RZ_HI))   return false;
    if (!in_range(r, RROT_LO, RROT_HI)) return false;
    if (std::fabs(x) < 0.01 && std::fabs(y) < 0.01) return false;
    if (std::fabs(x) < 10.0 && std::fabs(y) < 10.0 && std::fabs(z) < 10.0) {
        const double eps = 1e-6;
        if (std::fabs(x - std::round(x)) < eps &&
            std::fabs(y - std::round(y)) < eps &&
            std::fabs(z - std::round(z)) < eps) {
            return false;
        }
    }

    std::uint64_t htype, owner, host, posp;
    std::uint8_t  removed;
    if (!mem_read_u64(hero_addr + OFF_HLTYPE,      &htype))   return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(htype))) return false;
    if (!mem_read_u64(hero_addr + OFF_OWNERPLAYER, &owner))   return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(owner))) return false;
    if (!mem_read_u64(hero_addr + OFF_HOST,        &host))    return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(host)))  return false;
    if (!mem_read_u64(hero_addr + OFF_POSITION,    &posp))    return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(posp)))  return false;
    if (!mem_read_u8 (hero_addr + OFF_REMOVED,     &removed)) return false;
    if (removed > 1) return false;
    return true;
}

bool is_local_hero(std::uintptr_t hero_addr) {
    std::uint64_t owner = 0;
    if (!mem_read_u64(hero_addr + OFF_OWNERPLAYER, &owner)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(owner))) return false;
    std::uint64_t player_hero = 0;
    if (!mem_read_u64(static_cast<std::uintptr_t>(owner) + OFF_HERO_IN_PLAYER,
                      &player_hero)) return false;
    if (player_hero != hero_addr) return false;
    std::uint8_t is_me = 0;
    if (!mem_read_u8(static_cast<std::uintptr_t>(owner) + OFF_ISME, &is_me))
        return false;
    return is_me == 1;
}

// --- state ----------------------------------------------------------

std::atomic<bool>           g_running{false};
std::atomic<bool>           g_locked{false};
std::atomic<bool>           g_failed{false};
std::atomic<std::uintptr_t> g_hero_addr{0};
std::atomic<DWORD>          g_scan_end_tick{0};
std::thread                 g_thread;

void scan_thread() {
    DWORD t_start = GetTickCount();
    auto regions = mem_collect_regions();
    std::size_t total_bytes = std::accumulate(
        regions.begin(), regions.end(), std::size_t{0},
        [](std::size_t a, const Region& r) { return a + r.size; });
    logf("hero_state: %zu candidate regions, %.0f MB total",
         regions.size(), total_bytes / (1024.0 * 1024.0));

    unsigned int hw = std::thread::hardware_concurrency();
    unsigned int n_workers =
        (hw <= 2) ? 1u : (hw <= 4) ? 2u : (hw <= 8) ? 3u : 4u;

    std::atomic<std::size_t>    next_region{0};
    std::atomic<std::uintptr_t> found_addr{0};

    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (unsigned int w = 0; w < n_workers; ++w) {
        workers.emplace_back([&]() {
            while (g_running.load(std::memory_order_acquire) &&
                   found_addr.load(std::memory_order_acquire) == 0) {
                std::size_t idx = next_region.fetch_add(
                    1, std::memory_order_relaxed);
                if (idx >= regions.size()) break;
                const Region& r = regions[idx];
                std::size_t max_off =
                    (r.size > kHeroSize) ? (r.size - kHeroSize) : 0;
                for (std::size_t off = 0; off <= max_off; off += 8) {
                    if ((off & 0xFFFF) == 0) {
                        if (!g_running.load(std::memory_order_acquire) ||
                            found_addr.load(std::memory_order_acquire) != 0)
                            return;
                    }
                    std::uintptr_t cand = r.base + off;
                    if (!looks_like_hero(cand)) continue;
                    if (!is_local_hero(cand))   continue;
                    std::uintptr_t exp = 0;
                    found_addr.compare_exchange_strong(
                        exp, cand, std::memory_order_acq_rel);
                    return;
                }
            }
        });
    }
    for (auto& t : workers) t.join();

    std::uintptr_t found = found_addr.load(std::memory_order_acquire);
    DWORD elapsed_ms = GetTickCount() - t_start;
    if (found) {
        g_hero_addr.store(found, std::memory_order_release);
        g_locked.store(true);
        logf("hero_state: locked Hero @ 0x%llx (workers=%u, %lu ms)",
             static_cast<unsigned long long>(found), n_workers, elapsed_ms);
    } else {
        g_failed.store(true);
        logf("hero_state: no local Hero found (workers=%u, %lu ms)",
             n_workers, elapsed_ms);
    }
    g_scan_end_tick.store(GetTickCount(), std::memory_order_release);
    g_running.store(false);
}

}  // namespace

void hero_state_start() {
    if (g_running.exchange(true)) return;
    if (g_thread.joinable()) g_thread.join();
    g_locked.store(false);
    g_failed.store(false);
    g_hero_addr.store(0);
    g_thread = std::thread(scan_thread);
}

void hero_state_stop() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

bool hero_state_locked() { return g_locked.load(std::memory_order_acquire); }
bool hero_state_failed() { return g_failed.load(std::memory_order_acquire); }

HeroSnapshot hero_state_read() {
    HeroSnapshot s{};
    std::uintptr_t a = g_hero_addr.load(std::memory_order_acquire);
    if (a == 0) {
        // Retry scan every 5 s if we previously failed and aren't already
        // scanning. World might not have been loaded yet.
        if (g_failed.load() && !g_running.load()) {
            DWORD now = GetTickCount();
            if (now - g_scan_end_tick.load(std::memory_order_acquire)
                    > 5000UL) {
                hero_state_start();
            }
        }
        return s;
    }

    // Re-validate every 64 reads. Dungeon transitions etc. invalidate
    // the cached pointer without faulting it (see feedback memory).
    static std::atomic<int> validate_tick{0};
    int vt = validate_tick.fetch_add(1, std::memory_order_relaxed);
    if ((vt & 0x3F) == 0) {
        if (!is_local_hero(a)) {
            g_hero_addr.store(0);
            g_locked.store(false);
            logf("hero_state: validation failed, restarting scan");
            hero_state_start();
            return s;
        }
    }

    std::int32_t  combat_id    = 0;
    double        combat_start = 0.0;
    std::uint8_t  in_combat    = 0;
    if (!mem_read_i32(a + OFF_COMBAT_ID,    &combat_id))    return s;
    if (!mem_read_f64(a + OFF_COMBAT_START, &combat_start)) return s;
    if (!mem_read_u8 (a + OFF_IS_IN_COMBAT, &in_combat))    return s;
    s.valid        = true;
    s.combat_id    = combat_id;
    s.combat_start = combat_start;
    s.is_in_combat = in_combat;
    return s;
}

}  // namespace dpsmeter
