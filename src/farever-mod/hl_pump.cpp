// v0.4.17 background HL tick driver. Backport of v0.6.0's
// hl_pump.cpp into the v0.4.16 legacy fallback line.
//
// HL-GC-invisible: plain Win32 thread, no hl_register_thread,
// no hl_blocking. The HashLink GC ignores this thread; stop-the-
// world sync on game-managed threads won't wait for us. We're a
// pure reader through SEH-protected accessors and type-tag-checked
// pointers.
//
// In v0.4.x this is OPT-OUT (default on). dllmain checks for
// data/no_worker.flag — if present we don't spawn the worker and
// the d3d12 Present hook drives ticks like in v0.4.16. That's the
// fallback for anyone who has trouble with the new path.
//
// Anticrash interaction: when data/anticrash.flag is set the
// hero_state module disarms the alloc-hook ~5 s after locking.
// The worker keeps ticking from there: hero_state_tick still polls
// Player.hero via the post-disarm code path, damage_tick is a no-op
// (damage_stop was already called). Effectively the worker just
// keeps publishing the hero snapshot. That's the desired behaviour.

#include "hl_pump.h"
#include "log.h"
#include "damage.h"
#include "hero_state.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace farever {
namespace {

LibHL                       g_libhl{};
std::thread                 g_thread;
std::atomic<bool>           g_stop{false};
std::atomic<bool>           g_running{false};
std::atomic<unsigned long>  g_worker_tid{0};

// 50 ms = 20 Hz. Matches v0.6.0 default.
constexpr auto kTickPeriod = std::chrono::microseconds(50'000);

void pump_thread_main() {
    g_worker_tid.store(GetCurrentThreadId(), std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    logf("hl_pump: worker live @ 20 Hz (TID=%lu, HL-GC-invisible)",
         GetCurrentThreadId());

    std::uint64_t tick_count = 0;
    auto next_tick = std::chrono::steady_clock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
        next_tick += kTickPeriod;
        std::this_thread::sleep_until(next_tick);
        auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now;

        damage_tick();
        hero_state_tick();

        if ((++tick_count) % 600 == 0) {  // every 30 s
            logf("hl_pump: heartbeat tick=%llu",
                 static_cast<unsigned long long>(tick_count));
        }
    }

    g_worker_tid.store(0, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
    logf("hl_pump: worker stopped after %llu ticks",
         static_cast<unsigned long long>(tick_count));
}

}  // namespace

bool hl_pump_start(const LibHL& libhl) {
    if (g_running.load(std::memory_order_acquire)) return true;
    g_libhl = libhl;
    g_stop.store(false, std::memory_order_release);
    g_thread = std::thread(pump_thread_main);
    logf("hl_pump: worker thread spawned (default-on; opt-out via "
         "data/no_worker.flag, HL-GC-invisible model)");
    return true;
}

void hl_pump_stop() {
    if (!g_running.load(std::memory_order_acquire)) return;
    g_stop.store(true, std::memory_order_release);
    if (g_thread.joinable()) g_thread.join();
}

bool hl_pump_is_active() {
    return g_running.load(std::memory_order_acquire);
}

unsigned long hl_pump_worker_tid() {
    return g_worker_tid.load(std::memory_order_acquire);
}

}  // namespace farever
