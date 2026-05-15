// Background scanner — direct port of tools/dpsmeter_live.py's scan_dd
// + skill-name decoding + dedupe, running in-process via SEH-wrapped
// reads. See research/m0-findings.md for the offset rationale.
//
// Flow:
//   1. Wait/retry until type_anchor_find succeeds (world is loaded).
//   2. Snapshot all heap regions; thereafter narrow to "hot" regions
//      where the previous scan saw at least one DamageDisplay hit.
//   3. ~10 Hz scan loop: for every u64 == tag, read DD.damage / .crit
//      / .dmgPtr; dedupe by DR ptr; read DR.hitCount / .kill; decode
//      DR.baseSkill.kind into an ASCII skill name; reject garbage and
//      push the event onto an internal queue.
//
// The internal queue is drained by the render thread via the lock-
// protected damage_scan_drain() — keeps the scan loop independent of
// the UI frame cadence.

#include "damage_scan.h"
#include "mem_scan.h"
#include "type_anchor.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dpsmeter {
namespace {

// --- offsets (see dpsmeter/research/m0-findings.md) -----------------

// ui.comp.DamageDisplay (1184 bytes)
constexpr std::size_t DD_SIZE        = 1184;
constexpr std::size_t OFF_DD_DAMAGE  = 1136;  // f64
constexpr std::size_t OFF_DD_ISCRIT  = 1144;  // u8
constexpr std::size_t OFF_DD_DMG_PTR = 1176;  // *DamageResult

// st.skill.DamageResult (136 bytes)
constexpr std::size_t OFF_DR_BASESKILL = 8;
constexpr std::size_t OFF_DR_HITCOUNT  = 88;   // i32
constexpr std::size_t OFF_DR_KILL      = 104;  // u8

// st.skill.Skill / BaseSkill: kind:String at +152
constexpr std::size_t OFF_SKILL_KIND = 152;

// hl_string layout: bytes:*u16 @ +8, length:i32 @ +16 (chars, not bytes)
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

// --- queue / state --------------------------------------------------

std::atomic<bool>          g_running{false};
std::atomic<bool>          g_ready{false};
std::atomic<std::uintptr_t> g_type_tag{0};
std::atomic<std::uint64_t>  g_poll_count{0};
std::atomic<std::uint32_t>  g_last_scan_ms{0};
std::atomic<std::uint32_t>  g_hot_count{0};
std::thread                 g_thread;

std::mutex                    g_state_mu;
std::unordered_set<std::uintptr_t> g_seen_dr;   // guarded by g_state_mu
std::deque<DamageEvent>       g_pending;        // guarded by g_state_mu

// Hot-region cache. Owned exclusively by the worker thread, so no lock.
std::vector<Region>           g_all_regions;
std::vector<Region>           g_hot_regions;

constexpr std::size_t kMaxPending = 4096;

// --- skill name decoding -------------------------------------------

// Read the Haxe skill kind string (DR.baseSkill.kind) and convert to
// printable ASCII. Returns false if any pointer is bogus or the name
// contains non-printable characters (those are garbage DRs).
bool decode_skill_name(std::uintptr_t dr_ptr, char out[64]) {
    out[0] = 0;

    std::uint64_t bs_u64 = 0;
    if (!mem_read_u64(dr_ptr + OFF_DR_BASESKILL, &bs_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bs_u64))) return false;
    auto bs = static_cast<std::uintptr_t>(bs_u64);

    std::uint64_t skind_u64 = 0;
    if (!mem_read_u64(bs + OFF_SKILL_KIND, &skind_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(skind_u64))) return false;
    auto skind = static_cast<std::uintptr_t>(skind_u64);

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

    // UTF-16 → ASCII; reject anything outside [A-Za-z0-9_.].
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

// --- scan iteration ------------------------------------------------

void scan_iteration() {
    std::uintptr_t tag = g_type_tag.load(std::memory_order_acquire);
    if (!tag) return;

    const std::vector<Region>& regions =
        g_hot_regions.empty() ? g_all_regions : g_hot_regions;

    std::vector<Region> new_hot;
    new_hot.reserve(8);

    ScanBuf buf;
    DWORD t_start = GetTickCount();

    for (const Region& r : regions) {
        if (!mem_scan_u64(r.base, r.size,
                          static_cast<std::uint64_t>(tag), &buf)) continue;
        if (buf.count == 0) continue;
        new_hot.push_back(r);

        for (std::size_t i = 0; i < buf.count; ++i) {
            std::uintptr_t dd_addr = buf.hits[i];

            double damage = 0;
            if (!mem_read_f64(dd_addr + OFF_DD_DAMAGE, &damage)) continue;
            if (!(damage > 0.0 && damage < 1e8)) continue;

            std::uint8_t crit = 0;
            if (!mem_read_u8(dd_addr + OFF_DD_ISCRIT, &crit)) continue;
            if (crit > 1) continue;

            std::uint64_t dr_u64 = 0;
            if (!mem_read_u64(dd_addr + OFF_DD_DMG_PTR, &dr_u64)) continue;
            if (!mem_is_userland(static_cast<std::uintptr_t>(dr_u64))) continue;
            auto dr_ptr = static_cast<std::uintptr_t>(dr_u64);

            // Dedupe BEFORE the expensive skill read.
            {
                std::lock_guard<std::mutex> lk(g_state_mu);
                if (g_seen_dr.count(dr_ptr)) continue;
            }

            std::int32_t hits = 1;
            if (!mem_read_i32(dr_ptr + OFF_DR_HITCOUNT, &hits)) continue;
            if (hits < 1 || hits > 50) continue;

            std::uint8_t kill = 0;
            mem_read_u8(dr_ptr + OFF_DR_KILL, &kill);

            char skill[64];
            if (!decode_skill_name(dr_ptr, skill)) continue;

            DamageEvent ev{};
            ev.dr_ptr    = dr_ptr;
            ev.damage    = damage;
            ev.hit_count = hits;
            ev.is_crit   = crit;
            ev.is_kill   = kill ? 1 : 0;
            std::memcpy(ev.skill, skill, sizeof(ev.skill));

            {
                std::lock_guard<std::mutex> lk(g_state_mu);
                if (g_seen_dr.insert(dr_ptr).second) {
                    g_pending.push_back(ev);
                    while (g_pending.size() > kMaxPending) {
                        g_pending.pop_front();
                    }
                }
            }
        }
    }

    g_last_scan_ms.store(GetTickCount() - t_start,
                         std::memory_order_relaxed);
    g_poll_count.fetch_add(1, std::memory_order_relaxed);

    // Warm/extend the hot cache. Union semantics keep regions that
    // happen to be empty between polls but had hits at some point.
    if (!new_hot.empty()) {
        if (g_hot_regions.empty()) {
            g_hot_regions = std::move(new_hot);
        } else {
            for (const Region& nr : new_hot) {
                bool present = false;
                for (const Region& er : g_hot_regions) {
                    if (er.base == nr.base) { present = true; break; }
                }
                if (!present) g_hot_regions.push_back(nr);
            }
        }
        g_hot_count.store(
            static_cast<std::uint32_t>(g_hot_regions.size()),
            std::memory_order_relaxed);
    }
}

// --- worker thread --------------------------------------------------

void sleep_chunked(int ms) {
    // Sleep in 100 ms chunks so stop() responds within a frame.
    while (ms > 0 && g_running.load(std::memory_order_acquire)) {
        int slice = ms < 100 ? ms : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        ms -= slice;
    }
}

void thread_main() {
    logf("damage_scan: thread starting");

    // 1) Anchor — retry every 5 s until the world is loaded.
    std::uintptr_t tag = 0;
    while (g_running.load(std::memory_order_acquire)) {
        if (type_anchor_find(L"ui.comp.DamageDisplay", &tag)) {
            g_type_tag.store(tag, std::memory_order_release);
            break;
        }
        logf("damage_scan: anchor not yet resident, retrying in 5 s");
        sleep_chunked(5000);
    }
    if (!g_running.load(std::memory_order_acquire)) {
        logf("damage_scan: stop requested before anchor; exiting");
        return;
    }

    // 2) Snapshot regions (the scan loop will narrow to hot regions).
    g_all_regions = mem_collect_regions();
    logf("damage_scan: %zu regions snapshotted, beginning scan loop",
         g_all_regions.size());
    g_ready.store(true, std::memory_order_release);

    // 3) Scan loop — ~10 Hz.
    while (g_running.load(std::memory_order_acquire)) {
        scan_iteration();
        sleep_chunked(100);
    }

    logf("damage_scan: thread exiting after %llu polls",
         static_cast<unsigned long long>(
             g_poll_count.load(std::memory_order_relaxed)));
}

}  // namespace

// --- Public API -----------------------------------------------------

void damage_scan_start() {
    if (g_running.exchange(true)) return;
    if (g_thread.joinable()) g_thread.join();
    g_ready.store(false);
    g_type_tag.store(0);
    g_poll_count.store(0);
    g_last_scan_ms.store(0);
    g_hot_count.store(0);
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        g_seen_dr.clear();
        g_pending.clear();
    }
    g_all_regions.clear();
    g_hot_regions.clear();
    g_thread = std::thread(thread_main);
}

void damage_scan_stop() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

bool damage_scan_ready() {
    return g_ready.load(std::memory_order_acquire);
}

std::size_t damage_scan_drain(DamageEvent* out, std::size_t max) {
    if (!out || max == 0) return 0;
    std::lock_guard<std::mutex> lk(g_state_mu);
    std::size_t n = 0;
    while (n < max && !g_pending.empty()) {
        out[n++] = g_pending.front();
        g_pending.pop_front();
    }
    return n;
}

ScanStats damage_scan_stats() {
    ScanStats s{};
    s.poll_count   = g_poll_count.load(std::memory_order_relaxed);
    s.last_scan_ms = g_last_scan_ms.load(std::memory_order_relaxed);
    s.hot_regions  = g_hot_count.load(std::memory_order_relaxed);
    s.type_tag     = g_type_tag.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        s.unique_drs = static_cast<std::uint64_t>(g_seen_dr.size());
    }
    return s;
}

}  // namespace dpsmeter
