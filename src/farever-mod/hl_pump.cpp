// v0.6.0 background HL tick driver. Mirrors the worker-thread
// architecture of the Rust companion mod for the same game.
//
// **REVISED 2026-05-23 afternoon**: After two tests with hl_register_thread
// + hl_blocking discipline showed unstable game-freeze symptoms under
// combat, re-disassembly of Companion v0.1.5 confirmed they do NOT call
// hl_register_thread, hl_unregister_thread, or hl_blocking at all (the
// strings are entirely absent from their binary). Their worker is HL-
// GC-invisible: just a plain Win32/Rust thread reading game memory.
//
// We now match that model. No GC participation. Risk: GC can run while
// we're mid-read and collect / move objects under us. Mitigations we
// already have:
//   - SEH-wrapped mem_read_* catches AVs on freed pages
//   - Type-tag check on every cached pointer catches GC slot reuse
//   - Hero pointer is anchored via the hl_alloc_obj watcher
//
// Default poll rate is 20 Hz (50 ms period). Companion's settings.toml
// description suggests lower rates are safer for the hxbit-vs-our-reads
// hazard ("Lower = less CPU; higher = snappier reaction at fight
// start/end"). The hl_alloc_obj hook keeps producing damage events
// regardless - the poll rate only governs how often we read state out.
//
// This module is OPT-IN. dllmain starts it only when a data/hl_pump.flag
// file exists, so the default build behaves exactly like v0.5.6.1
// (Present-hook tick driver) and we don't regress on users who already
// run that path.

#include "hl_pump.h"
#include "log.h"
#include "damage.h"
#include "hero_state.h"
#include "party_state.h"
#include "camera_state.h"
#include "boss_timer.h"
#include "overlay.h"
#include "mob_watch.h"
#include "activity_icons.h"
#include "target_state.h"
#include "codex_state.h"
#include "anticheat.h"
#include "heal.h"
#include "poi_progress.h"
#include "poi_profile_key.h"
#include "plugins.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace farever {
namespace {

LibHL                       g_libhl{};
std::thread                 g_thread;
std::atomic<bool>           g_stop{false};
std::atomic<bool>           g_running{false};
std::atomic<unsigned long>  g_worker_tid{0};

// Period in microseconds. 25 ms = 40 Hz. Bumped from 20 Hz (50 ms) so
// target / HP / combat state and the hero position refresh faster; the
// minimap also smooths visually between ticks (render-thread lerp), so
// this mainly improves reaction latency. The SEH auto-disable in each
// tick body still guards against a runaway read storm.
constexpr auto kTickPeriod = std::chrono::microseconds(25'000);

void pump_thread_main() {
    // HL-GC-INVISIBLE worker (mirrors Companion). No hl_register_thread,
    // no hl_blocking. The HashLink GC ignores this thread; stop-the-
    // world sync on game-managed threads won't wait for us. We're
    // effectively just a Win32 thread that reads game memory through
    // SEH-protected accessors and type-tag-checked pointers.
    //
    // The previous version called hl_register_thread + hl_blocking(true/
    // false) discipline (the libhl-embedded threading pattern). That
    // pattern is correct for code that ALLOCATES Haxe values from a
    // worker. We only READ, so the registration was unnecessary - and
    // it was load-bearing in the wrong direction: under combat the
    // hl_blocking transitions appeared to interact poorly with hxbit
    // deserialise and froze the game (worker + overlay + game all
    // hung). See 2026-05-23 evening logs at PID=2348.

    g_worker_tid.store(GetCurrentThreadId(), std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    logf("hl_pump: worker live @ 40 Hz (TID=%lu, HL-GC-invisible)",
         GetCurrentThreadId());

    std::uint64_t tick_count = 0;
    auto next_tick = std::chrono::steady_clock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
        next_tick += kTickPeriod;
        std::this_thread::sleep_until(next_tick);
        auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now;  // catch up after stall

        // Issue #54: AC appeared mid-session -> stop reading game state.
        if (anticheat_is_disabled()) continue;

        damage_tick();
        hero_state_tick();
        target_state_tick();
        codex_state_tick();  // issue #44 phase-1 diagnostic (no-op once dumped)
        heal_tick();         // issue #57 - local-player heal source
        party_state_tick();  // party members chase
        camera_state_tick();  // #65 camera-relative compass (camera yaw)
        boss_timer_tick();    // boss speedrun mode
        mob_watch_tick();     // #65 rare/sparkling-mob proximity alert
        // #107: write out newly learned activity icon cells. No-op unless
        // something changed, so it is a bool test on almost every tick.
        if (tick_count % 200 == 0) activity_icons_flush();

        // #62: pick the per-character POI profile once the hero is locked.
        // Throttled to ~1 s. Debounced: only switch once the SAME key has been
        // read on 3 consecutive checks (~3 s), so a transient bad name during a
        // zone/character transition (e.g. a placeholder hero briefly named "1")
        // never creates a spurious profile or mis-loads. set_profile itself is
        // a cheap no-op while the active key is unchanged.
        if (tick_count % 40 == 0) {
            std::string cname;
            if (hero_state_local_name(cname)) {
                std::string key = poi_sanitize_profile_key(cname);
                static std::string s_pending_key;
                static int         s_pending_count = 0;
                if (key == s_pending_key) {
                    if (s_pending_count < 3 && ++s_pending_count == 3) {
                        poi_progress_set_profile(key.c_str());
                        boss_timer_set_profile(key.c_str());
                        overlay_set_profile(key.c_str());   // #65 per-character UI
                        // #109: same debounced key for plugins, so their
                        // per-character state switches with ours.
                        plugins_set_character(key.c_str(), cname.c_str());
                        // Drop what we cached about the previous character.
                        // Without this the new one inherits its gold,
                        // equipment and class until each of those happens to
                        // read non-empty. Reported by @dpiza for currencies.
                        hero_state_reset_caches();
                    }
                } else {
                    s_pending_key   = key;
                    s_pending_count = 1;
                }
            }
        }

        if ((++tick_count) % 1200 == 0) {  // every 30 s @ 40 Hz
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
    // No threading-API requirement anymore - the HL-GC-invisible model
    // doesn't call hl_register_thread / hl_blocking. We just need a
    // running game process.
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
