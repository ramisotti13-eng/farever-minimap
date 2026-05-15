// In-process Hero scanner. Port of tools/find_me.py to C++ — runs on a
// background thread, walks committed RW regions in our own process,
// matches the ent.Hero signature (4 plausible doubles + 4 userland
// pointers), then validates via Hero.ownerPlayer.isMe and the
// bidirectional Player.hero == Hero check.
//
// Multi-threaded variant: a coordinator thread snapshots the region
// list, then up to 4 worker threads claim regions from a shared atomic
// counter. First worker to validate a Hero stores it via CAS and the
// rest bail out.

#include "hero_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

namespace farevermod {
namespace {

// --- Memory layout constants (from research/hero-layout.md) ---------

constexpr std::size_t kHeroSize           = 1584;
constexpr std::size_t OFF_HLTYPE          = 0;
constexpr std::size_t OFF_REMOVED         = 8;
constexpr std::size_t OFF_OWNERPLAYER     = 16;
constexpr std::size_t OFF_HOST            = 48;
constexpr std::size_t OFF_POSX            = 144;
constexpr std::size_t OFF_POSY            = 152;
constexpr std::size_t OFF_POSZ            = 160;
constexpr std::size_t OFF_ROTZ            = 168;
constexpr std::size_t OFF_POSITION        = 176;
constexpr std::size_t OFF_HERO_IN_PLAYER  = 272;
constexpr std::size_t OFF_ISME            = 280;

// Plausible world-coordinate ranges (W1 spans ~6 tiles, each 256 m).
constexpr double      RX_LO   = -10000.0, RX_HI   = 10000.0;
constexpr double      RY_LO   = -10000.0, RY_HI   = 10000.0;
constexpr double      RZ_LO   =   -500.0, RZ_HI   =  1500.0;
constexpr double      RROT_LO =     -7.0, RROT_HI =     7.0;

// User-land pointer range: HashLink heap is above 4 GB on Windows x64;
// the upper limit is the architecturally-mandated 47-bit user-mode cap.
constexpr std::uintptr_t USERLAND_LO = 0x0000000100000000ULL;
constexpr std::uintptr_t USERLAND_HI = 0x00007FFFFFFFFFFFULL;

// --- State ---------------------------------------------------------

std::atomic<bool>           g_thread_running{false};
std::atomic<bool>           g_locked{false};
std::atomic<bool>           g_failed{false};
std::atomic<std::uintptr_t> g_hero_addr{0};
std::atomic<DWORD>          g_scan_end_tick{0};
std::thread                 g_thread;

// --- SEH helpers ---------------------------------------------------

int seh_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uint64_t*>(addr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int seh_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const std::uint8_t*>(addr);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int seh_read_4_doubles(std::uintptr_t addr, double out[4]) {
    __try {
        out[0] = *reinterpret_cast<const double*>(addr + 0);
        out[1] = *reinterpret_cast<const double*>(addr + 8);
        out[2] = *reinterpret_cast<const double*>(addr + 16);
        out[3] = *reinterpret_cast<const double*>(addr + 24);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// --- Candidate filter ----------------------------------------------

bool is_userland(std::uintptr_t v) {
    return v >= USERLAND_LO && v <= USERLAND_HI;
}

bool in_range(double v, double lo, double hi) {
    return v >= lo && v <= hi && !std::isnan(v) && !std::isinf(v);
}

// Cheap structural check. Filters ordered cheapest-first so most
// offsets reject after one 32-byte read.
bool looks_like_hero(std::uintptr_t hero_addr) {
    double pos4[4];
    if (!seh_read_4_doubles(hero_addr + OFF_POSX, pos4)) return false;
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
    if (!seh_read_u64(hero_addr + OFF_HLTYPE,      &htype))    return false;
    if (!is_userland(htype))   return false;
    if (!seh_read_u64(hero_addr + OFF_OWNERPLAYER, &owner))    return false;
    if (!is_userland(owner))   return false;
    if (!seh_read_u64(hero_addr + OFF_HOST,        &host))     return false;
    if (!is_userland(host))    return false;
    if (!seh_read_u64(hero_addr + OFF_POSITION,    &posp))     return false;
    if (!is_userland(posp))    return false;
    if (!seh_read_u8 (hero_addr + OFF_REMOVED,     &removed))  return false;
    if (removed > 1)           return false;
    return true;
}

// Bidirectional validation: candidate must be the local player.
bool is_local_hero(std::uintptr_t hero_addr) {
    std::uint64_t owner = 0;
    if (!seh_read_u64(hero_addr + OFF_OWNERPLAYER, &owner)) return false;
    if (!is_userland(owner)) return false;
    std::uint64_t player_hero = 0;
    if (!seh_read_u64(owner + OFF_HERO_IN_PLAYER, &player_hero)) return false;
    if (player_hero != hero_addr) return false;
    std::uint8_t is_me = 0;
    if (!seh_read_u8(owner + OFF_ISME, &is_me)) return false;
    return is_me == 1;
}

// --- Scan loop (parallel) ------------------------------------------

struct Region {
    std::uintptr_t base;
    std::size_t    size;
};

// One-shot region snapshot — done once on the coordinator thread so the
// worker pool sees a stable list.
std::vector<Region> collect_regions() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto cur = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<Region> out;
    out.reserve(8192);
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)) == 0)
            break;
        auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        auto size = static_cast<std::size_t>(mbi.RegionSize);
        if (size == 0) { cur += 4096; continue; }
        cur = base + size;
        if (mbi.State != MEM_COMMIT) continue;
        if (size < (64 * 1024)) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable) == 0) continue;
        // Skip huge mapped sections (>= 1 GB): HashLink GC chunks are at
        // most a few hundred MB; gigantic regions are usually file
        // mappings / textures and bog the scan down.
        if (size > (1ULL << 30)) continue;
        out.push_back({base, size});
    }
    return out;
}

void scan_thread() {
    DWORD t_start = GetTickCount();
    auto regions = collect_regions();
    std::size_t total_bytes = std::accumulate(
        regions.begin(), regions.end(), std::size_t{0},
        [](std::size_t a, const Region& r) { return a + r.size; });
    logf("hero_scan: %zu candidate regions, %.0f MB total",
         regions.size(), total_bytes / (1024.0 * 1024.0));

    // Worker pool size: leave a core for the game / render threads.
    // First worker to find a valid Hero wins via a CAS into found_addr;
    // others see it and return.
    unsigned int hw = std::thread::hardware_concurrency();
    unsigned int n_workers =
        (hw <= 2) ? 1u : (hw <= 4) ? 2u : (hw <= 8) ? 3u : 4u;

    std::atomic<std::size_t>    next_region{0};
    std::atomic<std::uintptr_t> found_addr{0};
    std::atomic<std::size_t>    regions_done{0};

    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (unsigned int w = 0; w < n_workers; ++w) {
        workers.emplace_back([&]() {
            while (g_thread_running.load(std::memory_order_acquire) &&
                   found_addr.load(std::memory_order_acquire) == 0) {
                std::size_t idx = next_region.fetch_add(
                    1, std::memory_order_relaxed);
                if (idx >= regions.size()) break;
                const Region& r = regions[idx];
                std::size_t max_off =
                    (r.size > kHeroSize) ? (r.size - kHeroSize) : 0;
                for (std::size_t off = 0; off <= max_off; off += 8) {
                    if ((off & 0xFFFF) == 0) {
                        if (!g_thread_running.load(
                                std::memory_order_acquire) ||
                            found_addr.load(std::memory_order_acquire) != 0)
                            return;
                    }
                    std::uintptr_t cand = r.base + off;
                    if (!looks_like_hero(cand)) continue;
                    if (!is_local_hero(cand))   continue;
                    std::uintptr_t exp = 0;
                    found_addr.compare_exchange_strong(
                        exp, cand, std::memory_order_acq_rel);
                    return;  // either we won, or someone else did
                }
                regions_done.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : workers) t.join();

    std::uintptr_t found = found_addr.load(std::memory_order_acquire);
    DWORD elapsed_ms = GetTickCount() - t_start;
    if (found) {
        g_hero_addr.store(found, std::memory_order_release);
        g_locked.store(true);
        logf("hero_scan: locked Hero @ 0x%llx "
             "(workers=%u, regions=%zu/%zu, %.0f MB, %lu ms)",
             (unsigned long long)found, n_workers,
             regions_done.load(std::memory_order_relaxed),
             regions.size(),
             total_bytes / (1024.0 * 1024.0), elapsed_ms);
    } else {
        g_failed.store(true);
        logf("hero_scan: no local Hero found "
             "(workers=%u, regions=%zu, %.0f MB, %lu ms)",
             n_workers, regions.size(),
             total_bytes / (1024.0 * 1024.0), elapsed_ms);
    }
    g_scan_end_tick.store(GetTickCount(), std::memory_order_release);
    g_thread_running.store(false);
}

}  // namespace

// --- Public API -----------------------------------------------------

void hero_scan_start() {
    if (g_thread_running.exchange(true)) return;
    if (g_thread.joinable()) g_thread.join();
    g_locked.store(false);
    g_failed.store(false);
    g_hero_addr.store(0);
    g_thread = std::thread(scan_thread);
}

void hero_scan_stop() {
    g_thread_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

bool hero_scan_locked() { return g_locked.load(std::memory_order_acquire); }
bool hero_scan_failed() { return g_failed.load(std::memory_order_acquire); }

LivePosition hero_scan_read() {
    LivePosition lp{};
    std::uintptr_t a = g_hero_addr.load(std::memory_order_acquire);
    if (a == 0) {
        // No lock. Two reasons we might want to start a fresh scan:
        //   - The previous scan FAILED (player not in the world yet).
        //     Retry, but only every few seconds so we don't burn CPU.
        //   - The validation handler below kicked the lock — that path
        //     already calls hero_scan_start() directly, so we don't
        //     need to also restart from here.
        if (g_failed.load() && !g_thread_running.load()) {
            DWORD now = GetTickCount();
            if (now - g_scan_end_tick.load(std::memory_order_acquire)
                    > 5000UL) {
                hero_scan_start();
            }
        }
        return lp;
    }

    // Validate the lock every ~64 frames. After a dungeon transition
    // the game often keeps the OLD Hero memory mapped but flips its
    // `Player.isMe` to 0 — we'd happily keep reading frozen coordinates
    // without this check.
    static std::atomic<int> validate_tick{0};
    int vt = validate_tick.fetch_add(1, std::memory_order_relaxed);
    if ((vt & 0x3F) == 0) {
        if (!is_local_hero(a)) {
            g_hero_addr.store(0);
            g_locked.store(false);
            logf("hero_scan_read: validation failed (isMe/bidir), "
                 "restarting scan");
            hero_scan_start();
            return lp;
        }
    }

    double pos4[4];
    if (!seh_read_4_doubles(a + OFF_POSX, pos4)) {
        g_hero_addr.store(0);
        g_locked.store(false);
        logf("hero_scan_read: pointer went stale, restarting scan");
        hero_scan_start();
        return lp;
    }
    lp.x     = pos4[0];
    lp.y     = pos4[1];
    lp.z     = pos4[2];
    lp.rot_z = pos4[3];
    lp.valid = true;
    return lp;
}

}  // namespace farevermod
