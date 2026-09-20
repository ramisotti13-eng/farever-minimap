// Hero.target tracker (Slice 1 of the boss-mechanic plugin surface).
//
// Each frame, after hero_state_tick has run, we read Hero.target
// (HI64 at offset 640 on ent.Unit - the field is declared HI64 for
// hxbit replication but holds a runtime pointer to the targeted
// Unit). The candidate is validated:
//
//   - userland pointer
//   - type tag at offset 0 matches one of our anchor types
//     (currently ent.Hero, ent.Foe)
//
// On success we read the unit's `kind` String (offset 584) into the
// snapshot. That's the internal id string (e.g. "Wolf_Boss_01"), not
// the localized name - chase that later through the `inf` virtual.
//
// Whole tick is SEH-wrapped so a half-constructed target can't take
// down Present. On repeated trips we self-disable like hero_state.

#include "target_state.h"
#include "hero_state.h"
#include "hl_hook.h"
#include "boss_names.h"   // is_boss_type / boss_display_name (#69 fallback)
#include "boss_target.h"  // boss_target_recent (#69 fallback)
#include "hl_pump.h"
#include "mem_scan.h"
#include "log.h"
#include "plugins.h"
#include "uid_registry.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace farever {
namespace {

// ent.Unit / ent.Hero offsets (classes_v0152.json game v0.1.5.25921).
// v0.5.5 game update added sleeping @ 138 + accDt @ 144 in ent.Serializable
// / GameObject base, shifting every inherited Unit/Hero/GameObject field
// at offset >= 144 by +8 bytes. Unit.host @ 48 sits above the shift point
// and is unchanged.
constexpr std::size_t OFF_UNIT_HOST          = 48;    // HOBJ:hxbit.NetworkHost (unchanged)
constexpr std::size_t OFF_GAMEOBJECT_POSX    = 176;   // f64 (was 152) [v018 +16]
constexpr std::size_t OFF_UNIT_KIND          = 600;   // HOBJ:String was 592 [v023 +8]
constexpr std::size_t OFF_UNIT_SKILLS        = 488;   // ArrayObj of BaseSkill was 480 [v023 +8]
constexpr std::size_t OFF_UNIT_ATTR          = 984;   // HOBJ:UnitAttributes was 976 [v023 +8]
constexpr std::size_t OFF_UNIT_LEVEL         = 992;  // i32 (Unit._level) was 984 [v023 +8]

// hl.types.ArrayObj layout: length @8 (i32), inner varray @16.
constexpr std::size_t OFF_ARRAYOBJ_LENGTH    = 8;
constexpr std::size_t OFF_ARRAYOBJ_VARRAY    = 16;
// varray header is { t@0, at@8, length@16, allocated@20 } = 24 bytes,
// element data follows inline. Element stride for object arrays is 8.
constexpr std::size_t OFF_VARRAY_DATA        = 24;

// st.skill.BaseSkill layout (classes_v0152.json). BaseSkill also extends
// ent.Serializable, so it picked up the same +8 shift as GameObject for
// every field at offset >= 144.
// st.skill.Skill (BaseSkill) - all fields +16 in v018.
constexpr std::size_t OFF_BASESKILL_KIND      = 184;   // HOBJ:String (was 160)
constexpr std::size_t OFF_BASESKILL_START     = 192;   // HF64 startTime (was 168)
constexpr std::size_t OFF_BASESKILL_STOP      = 200;   // HF64 stopTime (was 176)
constexpr std::size_t OFF_BASESKILL_DYN1      = 216;   // HF64 dynVal1 (was 192)
constexpr std::size_t OFF_BASESKILL_DYN2      = 224;   // HF64 dynVal2 (was 200)
constexpr std::size_t OFF_BASESKILL_DYN3      = 232;   // HF64 dynVal3 (was 208)
constexpr std::size_t OFF_BASESKILL_EXEC_STEP = 320;   // HOBJ:SkillStep (was 296)
constexpr std::size_t OFF_BASESKILL_RUNNING   = 360;   // HOBJ:SkillContext (was 336)

// st.skill.SkillContext [v018]: castProgress @168, hitCount @180 (both
// unchanged), startTime @224, stopTime @232, stopReason @240 (HENUM, all +8
// because v018 inserted SkillContext.currentArea@216).
//
// hitCount is the discriminator we use for "cast bar ends": when it
// flips from 0 to >0 the skill has actually impacted, which matches
// what the player sees on screen. runningCtx itself stays attached
// for the recovery animation past that point.
constexpr std::size_t OFF_SCTX_CAST_PROGRESS = 176;   // was 168 [v019 +8]
constexpr std::size_t OFF_SCTX_HIT_COUNT     = 188;   // was 180 [v019 +8]
constexpr std::size_t OFF_SCTX_START_TIME    = 232;   // was 216 (+8)
constexpr std::size_t OFF_SCTX_STOP_TIME     = 240;   // was 224 (+8)
constexpr std::size_t OFF_SCTX_STOP_REASON   = 248;   // was 232 (+8)

// Bound how many skills we scan per target per frame. Real units have
// well under 32; the cap is just a safety against a corrupted length.
constexpr int kMaxSkillScan = 32;

// UnitAttributes block - same indices hero_state uses. Slice 2 needs
// health (@240) and max_health (@248); slice 4 (damage planner) adds
// armor (@216), magic_armor (@224), magic_reduction (@232). One batch
// read covers everything up to max_health.
constexpr std::size_t OFF_ATTR_BLOCK_BASE    = 48;
constexpr std::size_t UA_BLOCK_BYTES         = 208;   // covers up to max_health@248
constexpr int UA_IDX_ARMOR           = 21;   // 48 + 21*8 = 216
constexpr int UA_IDX_MAGIC_ARMOR     = 22;   // 48 + 22*8 = 224
constexpr int UA_IDX_MAGIC_REDUCTION = 23;   // 48 + 23*8 = 232
constexpr int UA_IDX_HEALTH          = 24;   // 48 + 24*8 = 240
constexpr int UA_IDX_MAX_HEALTH      = 25;   // 48 + 25*8 = 248

// hxbit indirection: Hero.__host(+48) -> NetworkHost; NetworkHost.ctx
// (+112) -> NetworkSerializer; NetworkSerializer.refs(+8) -> the
// hl_int64_map<UID, Serializable> that resolves replicated UIDs to
// in-process object pointers.
//
// NetworkHost also exposes the game's current time (haxe.Timer.stamp
// based seconds) at offset 88. Same scale as SkillContext.startTime,
// so we can compute true elapsed = currentTime - startTime even when
// our render-thread sampling first sees the cast 1-2s after the
// server actually started it.
constexpr std::size_t OFF_NHOST_CURRENT_TIME = 88;
constexpr std::size_t OFF_NHOST_CTX          = 112;
constexpr std::size_t OFF_NSERIALIZER_REFS   = 8;

using HlHi64Get = void* (*)(void* map, std::int64_t key);
HlHi64Get g_hi64get = nullptr;

// v0.6.0 phase 2b: hxbit-safe UID resolver. The worker thread can't
// call hl_hi64get directly (races with hxbit deserialise on the same
// refs map). Instead the worker sets `g_pending_uid_request`, and a
// tail handler registered with hl_hook fires from alloc-context on
// every hl_alloc_obj - that runs on the thread that already called the
// allocator, so any hxbit bucket walk on that thread is suspended
// below us. The handler does the actual hl_hi64get and stores the
// result in (g_resolved_uid, g_resolved_ptr). The worker reads from
// the cache; one alloc-tick lag is acceptable for target tracking.
//
// Cache lifecycle: tail handler unconditionally re-resolves on every
// alloc while a request is pending. That auto-refreshes if the
// underlying Foe died or its refs entry got rewritten. When no allocs
// happen (player AFK, no combat) the cache freezes - which is fine
// because the game state isn't changing either.
std::atomic<std::uint64_t>  g_pending_uid_request{0};
std::atomic<std::uint64_t>  g_resolved_uid{0};
std::atomic<std::uintptr_t> g_resolved_ptr{0};

// Three candidate target fields on ent.Hero. In this game an action
// swing does NOT populate Unit.target - the engine writes the picked
// foe into `autoTarget` instead. `lockedTarget` is the explicit Tab
// lock, `target` is the legacy / generic Unit field. We try them in
// priority order and use the first non-null pointer.
constexpr std::size_t OFF_HERO_TARGET        = 656;    // HI64 was 648 [v023 +8]
constexpr std::size_t OFF_HERO_LOCKED_TARGET = 1240;   // HI64 was 1232 [v023 +8]
constexpr std::size_t OFF_HERO_AUTO_TARGET   = 1248;   // HI64 was 1240 [v023 +8]

// Haxe String layout: bytes ptr @ 8, length (code units) @ 16.
constexpr std::size_t OFF_STR_BYTES        = 8;
constexpr std::size_t OFF_STR_LEN          = 16;

// hl_type / hl_type_obj layout - same offsets hl_hook.cpp uses to learn
// class names from allocation type pointers.
constexpr std::size_t OFF_TYPE_OBJ_PTR = 8;    // hl_type.obj
constexpr std::size_t OFF_OBJ_NAME_PTR = 16;   // hl_type_obj.name (wchar*)

// hl_type.kind values we care about. HashLink defines them as enum
// in hl.h; we only need the two for unwrapping virtual targets.
constexpr std::uint32_t HKIND_HOBJ      = 11;
constexpr std::uint32_t HKIND_HVIRTUAL  = 15;

// vvirtual layout (hl.h): t at @0, value (vdynamic*) at @8, next at @16.
constexpr std::size_t OFF_VVIRTUAL_VALUE = 8;

// Anchor decision per type ptr: 1 = accepted, 0 = rejected. The cache
// avoids re-reading the UTF-16 class name on every frame.
std::unordered_map<std::uintptr_t, int> g_type_decision;
std::mutex                              g_type_decision_mu;

std::atomic<std::uintptr_t> g_locked_target{0};
std::atomic<std::uint64_t>  g_ticks{0};

// Diagnostic: last target pointer we logged on transition, so the
// "ACQUIRED" / "cleared" lines fire exactly once per change.
std::atomic<std::uintptr_t> g_last_logged_target{0};
// Diagnostic: same for cast start / end so we get exactly one log
// line per cast lifecycle. Tracks the SkillContext pointer.
std::atomic<std::uintptr_t> g_last_logged_cast{0};

// Cast timing state. Two-layer model:
//   - steady_clock anchor (g_cast_detected_at) - always available,
//     ticks reliably from the moment our render thread first saw
//     the cast.
//   - game-time late_offset (g_cast_late_offset_sec) - if NetworkHost.
//     currentTime AND SkillContext.startTime are both populated when
//     we detect, this is how many game-seconds the cast had already
//     been running on the server before we noticed. Added to elapsed
//     so the displayed value matches the game's own cast bar.
// Without the offset the bar would always show "0s" at first frame.
// Without the steady_clock fallback it would freeze at 0s whenever
// startTime is still 0.000 (server hasn't replicated yet).
std::chrono::steady_clock::time_point   g_cast_detected_at{};
double                                  g_cast_late_offset_sec = 0.0;
std::string                             g_cast_active_kind;
std::unordered_map<std::string, double> g_skill_duration_cache;
std::mutex                              g_cache_mu;

TargetSnapshot              g_snapshot{};
std::mutex                  g_snapshot_mu;

// v0.4.14 (F): auto-disable after repeated SEH trips. Same self-heal
// pattern hero_state uses.
constexpr int               kMaxConsecutiveFailures = 5;
std::atomic<int>            g_consecutive_failures{0};
std::atomic<bool>           g_disabled{false};

// Pull the UTF-16 class name out of a hl_type pointer. Same chase
// hl_hook does: type+8 -> hl_type_obj; obj+16 -> wchar*. Returns
// nullptr if any step would fault or yields a null pointer.
const wchar_t* read_class_name(std::uintptr_t type_ptr) {
    if (!type_ptr || !mem_is_userland(type_ptr)) return nullptr;
    std::uint64_t obj_u64 = 0;
    if (!mem_read_u64(type_ptr + OFF_TYPE_OBJ_PTR, &obj_u64)) return nullptr;
    if (!mem_is_userland(obj_u64)) return nullptr;
    std::uint64_t name_u64 = 0;
    if (!mem_read_u64(static_cast<std::uintptr_t>(obj_u64) + OFF_OBJ_NAME_PTR,
                      &name_u64)) return nullptr;
    if (!mem_is_userland(name_u64)) return nullptr;
    return reinterpret_cast<const wchar_t*>(name_u64);
}

// True if `name` starts with `prefix` (cheap wcsncmp). The wchar
// buffer is owned by libhl and lives for the process lifetime.
bool wstr_starts_with(const wchar_t* name, const wchar_t* prefix) {
    __try {
        std::size_t n = wcslen(prefix);
        return wcsncmp(name, prefix, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whitelist of acceptable target class-name prefixes. All entity
// classes in this game live under the `ent.` namespace and any
// subclass of ent.Unit inherits the kind field at offset 584. Add
// more prefixes here if we ever need to be choosier (e.g. exclude
// ent.GameObject leaves that aren't ent.Unit).
const wchar_t* const kAnchorPrefixes[] = {
    L"ent.",
};

// Dump the first 16 wchar code units of a wstring as a hex blob plus
// best-effort ASCII for diagnostics. Bounded read so we don't fault
// off the end of an unterminated buffer.
void dump_class_name(const wchar_t* name, char* out, std::size_t cap) {
    if (!name || cap == 0) { if (cap) out[0] = 0; return; }
    std::uint16_t buf[16] = {0};
    if (!mem_read_bytes(reinterpret_cast<std::uintptr_t>(name),
                        buf, sizeof(buf))) {
        snprintf(out, cap, "<read fail>");
        return;
    }
    std::size_t w = 0;
    for (int i = 0; i < 16 && w + 6 < cap; ++i) {
        std::uint16_t c = buf[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7f) {
            out[w++] = static_cast<char>(c);
        } else {
            w += snprintf(out + w, cap - w, "\\x%04x", c);
        }
    }
    if (w < cap) out[w] = 0;
}

// Decide once per type_ptr; cache the result so repeat reads are
// just a hash lookup. Logs the class name the first time we see it.
bool type_is_anchored(std::uintptr_t type_ptr) {
    if (!type_ptr) return false;
    {
        std::lock_guard<std::mutex> lk(g_type_decision_mu);
        auto it = g_type_decision.find(type_ptr);
        if (it != g_type_decision.end()) return it->second != 0;
    }
    std::uint32_t kind = 0xffffffffu;
    mem_read_u32(type_ptr, &kind);
    const wchar_t* name = read_class_name(type_ptr);
    bool ok = false;
    char dump[160] = "<null>";
    if (name) {
        for (const wchar_t* prefix : kAnchorPrefixes) {
            if (wstr_starts_with(name, prefix)) { ok = true; break; }
        }
        dump_class_name(name, dump, sizeof(dump));
    }
    logf("target_state: %s kind=%u name_ptr=0x%llx name='%s' type=0x%llx",
         ok ? "ANCHOR" : "REJECT",
         kind,
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(name)),
         dump,
         static_cast<unsigned long long>(type_ptr));
    {
        std::lock_guard<std::mutex> lk(g_type_decision_mu);
        g_type_decision.emplace(type_ptr, ok ? 1 : 0);
    }
    return ok;
}

// Read a Haxe String into ASCII. We bail on any code unit >= 0x80
// for now - `kind` ids in res.pak are all ASCII. Localized strings
// will need real UTF-16->UTF-8 conversion (Slice 2 or later).
bool read_haxe_string(std::uintptr_t str_ptr, std::string& out) {
    out.clear();
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    auto bytes_ptr = static_cast<std::uintptr_t>(bytes_u64);
    if (!mem_is_userland(bytes_ptr)) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 128) return false;

    std::uint8_t buf[256];
    std::size_t  nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(bytes_ptr, buf, nb)) return false;

    out.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 0x80) return false;
        out.push_back(static_cast<char>(c));
    }
    return true;
}

// Walk Unit.skills, return the BaseSkill pointer whose runningCtx is
// non-null. Also returns the running SkillContext pointer via out
// parameter. Both 0 if no active cast.
//
// First time we see a given target, log a one-shot dump of the
// skills array: its size, its first few element pointers, and the
// kind/runningCtx of each. That's how we learn whether Foes really
// store BaseSkill objects in this field with runningCtx at @328.
bool find_active_cast(std::uintptr_t unit,
                      std::uintptr_t* out_skill,
                      std::uintptr_t* out_ctx) {
    *out_skill = 0;
    *out_ctx   = 0;
    std::uint64_t arr_obj_u64 = 0;
    bool arr_obj_read = mem_read_u64(unit + OFF_UNIT_SKILLS, &arr_obj_u64);
    auto arr_obj = static_cast<std::uintptr_t>(arr_obj_u64);
    bool arr_obj_ok = arr_obj_read && arr_obj && mem_is_userland(arr_obj);

    std::int32_t length = 0;
    std::uint64_t varray_u64 = 0;
    std::uintptr_t varray = 0;
    if (arr_obj_ok) {
        mem_read_i32(arr_obj + OFF_ARRAYOBJ_LENGTH, &length);
        if (mem_read_u64(arr_obj + OFF_ARRAYOBJ_VARRAY, &varray_u64)) {
            varray = static_cast<std::uintptr_t>(varray_u64);
        }
    }

    if (!arr_obj_ok || length <= 0 || !varray || !mem_is_userland(varray))
        return false;
    if (length > kMaxSkillScan) length = kMaxSkillScan;

    // For foes, castProgress stays 0 (server-authoritative - the
    // client never simulates the cast curve) and stopTime stays -1
    // while running. So we can't pick "the meaningful cast" from
    // those fields. Empirical disambiguator: skip skills whose kind
    // contains "_Auto" (Boar_Auto_Elite, Skunk_Auto, etc.); those are
    // the auto-attacks whose runningCtx flips on every swing.
    struct Active {
        std::uintptr_t skill;
        std::uintptr_t ctx;
        double         start_t;
        std::string    kind;
    };
    Active actives[kMaxSkillScan];
    int n_active = 0;

    for (int i = 0; i < length; ++i) {
        std::uint64_t skill_u64 = 0;
        if (!mem_read_u64(varray + OFF_VARRAY_DATA + i * 8, &skill_u64)) break;
        auto skill = static_cast<std::uintptr_t>(skill_u64);
        if (!skill || !mem_is_userland(skill)) continue;
        std::uint64_t ctx_u64 = 0;
        if (!mem_read_u64(skill + OFF_BASESKILL_RUNNING, &ctx_u64)) continue;
        auto ctx = static_cast<std::uintptr_t>(ctx_u64);
        if (!ctx || !mem_is_userland(ctx)) continue;

        // Two end signals: stopReason != null (skill fully torn down),
        // OR hitCount > 0 (skill impacted; recovery animation may
        // still hold runningCtx but the cast bar should be gone).
        std::uint64_t stop_reason_u64 = 0;
        std::int32_t  hit_count       = 0;
        mem_read_u64(ctx + OFF_SCTX_STOP_REASON, &stop_reason_u64);
        mem_read_i32(ctx + OFF_SCTX_HIT_COUNT,   &hit_count);
        if (stop_reason_u64 != 0 || hit_count > 0) continue;

        std::string kind;
        std::uint64_t kind_u64 = 0;
        if (mem_read_u64(skill + OFF_BASESKILL_KIND, &kind_u64)) {
            read_haxe_string(static_cast<std::uintptr_t>(kind_u64), kind);
        }
        double start_t = 0.0;
        mem_read_f64(ctx + OFF_SCTX_START_TIME, &start_t);

        // Skip auto-attacks. kind.find returns npos if not found.
        if (kind.find("_Auto") != std::string::npos) continue;

        actives[n_active++] = { skill, ctx, start_t, std::move(kind) };
    }
    if (n_active == 0) return false;

    // If more than one non-auto skill is active (overlapping casts on
    // multi-phase bosses, for instance), prefer the most recently
    // started one.
    int best = 0;
    for (int i = 1; i < n_active; ++i) {
        if (actives[i].start_t > actives[best].start_t) best = i;
    }
    *out_skill = actives[best].skill;
    *out_ctx   = actives[best].ctx;
    return true;
}

void publish_snapshot(const TargetSnapshot& s) {
    std::lock_guard<std::mutex> lk(g_snapshot_mu);
    g_snapshot = s;
}

// Read NetworkHost.currentTime - the game's haxe.Timer.stamp() value
// updated each frame. Same time base as SkillContext.startTime.
// Returns 0.0 if the chain is broken.
double read_game_now(std::uintptr_t hero) {
    std::uint64_t host = 0;
    if (!mem_read_u64(hero + OFF_UNIT_HOST, &host)) return 0.0;
    if (!mem_is_userland(host)) return 0.0;
    double t = 0.0;
    if (!mem_read_f64(static_cast<std::uintptr_t>(host) +
                      OFF_NHOST_CURRENT_TIME, &t)) return 0.0;
    return t;
}

// Walk Hero -> __host -> ctx -> refs to get the hxbit UID resolver
// map. Returns 0 if any pointer in the chain is null / non-userland.
std::uintptr_t resolve_refs_map(std::uintptr_t hero) {
    std::uint64_t host = 0;
    if (!mem_read_u64(hero + OFF_UNIT_HOST, &host)) return 0;
    if (!mem_is_userland(host)) return 0;
    std::uint64_t ctx = 0;
    if (!mem_read_u64(static_cast<std::uintptr_t>(host) + OFF_NHOST_CTX,
                      &ctx)) return 0;
    if (!mem_is_userland(ctx)) return 0;
    std::uint64_t refs = 0;
    if (!mem_read_u64(static_cast<std::uintptr_t>(ctx) + OFF_NSERIALIZER_REFS,
                      &refs)) return 0;
    if (!mem_is_userland(refs)) return 0;
    return static_cast<std::uintptr_t>(refs);
}

// Internal helper: HVIRTUAL unwrap shared by the direct (non-worker)
// path and the tail-handler path. Returns 0 if the pointer is not
// userland or its type tag is unreadable.
std::uintptr_t hvirtual_unwrap(std::uintptr_t p) {
    if (!p || !mem_is_userland(p)) return 0;
    std::uint64_t type_u64 = 0;
    if (!mem_read_u64(p, &type_u64)) return 0;
    std::uint32_t kind = 0;
    if (!mem_read_u32(static_cast<std::uintptr_t>(type_u64), &kind)) return 0;
    if (kind == HKIND_HVIRTUAL) {
        std::uint64_t inner_u64 = 0;
        if (!mem_read_u64(p + OFF_VVIRTUAL_VALUE, &inner_u64)) return 0;
        auto inner = static_cast<std::uintptr_t>(inner_u64);
        if (!inner || !mem_is_userland(inner)) return 0;
        return inner;
    }
    return p;
}

// Look up a UID in the refs map. Two paths:
//
//   1. Non-worker thread (Present-driven tick path, or any non-pump
//      caller): call hl_hi64get directly. Safe because the Present
//      hook runs on the game's render thread, which doesn't race with
//      hxbit deserialise reentrantly on the refs map in practice.
//
//   2. Worker thread (hl_pump_worker): NEVER call hl_hi64get here.
//      That races with hxbit's bucket walk and crashes (the v0.5.x
//      mid-session AV we've been chasing). Instead:
//        - Store `uid` in g_pending_uid_request
//        - Read the cached resolved pointer from (g_resolved_uid,
//          g_resolved_ptr); if the cached uid matches, return it
//        - Otherwise return 0 (target won't be tracked this tick)
//      The actual hl_hi64get fires from `target_state_tail_handler`
//      in alloc-context on the next hl_alloc_obj, which is hxbit-safe
//      because the alloc nests under the same thread's hxbit call.
std::uintptr_t resolve_uid(std::uintptr_t refs_map, std::uint32_t uid) {
    if (!uid) return 0;
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) {
        // v0.6.0 phase 2d: worker reads from our own UID registry,
        // built up from alloc-hook context (ent.Foe and ent.Hero
        // watchers). No game-state read, no hxbit refs map walk.
        // Registry returns 0 if (a) the UID hasn't been alloc'd yet,
        // (b) the entry was evicted (slot reuse), or (c) we don't
        // watch the class of this object.
        return uid_registry_lookup(static_cast<std::uint64_t>(uid));
    }

    if (!g_hi64get || !refs_map) return 0;
    void* obj = g_hi64get(reinterpret_cast<void*>(refs_map),
                          static_cast<std::int64_t>(uid));
    return hvirtual_unwrap(reinterpret_cast<std::uintptr_t>(obj));
}

// Tail handler: runs from alloc-context on every hl_alloc_obj. Drains
// the worker's pending UID request and stores the resolved pointer in
// the atomic cache. hxbit-safe because we're synchronously nested
// inside an hl_alloc_obj call on the same thread that called the
// allocator - hxbit's bucket walk on this thread (if it's the caller)
// is suspended below us.
void target_state_tail_handler() {
    std::uint64_t uid = g_pending_uid_request.load(std::memory_order_acquire);
    if (uid == 0) return;
    if (!g_hi64get) return;

    std::uintptr_t hero = hero_state_locked_ptr();
    if (!hero) return;
    std::uintptr_t refs = resolve_refs_map(hero);
    if (!refs) return;

    void* obj = g_hi64get(reinterpret_cast<void*>(refs),
                          static_cast<std::int64_t>(uid));
    std::uintptr_t resolved = hvirtual_unwrap(reinterpret_cast<std::uintptr_t>(obj));

    // Publish even a 0 result - caller relies on UID match to decide
    // cache validity, so storing the UID with ptr=0 is a meaningful
    // "I tried and found nothing" answer.
    g_resolved_ptr.store(resolved, std::memory_order_release);
    g_resolved_uid.store(uid, std::memory_order_release);
}

// A candidate field value is a real object pointer only if it points
// at an anchored ent.* hl_type. Magnitude alone can't tell a pointer
// from a hxbit UID: #48 dropped the userland floor to 64 KB so UIDs
// (small ints, ~millions) now clear mem_is_userland too. Validate
// structurally instead. mem_read_u64 is SEH-wrapped and returns false
// on a fault without tripping the tick's auto-disable counter, so
// probing a UID-as-address here is safe.
bool ptr_is_anchored(std::uintptr_t p) {
    if (!mem_is_userland(p)) return false;
    std::uint64_t type_u64 = 0;
    if (!mem_read_u64(p, &type_u64)) return false;
    return type_is_anchored(static_cast<std::uintptr_t>(type_u64));
}

// Try the three candidate fields in priority order. Each one stores
// either a raw pointer or a hxbit UID (small integer). We can't tell
// the two apart by magnitude anymore (see ptr_is_anchored), so we try
// each value as a pointer first (accept iff it lands on an anchored
// ent.* type) and fall back to resolving it as a UID through the hxbit
// refs map, validating the resolved pointer the same way.
std::uintptr_t pick_target(std::uintptr_t hero,
                           std::uintptr_t refs_map,
                           const char** out_label) {
    struct Candidate {
        const char*    label;
        std::size_t    offset;
    };
    static constexpr Candidate kCandidates[] = {
        { "locked", OFF_HERO_LOCKED_TARGET },
        { "auto",   OFF_HERO_AUTO_TARGET   },
        { "target", OFF_HERO_TARGET        },
    };
    for (const auto& c : kCandidates) {
        std::uint64_t v = 0;
        if (!mem_read_u64(hero + c.offset, &v)) continue;
        if (v == 0) continue;
        // Raw object pointer?
        if (ptr_is_anchored(static_cast<std::uintptr_t>(v))) {
            *out_label = c.label;
            return static_cast<std::uintptr_t>(v);
        }
        // Otherwise treat it as a hxbit UID and resolve it.
        auto resolved = resolve_uid(refs_map, static_cast<std::uint32_t>(v));
        if (resolved && ptr_is_anchored(resolved)) {
            *out_label = c.label;
            return resolved;
        }
    }
    *out_label = "";
    return 0;
}

void publish() {
    TargetSnapshot s{};
    std::uintptr_t hero = hero_state_locked_ptr();
    if (!hero) {
        g_locked_target.store(0, std::memory_order_release);
        publish_snapshot(s);
        return;
    }

    std::uintptr_t refs = resolve_refs_map(hero);
    const char* picked = "";
    std::uintptr_t tgt = pick_target(hero, refs, &picked);
    if (!tgt) {
        // #69: boss-only-dungeon bosses expose autoTarget as an hxbit UID our
        // alloc registry can't resolve (it watches ent.Foe/ent.Hero, not
        // ent.boss.*), so pick_target comes up empty for the whole fight. The
        // damage path sees the boss pointer directly on every outgoing hit, so
        // fall back to that cache while it is fresh and the pointer is still a
        // live anchored ent.* object. The cached display name is already
        // resolved (e.g. "Lady Bee"), so seed it here; the field reads below
        // fill in position, health, level and cast from the boss itself.
        std::uintptr_t boss_ptr = 0;
        std::string    boss_name;
        if (boss_target_recent(&boss_ptr, &boss_name) &&
            ptr_is_anchored(boss_ptr)) {
            tgt    = boss_ptr;
            picked = "boss";
            s.kind = boss_name;
        }
    }
    if (!tgt) {
        if (g_last_logged_target.exchange(0) != 0) {
            logf("target_state: target cleared");
            plugins_emit_target_changed("");
        }
        g_locked_target.store(0, std::memory_order_release);
        publish_snapshot(s);
        return;
    }

    // Read type tag, then accept iff the class name starts with one
    // of our anchor prefixes (currently "ent."). Decision is cached
    // per type_ptr inside type_is_anchored.
    std::uint64_t type_u64 = 0;
    bool type_read_ok = mem_read_u64(tgt, &type_u64);
    bool anchored = type_read_ok &&
                    type_is_anchored(static_cast<std::uintptr_t>(type_u64));

    if (!anchored) {
        g_locked_target.store(0, std::memory_order_release);
        publish_snapshot(s);
        return;
    }

    // Type-anchored - safe to read the kind String.
    std::uint64_t kind_u64 = 0;
    if (mem_read_u64(tgt + OFF_UNIT_KIND, &kind_u64)) {
        read_haxe_string(static_cast<std::uintptr_t>(kind_u64), s.kind);
    }

    // #69: boss-only-dungeon bosses leave Unit.kind null/empty, so the name
    // never reached farever.target even though the boss timer names them
    // fine. Fall back to the HashLink class name - the exact source the boss
    // timer reads - and strip the prefix: "ent.boss.Crabgantua" ->
    // "Crabgantua". Ordinary Foes already filled kind above, so this only
    // fires when the kind String was missing.
    if (s.kind.empty()) {
        char cls[64] = {0};
        if (hl_hook_class_name(tgt, cls, sizeof(cls))) {
            s.kind = boss_display_name(cls);
        }
    }

    // Position (inherited from ent.GameObject, same layout as Hero).
    double pos[4]{0,0,0,0};
    if (mem_read_bytes(tgt + OFF_GAMEOBJECT_POSX, pos, sizeof(pos))) {
        s.x = pos[0]; s.y = pos[1]; s.z = pos[2];
    }

    // Level (i32 on ent.Unit).
    std::int32_t lvl = 0;
    if (mem_read_i32(tgt + OFF_UNIT_LEVEL, &lvl)) s.level = static_cast<int>(lvl);

    // Health / max-health via Unit.attr. Same pattern as hero_state's
    // attr chase but only the two fields we need for Slice 2.
    std::uint64_t attr_u64 = 0;
    if (mem_read_u64(tgt + OFF_UNIT_ATTR, &attr_u64)) {
        auto attr = static_cast<std::uintptr_t>(attr_u64);
        if (mem_is_userland(attr)) {
            double ua[26]{};   // up to and including max_health
            if (mem_read_bytes(attr + OFF_ATTR_BLOCK_BASE, ua, UA_BLOCK_BYTES)) {
                s.attr_ok         = true;
                s.health          = ua[UA_IDX_HEALTH];
                s.max_health      = ua[UA_IDX_MAX_HEALTH];
                s.armor           = ua[UA_IDX_ARMOR];
                s.magic_armor     = ua[UA_IDX_MAGIC_ARMOR];
                s.magic_reduction = ua[UA_IDX_MAGIC_REDUCTION];
            }
        }
    }

    // Slice 3: walk skills, filter out auto-attacks, take the most
    // recently started non-auto runningCtx as the active cast.
    // Elapsed is computed from game time (NetworkHost.currentTime -
    // SkillContext.startTime) so it matches what the player sees on
    // screen even if we noticed the cast a couple of frames late.
    std::uintptr_t skill_ptr = 0, ctx_ptr = 0;
    if (find_active_cast(tgt, &skill_ptr, &ctx_ptr)) {
        s.is_casting = true;
        std::uint64_t skind_u64 = 0;
        if (mem_read_u64(skill_ptr + OFF_BASESKILL_KIND, &skind_u64)) {
            read_haxe_string(static_cast<std::uintptr_t>(skind_u64),
                             s.cast_skill);
        }

        std::uintptr_t prev_cast = g_last_logged_cast.load(
            std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        double sctx_start = 0.0;
        mem_read_f64(ctx_ptr + OFF_SCTX_START_TIME, &sctx_start);
        double game_now = read_game_now(hero);

        if (prev_cast != ctx_ptr) {
            g_cast_detected_at = now;
            // Late-offset: how many game-seconds had passed before
            // we noticed. Only meaningful if both timers are valid.
            g_cast_late_offset_sec = 0.0;
            if (game_now > 0.0 && sctx_start > 0.0 &&
                game_now >= sctx_start) {
                double late = game_now - sctx_start;
                // Sanity: if the offset looks unreasonable
                // (>10s) the game clock probably hasn't ticked yet
                // - fall back to no offset.
                if (late >= 0.0 && late < 10.0) {
                    g_cast_late_offset_sec = late;
                }
            }
            g_cast_active_kind = s.cast_skill;
            double cached = 0.0;
            {
                std::lock_guard<std::mutex> lk(g_cache_mu);
                auto it = g_skill_duration_cache.find(s.cast_skill);
                if (it != g_skill_duration_cache.end()) cached = it->second;
            }
            logf("target_state: cast STARTED skill='%s' start=%.3f "
                 "ctx=0x%llx now=%.3f late_by=%.3fs cached_total=%.2fs",
                 s.cast_skill.c_str(),
                 sctx_start,
                 static_cast<unsigned long long>(ctx_ptr),
                 game_now, g_cast_late_offset_sec, cached);
            g_last_logged_cast.store(ctx_ptr, std::memory_order_relaxed);
            plugins_emit_cast_start(s.cast_skill.c_str(), cached);
        }

        // Steady-clock-based elapsed plus the late offset we captured
        // at detection. This works even when sctx_start is still 0.
        double elapsed = std::chrono::duration<double>(
                             now - g_cast_detected_at).count() +
                         g_cast_late_offset_sec;
        s.cast_elapsed_sec = elapsed;

        double cached_total = 0.0;
        {
            std::lock_guard<std::mutex> lk(g_cache_mu);
            auto it = g_skill_duration_cache.find(s.cast_skill);
            if (it != g_skill_duration_cache.end()) cached_total = it->second;
        }
        if (cached_total > 0.0) {
            s.cast_total_sec     = cached_total;
            s.cast_remaining_sec =
                (cached_total > elapsed) ? (cached_total - elapsed) : 0.0;
            s.cast_progress      = (elapsed / cached_total);
            if (s.cast_progress > 1.0) s.cast_progress = 1.0;
        }
    } else {
        std::uintptr_t prev_cast = g_last_logged_cast.exchange(0);
        if (prev_cast != 0) {
            double duration = std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() -
                                  g_cast_detected_at).count() +
                              g_cast_late_offset_sec;
            if (duration > 0.15 && duration < 30.0 &&
                !g_cast_active_kind.empty()) {
                std::lock_guard<std::mutex> lk(g_cache_mu);
                g_skill_duration_cache[g_cast_active_kind] = duration;
                logf("target_state: cast ENDED skill='%s' duration=%.2fs "
                     "(cached)",
                     g_cast_active_kind.c_str(), duration);
            } else {
                logf("target_state: cast ENDED skill='%s' duration=%.2fs "
                     "(not cached)",
                     g_cast_active_kind.c_str(), duration);
            }
            plugins_emit_cast_end(g_cast_active_kind.c_str(), duration);
            g_cast_active_kind.clear();
            g_cast_late_offset_sec = 0.0;
        }
    }
    s.exists = true;

    std::uintptr_t prev = g_last_logged_target.load(std::memory_order_relaxed);
    if (prev != tgt) {
        logf("target_state: target ACQUIRED field=%s ptr=0x%llx kind='%s'",
             picked,
             static_cast<unsigned long long>(tgt),
             s.kind.c_str());
        g_last_logged_target.store(tgt, std::memory_order_relaxed);
        plugins_emit_target_changed(s.kind.c_str());
    }
    g_locked_target.store(tgt, std::memory_order_release);
    publish_snapshot(s);
}

// Read raw values of all three candidate fields, for diagnostic
// logging. Returns true if at least one field could be read at all
// (regardless of value).
bool read_raw_target_fields(std::uintptr_t hero,
                            std::uint64_t* tgt,
                            std::uint64_t* lck,
                            std::uint64_t* aut) {
    *tgt = *lck = *aut = 0;
    bool any = false;
    if (mem_read_u64(hero + OFF_HERO_TARGET,        tgt)) any = true;
    if (mem_read_u64(hero + OFF_HERO_LOCKED_TARGET, lck)) any = true;
    if (mem_read_u64(hero + OFF_HERO_AUTO_TARGET,   aut)) any = true;
    return any;
}

void target_state_tick_body(std::uint64_t n) {
    // Throttle to every 4th frame (~15 Hz). Target identity doesn't
    // change often enough to justify a 60 Hz read, and Slice 1 has
    // nothing time-sensitive in the snapshot yet.
    if ((n & 0x3) != 0) return;

    // Heartbeat every ~5 s while a Hero is locked: dump the raw values
    // of all three candidate fields so we can diagnose the case where
    // pick_target keeps returning 0. n increments per call, throttle
    // is /4 → 75 publishes per 5 s at 60 fps, but the body returns
    // early on most. The heartbeat hits at n divisible by 300 → every
    // ~5 s.
    if ((n % 300) == 0) {
        std::uintptr_t hero = hero_state_locked_ptr();
        if (hero) {
            std::uint64_t t = 0, l = 0, a = 0;
            read_raw_target_fields(hero, &t, &l, &a);
            std::uintptr_t refs = resolve_refs_map(hero);
            logf("target_state HB tick=%llu hero=0x%llx refs=0x%llx "
                 "target=0x%llx locked=0x%llx auto=0x%llx",
                 static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(hero),
                 static_cast<unsigned long long>(refs),
                 static_cast<unsigned long long>(t),
                 static_cast<unsigned long long>(l),
                 static_cast<unsigned long long>(a));
        }
    }
    publish();
}

}  // namespace

void target_state_init(const LibHL& libhl) {
    g_hi64get = reinterpret_cast<HlHi64Get>(libhl.hl_hi64get);
    if (!g_hi64get) {
        logf("target_state: hl_hi64get not resolved - UID-based "
             "targets will be ignored, only direct pointers work");
    } else {
        logf("target_state: hl_hi64get=%p - UID resolution armed",
             reinterpret_cast<void*>(g_hi64get));
    }
    // v0.6.0 phase 2b diag: tail handler temporarily disabled. The
    // run-from-every-alloc design races against hxbit when hl_alloc_obj
    // is called from a thread that isn't the hxbit-deserialize thread -
    // observed as a game-side thread freeze (worker + overlay kept
    // running, game state stopped updating) after ~2.5 min of worker
    // mode. Re-enable once we have a safer trigger (e.g., only fire
    // from specific class watchers known to be hxbit-deserialized, or
    // only fire from Present-detour context). See log 2026-05-23 morn.
    //
    // hl_hook_register_tail(target_state_tail_handler);
    (void)&target_state_tail_handler;  // silence unused-function warning
}

// SEH-wrapped tick entry. Body lives in target_state_tick_body so this
// function only holds POD locals - __try/__except cannot coexist with
// C++ destructor unwinding (C2712). On the disable threshold we set
// the disabled flag and clear the locked pointer here, then perform
// the snapshot reset in target_state_auto_disable() (where it can
// safely take the mutex).
void target_state_auto_disable() {
    {
        std::lock_guard<std::mutex> lk(g_snapshot_mu);
        g_snapshot = TargetSnapshot{};
    }
    logf("target_state: %d consecutive SEH trips - auto-disabling "
         "target tracker to keep the game alive",
         kMaxConsecutiveFailures);
}

void target_state_tick() {
    if (g_disabled.load(std::memory_order_acquire)) return;
    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    bool trip = false;
    __try {
        target_state_tick_body(n);
        g_consecutive_failures.store(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int fails = g_consecutive_failures.fetch_add(1) + 1;
        logf("target_state: SEH trip #%d in tick %llu (code 0x%08lx)",
             fails, static_cast<unsigned long long>(n),
             GetExceptionCode());
        if (fails >= kMaxConsecutiveFailures) {
            g_disabled.store(true);
            g_locked_target.store(0);
            trip = true;
        }
    }
    if (trip) target_state_auto_disable();
}

TargetSnapshot target_state_read() {
    std::lock_guard<std::mutex> lk(g_snapshot_mu);
    return g_snapshot;
}

std::uintptr_t target_state_locked_ptr() {
    return g_locked_target.load(std::memory_order_acquire);
}

}  // namespace farever
