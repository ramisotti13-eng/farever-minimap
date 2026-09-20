// Heal-event source (issue #57). Hooks st.skill.DamageResult directly (heals
// create no display object - see the phase-1 heal-probe diagnostic), keeps only
// effect==1 heals whose weakSource == the local player's UID (ent.Hero.__uid
// @+32), and emits HealEvents for the aggregator. Mirrors damage.cpp's
// queue/retry/SEH structure. Reads happen on the render thread.

#include "heal.h"
#include "hero_state.h"
#include "hl_hook.h"
#include "boss_names.h"   // boss_display_name (#87 target name)
#include "mem_scan.h"
#include "log.h"
#include "overlay.h"   // overlay_is_dps_tracking_paused

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace farever {
namespace {

constexpr std::size_t OFF_HERO_UID      = 32;   // ent.Hero.__uid (HI64)

// st.skill.DamageResult layout.
constexpr std::size_t OFF_DR_BASESKILL  = 8;
constexpr std::size_t OFF_DR_WEAKSOURCE = 40;
constexpr std::size_t OFF_DR_TARGET     = 48;   // *ent.GameObject (recipient) - #87
constexpr std::size_t OFF_DR_EFFECT     = 64;   // 0 = damage, 1 = heal
constexpr std::size_t OFF_DR_AMOUNT     = 88;
constexpr std::size_t OFF_DR_HITCOUNT   = 96;
constexpr std::size_t OFF_DR_CRITICAL   = 113;

constexpr std::size_t OFF_SKILL_KIND = 184;     // BaseSkill.kind (was 160) [v018 +16]
constexpr std::size_t OFF_UNIT_KIND  = 600;  // ent.Unit.kind (String) - #87 was 592 [v023 +8]
constexpr std::size_t OFF_STR_BYTES  = 8;
constexpr std::size_t OFF_STR_LEN    = 16;

constexpr std::int32_t kEffectHeal = 1;
constexpr int          kMaxRetries = 60;
constexpr std::size_t  kMaxPending = 256;
constexpr std::size_t  kEventRingMax = 2048;

std::atomic<bool>          g_active{false};
std::atomic<int>           g_seh_fails{0};
std::atomic<std::uint64_t> g_ticks{0};
std::atomic<std::uint64_t> g_emitted{0};   // cumulative heal events emitted

struct Pending { std::uintptr_t dr; int retries; };
std::mutex              g_pending_mu;
std::deque<Pending>     g_pending;

std::mutex              g_events_mu;
std::deque<HealEvent>   g_events;

// Cached local-player UID (= weakSource on our own events). Re-read each tick
// from the locked hero; 0 while unlocked (filter then drops everything).
std::uint64_t           g_my_uid = 0;
bool                    g_logged_uid = false;

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

// Read an ASCII Haxe String into `out`; non-ASCII becomes '?'. Mirrors
// damage.cpp's reader. False on bad pointer / empty.
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

// #87: heal recipient name, same form as farever.target.name() (ent.Unit.kind,
// boss class-name fallback). For self-heals this resolves to the local hero's
// kind. Empty on failure.
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

// true = settled (emitted or dropped), false = not populated yet (retry).
bool try_decode(std::uintptr_t dr, HealEvent* out) {
    std::int32_t hits = 0;
    if (!mem_read_i32(dr + OFF_DR_HITCOUNT, &hits)) return false;
    if (hits == 0) return false;                 // not settled yet
    if (hits < 1 || hits > 100000) return true;  // garbage, drop

    // Cheap discriminator first: only heals get further work.
    std::int32_t effect = 0;
    if (!mem_read_i32(dr + OFF_DR_EFFECT, &effect)) return false;
    if (effect != kEffectHeal) return true;      // damage / other, drop

    // Attribution: keep only heals cast by the local player.
    if (g_my_uid == 0) return true;              // no hero lock -> drop
    std::uint64_t weak = 0;
    if (!mem_read_u64(dr + OFF_DR_WEAKSOURCE, &weak)) return false;
    if (weak != g_my_uid) return true;           // someone else's heal, drop

    double amount = 0;
    if (!mem_read_f64(dr + OFF_DR_AMOUNT, &amount)) return false;
    if (!(amount > 0.0 && amount < 1e8)) return true;
    std::uint8_t crit = 0;
    mem_read_u8(dr + OFF_DR_CRITICAL, &crit);

    char skill[64];
    if (!decode_skill_name(dr, skill)) { std::strcpy(skill, "?"); }

    // #87: heal recipient name (self / ally) for FareverLogs.
    char target_name[64] = {0};
    {
        std::uint64_t tgt_u64 = 0;
        if (mem_read_u64(dr + OFF_DR_TARGET, &tgt_u64))
            resolve_target_name(static_cast<std::uintptr_t>(tgt_u64),
                                target_name);
    }

    out->dr_ptr    = dr;
    out->heal      = amount;
    out->hit_count = hits;
    out->is_crit   = crit ? 1 : 0;
    std::memcpy(out->skill, skill, sizeof(out->skill));
    std::memcpy(out->target_name, target_name, sizeof(out->target_name));
    return true;
}

void on_dr_alloc(std::uintptr_t dr) {
    if (!dr) return;
    if (!g_active.load(std::memory_order_acquire)) return;
    if (overlay_is_dps_tracking_paused()) return;
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.push_back({dr, 0});
    while (g_pending.size() > kMaxPending) g_pending.pop_front();
}

void tick_body() {
    // Heartbeat ~every 30 s @ 40 Hz so the log shows the probe is alive
    // and how many local-player heals it has emitted.
    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 1200 == 0) {
        logf("heal: heartbeat tick=%llu emitted=%llu",
             static_cast<unsigned long long>(n),
             static_cast<unsigned long long>(
                 g_emitted.load(std::memory_order_relaxed)));
    }

    // Refresh my UID from the locked hero.
    std::uintptr_t hero = hero_state_locked_ptr();
    if (hero) {
        std::uint64_t uid = 0;
        if (mem_read_u64(hero + OFF_HERO_UID, &uid)) {
            g_my_uid = uid;
            if (!g_logged_uid && uid != 0) {
                g_logged_uid = true;
                logf("heal: my_uid resolved = 0x%llx",
                     static_cast<unsigned long long>(uid));
            }
        }
    } else {
        g_my_uid = 0;
    }

    constexpr std::size_t kMaxPerTick = 12;
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

    std::vector<Pending>   retry;
    std::vector<HealEvent> emit;
    for (Pending p : work) {
        HealEvent ev{};
        if (try_decode(p.dr, &ev)) {
            if (ev.dr_ptr) emit.push_back(ev);   // dr_ptr set only on a kept heal
        } else if (++p.retries < kMaxRetries) {
            retry.push_back(p);
        }
    }
    if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        for (auto it = retry.rbegin(); it != retry.rend(); ++it)
            g_pending.push_front(*it);
    }
    if (!emit.empty()) {
        std::lock_guard<std::mutex> lk(g_events_mu);
        for (const HealEvent& ev : emit) {
            g_events.push_back(ev);
            while (g_events.size() > kEventRingMax) g_events.pop_front();
        }
        g_emitted.fetch_add(emit.size(), std::memory_order_relaxed);
    }
}

}  // namespace

void heal_start() {
    if (g_active.exchange(true)) return;
    g_my_uid = 0;
    g_logged_uid = false;
    g_ticks.store(0);
    g_emitted.store(0);
    { std::lock_guard<std::mutex> lk(g_pending_mu); g_pending.clear(); }
    { std::lock_guard<std::mutex> lk(g_events_mu);  g_events.clear();  }
    hl_hook_register(L"st.skill.DamageResult", on_dr_alloc);
    logf("heal: watcher registered on st.skill.DamageResult "
         "(effect==1 + weakSource==my UID)");
}

void heal_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    if (overlay_is_dps_tracking_paused()) return;
    __try {
        tick_body();
        g_seh_fails.store(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int f = g_seh_fails.fetch_add(1) + 1;
        logf("heal: SEH trip #%d (code 0x%08lx)", f, GetExceptionCode());
        if (f >= 5) {
            g_active.store(false);
            logf("heal: auto-disabled after %d SEH trips", f);
        }
    }
}

std::size_t heal_drain(HealEvent* out, std::size_t max) {
    if (!out || max == 0) return 0;
    std::lock_guard<std::mutex> lk(g_events_mu);
    std::size_t n = 0;
    while (n < max && !g_events.empty()) {
        out[n++] = g_events.front();
        g_events.pop_front();
    }
    return n;
}

}  // namespace farever
