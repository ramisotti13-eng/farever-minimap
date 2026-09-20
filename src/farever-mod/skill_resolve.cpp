// Pulls a CDB record's gfx (atlas + cell coords) out of the live HashLink
// heap by calling hl_dyn_get* on the virtual struct chain. Every CDB-backed
// class stores its record in an `inf` field; only the offset differs:
//
//   st.skill.BaseSkill  @ OFF_BS_INF     -> inf : HVIRTUAL (the CDB row)
//   st.Item             @ OFF_ITEM_INF        .gfx : HVIRTUAL ( file: String,
//   st.Activity         @ OFF_ACTIVITY_INF                      x/y/size: I32,
//                          (see activity_icons.cpp)             width/height: ? )
//
// We cache per-kind so the dyn calls only run once per skill / item.

#include "skill_resolve.h"
#include "hl_hook.h"
#include "hl_pump.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace farever {
namespace {

constexpr std::size_t OFF_BS_INF      = 176;  // BaseSkill.inf (was 152) [v018 +16]
constexpr std::size_t OFF_STR_BYTES   = 8;
constexpr std::size_t OFF_STR_LEN     = 16;

// #105: st.Item carries the same CDB record shape - inf.gfx {file,x,y,size}.
// Equipped items can grant a status with no artwork of its own (trinkets:
// "PurifiedHeart" grants "PurifiedHeart_Status"), and the icon the game shows
// for that buff is the item's. Resolving those through the same walk means one
// piece of machinery, one cache and one drain path.
constexpr std::size_t OFF_ITEM_INF    = 128;  // st.Item.inf, was 120 [v024 +8]

// HashLink-side function pointer types. Mirroring the exports we
// resolved in libhl.cpp.
using HlDynGetP    = void*  (*)(void* d, int hash, void* result_type);
using HlDynGetI    = int    (*)(void* d, int hash, void* result_type);
using HlDynGetD    = double (*)(void* d, int hash);
using HlHashUtf8   = int    (*)(const char* utf8);

LibHL          g_libhl{};
HlDynGetP      g_dyn_getp = nullptr;
HlDynGetI      g_dyn_geti = nullptr;
HlDynGetD      g_dyn_getd = nullptr;
HlHashUtf8     g_hash     = nullptr;
void*          g_hlt_dyn  = nullptr;
void*          g_hlt_i32  = nullptr;
void*          g_hlt_bytes = nullptr;
bool           g_ready    = false;

int            g_hash_gfx      = 0;
int            g_hash_file     = 0;
int            g_hash_x        = 0;
int            g_hash_y        = 0;
int            g_hash_size     = 0;
int            g_hash_width    = 0;
int            g_hash_height   = 0;
int            g_hash_cooldown = 0;   // #96: inf.cooldown (CDB base cooldown)
int            g_hash_inherit  = 0;   // #107: inf.inherit (activity subkind)
int            g_hash_id       = 0;   // #107: inf.id

std::mutex                                g_cache_mu;
std::unordered_map<std::string, SkillGfx> g_cache;

// #96: per-kind effective/base cooldown, captured on the safe resolve thread.
// OFF_SKILL_* are ent Skill (st.skill.Skill) runtime offsets [v019+].
constexpr std::size_t OFF_SKILL_KIND     = 184;  // BaseSkill.kind (String)
constexpr std::size_t OFF_SKILL_CHARGES  = 416;  // Skill.charges (f64)
constexpr std::size_t OFF_SKILL_TRIG_CD  = 448;  // Skill.triggeredCD (bool)
constexpr std::size_t OFF_SKILL_COOLDOWN = 456;  // Skill.cooldownDuration (f64, effective)

std::mutex                                    g_cd_mu;
std::unordered_map<std::string, SkillCooldown> g_cd_cache;   // by kind
std::unordered_map<int, int>                  g_cd_hist;     // (reduction*1000) bucket -> count

// v0.6.0 phase 2c: deferred-resolve queue. Worker sets entries when its
// query bails; the alloc-context pump drains them.
//
// #104: this used to be a SINGLE slot with overwrite-latest semantics, on the
// assumption that a dropped request would simply be re-queued. It is not:
// there are two independent producers (the damage path, once per unresolved
// kind seen in a combat event, and the hero-state readers, which run every
// publish tick). The hero-state producer fires far more often than the pump
// drains, so a class skill queued from a damage event was almost always
// overwritten before the next ui.comp.DamageDisplay alloc came round, and
// farever.icons.skill() stayed nil forever for those kinds. A small dedup'd
// queue plus a multi-entry drain removes the loss without touching the
// threading model (the drain still only runs in alloc context).
constexpr std::size_t   kPendingCap      = 32;  // queued kinds, oldest dropped
constexpr int           kDrainPerPump    = 4;   // resolves per pump
constexpr std::uint64_t kDrainCooldownMs = 200; // min gap between entity-alloc drains

struct PendingResolve {
    std::uintptr_t base_skill_ptr;
    std::size_t    inf_offset;      // OFF_BS_INF for skills, OFF_ITEM_INF for items
    char           skill_name[64];
};
std::mutex                 g_pending_mu;
std::deque<PendingResolve> g_pending;

// --- dyn dispatch safety -------------------------------------------------
//
// The pointers we walk come out of the pending queue and can be seconds old
// by the time an alloc context drains them. HashLink's collector does not
// move objects, so a dead pointer still reads without faulting - it just
// returns garbage, and a garbage `inf` handed to hl_dyn_getp faults inside
// HashLink, on the game's thread. mem_is_userland does not catch that: it
// only rejects kernel addresses, and any stale heap word passes.
//
// Two guards, both cheap:
//   1. is_dyn_target() checks the value is structurally a HashLink object we
//      can dispatch on - its first word is an hl_type* whose kind is one of
//      the three that carry named fields. Anchoring on the type is the same
//      rule the rest of the mod uses, and it rejects arbitrary heap garbage
//      rather than trusting a magnitude test.
//   2. the dyn calls themselves run under SEH, so even a pointer that
//      passes (1) and is stale cannot take the game down.
//
// The check deliberately accepts more than HVIRTUAL. A CDB record reaches us
// as a vvirtual today, but hl_dyn_get* is equally happy with a dynobj or a
// plain object, and being wrong about which one the runtime hands over would
// silently kill icon resolution again (#104). Three accepted kinds still
// leave the guard doing its job: garbage would have to point at something
// whose first word is a pointer to a word holding 11, 15 or 16.
//
// SEH needs functions without unwindable objects, hence the free helpers.
constexpr std::uint32_t HL_KIND_HOBJ     = 11;
constexpr std::uint32_t HL_KIND_HVIRTUAL = 15;
constexpr std::uint32_t HL_KIND_HDYNOBJ  = 16;
constexpr std::size_t   OFF_HL_TYPE_KIND = 0;   // hl_type.kind : u32 @0
constexpr std::size_t   OFF_VALUE_TYPE   = 0;   // vvirtual/vobj .t : hl_type* @0

bool is_dyn_target(const void* p) {
    std::uintptr_t v = reinterpret_cast<std::uintptr_t>(p);
    if (!v || !mem_is_userland(v)) return false;
    std::uint64_t type_ptr = 0;
    if (!mem_read_u64(v + OFF_VALUE_TYPE, &type_ptr)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(type_ptr))) return false;
    std::int32_t kind = 0;
    if (!mem_read_i32(static_cast<std::uintptr_t>(type_ptr) + OFF_HL_TYPE_KIND,
                      &kind))
        return false;
    auto k = static_cast<std::uint32_t>(kind);
    return k == HL_KIND_HVIRTUAL || k == HL_KIND_HDYNOBJ || k == HL_KIND_HOBJ;
}

bool seh_dyn_getp(void* obj, int hash, void** out) {
    __try {
        *out = g_dyn_getp(obj, hash, g_hlt_dyn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_dyn_geti(void* obj, int hash, int* out) {
    __try {
        *out = g_dyn_geti(obj, hash, g_hlt_i32);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool seh_dyn_getd(void* obj, int hash, double* out) {
    __try {
        *out = g_dyn_getd(obj, hash);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read a Haxe String (HOBJ:String at `str_ptr` - { bytes: vbytes*, len: i32 })
// into an ASCII output buffer. The string is UTF-16; we drop any code
// unit above 0x7F (file paths in res.pak are ASCII).
bool read_haxe_string(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    if (!str_ptr) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 256) return false;

    std::uint8_t buf[512];
    std::size_t  nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64), buf, nb))
        return false;

    std::size_t w = 0;
    for (int i = 0; i < length && w + 1 < cap; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 0x80) return false;
        out[w++] = static_cast<char>(c);
    }
    out[w] = 0;
    return true;
}

// Reduce a full path like "UI/icons/atlas_class_Mage_96PX.png" to the
// basename - that's what we use as the atlas key.
void basename_into(const char* full, char* dst, std::size_t cap) {
    const char* slash = std::strrchr(full, '/');
    const char* base  = slash ? slash + 1 : full;
    std::size_t n = std::strlen(base);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, base, n);
    dst[n] = 0;
}

}  // namespace

// #104 drain trigger, defined below next to the pump it calls.
static void on_entity_alloc(std::uintptr_t obj);

void skill_resolve_init(const LibHL& libhl) {
    g_libhl     = libhl;
    g_dyn_getp  = reinterpret_cast<HlDynGetP >(libhl.hl_dyn_getp);
    g_dyn_geti  = reinterpret_cast<HlDynGetI >(libhl.hl_dyn_geti);
    g_dyn_getd  = reinterpret_cast<HlDynGetD >(libhl.hl_dyn_getd);
    g_hash      = reinterpret_cast<HlHashUtf8>(libhl.hl_hash_utf8);
    g_hlt_dyn   = libhl.hlt_dyn;
    g_hlt_i32   = libhl.hlt_i32;
    g_hlt_bytes = libhl.hlt_bytes;

    g_ready = g_dyn_getp && g_dyn_geti && g_hash &&
              g_hlt_dyn && g_hlt_i32 && g_hlt_bytes;
    if (!g_ready) {
        logf("skill_resolve: missing exports - won't auto-map icons");
        return;
    }
    g_hash_gfx      = g_hash("gfx");
    g_hash_file     = g_hash("file");
    g_hash_x        = g_hash("x");
    g_hash_y        = g_hash("y");
    g_hash_size     = g_hash("size");
    g_hash_width    = g_hash("width");
    g_hash_height   = g_hash("height");
    g_hash_cooldown = g_hash("cooldown");
    g_hash_inherit  = g_hash("inherit");
    g_hash_id       = g_hash("id");
    logf("skill_resolve: ready (gfx=%d file=%d x=%d y=%d size=%d "
         "width=%d height=%d)",
         g_hash_gfx, g_hash_file, g_hash_x, g_hash_y, g_hash_size,
         g_hash_width, g_hash_height);

    // #104: give the deferred queue a drain trigger that does not require
    // combat. These classes are already watched by other modules; an extra
    // callback per class is one more entry in the dispatcher's list.
    hl_hook_register(L"ent.Foe",  on_entity_alloc);
    hl_hook_register(L"ent.Hero", on_entity_alloc);
    logf("skill_resolve: icon-drain watchers registered (ent.Foe, ent.Hero) "
         "- resolves no longer need combat (#104)");
}

// First N calls log every step so we can diagnose why resolution
// fails. After that the call is silent (steady state).
std::atomic<int> g_trace_left{8};

namespace {

// #96: fold a resolved skill's cooldown pair into the cache + global-CDR
// histogram. Runs on the same safe thread as skill_resolve_query_impl (dyn is
// OK here). base = inf.cooldown (CDB config), eff = Skill.cooldownDuration@456
// (runtime, all reductions applied). Each kind contributes to the histogram
// once; the bucket most kinds share is the global (gear) cooldown reduction.
void skill_cooldown_capture(std::uintptr_t bs, void* inf) {
    if (!g_dyn_getd || !mem_is_userland(bs)) return;
    std::uint64_t kind_u64 = 0;
    if (!mem_read_u64(bs + OFF_SKILL_KIND, &kind_u64)) return;
    char kbuf[96] = {};
    if (!read_haxe_string(static_cast<std::uintptr_t>(kind_u64), kbuf,
                          sizeof(kbuf)) || !kbuf[0])
        return;

    double       eff = 0, charges = 0;
    std::uint8_t trig = 0;
    mem_read_bytes(bs + OFF_SKILL_COOLDOWN, &eff,     sizeof(eff));
    mem_read_bytes(bs + OFF_SKILL_CHARGES,  &charges, sizeof(charges));
    mem_read_bytes(bs + OFF_SKILL_TRIG_CD,  &trig,    1);
    double base = 0.0;
    if (!seh_dyn_getd(inf, g_hash_cooldown, &base)) return;

    // sanity: cooldowns are seconds, small positive
    if (!(eff >= 0.0 && eff < 100000.0) || !(base >= 0.0 && base < 100000.0))
        return;

    std::lock_guard<std::mutex> lk(g_cd_mu);
    bool is_new = g_cd_cache.find(kbuf) == g_cd_cache.end();
    SkillCooldown sc;
    sc.kind      = kbuf;
    sc.effective = eff;
    sc.base      = base;
    sc.charges   = charges;
    g_cd_cache[kbuf] = sc;

    // Only real-cooldown skills that aren't mid-cooldown feed the estimate.
    if (is_new && base > 0.5 && eff > 0.0 && trig == 0) {
        double ratio = 1.0 - eff / base;
        if (ratio < 0.0)  ratio = 0.0;
        if (ratio > 0.95) ratio = 0.95;
        int bucket = static_cast<int>(ratio * 1000.0 + 0.5);
        g_cd_hist[bucket]++;
    }
}

// Fetch and validate an object's CDB record pointer (`obj->inf`). Every
// CDB-backed class in the game holds one: skills at OFF_BS_INF, items at
// OFF_ITEM_INF, activities at OFF_ACTIVITY_INF. `why` names the step that
// failed, for the trace lines.
bool inf_ptr_of(std::uintptr_t obj, std::size_t inf_offset, void** out,
                const char** why) {
    *why = "";
    if (!g_ready)                    { *why = "not_ready";   return false; }
    if (!obj)                        { *why = "obj_null";    return false; }
    if (!mem_is_userland(obj))       { *why = "obj_kernel";  return false; }
    std::uint64_t inf_u64 = 0;
    if (!mem_read_u64(obj + inf_offset, &inf_u64))
                                     { *why = "read_inf";    return false; }
    if (!inf_u64)                    { *why = "inf_null";    return false; }
    void* inf = reinterpret_cast<void*>(inf_u64);
    // Structural check before the first dyn call: a stale obj reads fine and
    // yields a garbage `inf`, which would fault inside HashLink rather than
    // here.
    if (!is_dyn_target(inf))         { *why = "inf_not_dyn";     return false; }
    *out = inf;
    return true;
}

// inf.gfx -> {file basename, cell x/y, cell size, cell span}.
bool gfx_of_inf(void* inf, SkillGfx* out, const char** why) {
    *why = "";
    void* gfx = nullptr;
    if (!seh_dyn_getp(inf, g_hash_gfx, &gfx)) { *why = "gfx_fault"; return false; }
    if (!gfx)                                 { *why = "gfx_null";  return false; }
    if (!is_dyn_target(gfx))         { *why = "gfx_not_dyn";       return false; }

    // hl_dyn_getp with result_type=hlt_dyn returns the underlying
    // pointer for a pointer-typed field - no extra vdynamic wrapper -
    // so file_str is the Haxe String object directly.
    void* file_str = nullptr;
    if (!seh_dyn_getp(gfx, g_hash_file, &file_str))
                                              { *why = "file_fault"; return false; }
    if (!file_str)                            { *why = "file_null";  return false; }
    if (!mem_is_userland(reinterpret_cast<std::uintptr_t>(file_str)))
                                              { *why = "file_kernel"; return false; }

    char file_buf[160] = {};
    if (!read_haxe_string(reinterpret_cast<std::uintptr_t>(file_str),
                          file_buf, sizeof(file_buf)))
                                              { *why = "decode_file"; return false; }

    int x = 0, y = 0, size = 0;
    if (!seh_dyn_geti(gfx, g_hash_x,    &x) ||
        !seh_dyn_geti(gfx, g_hash_y,    &y) ||
        !seh_dyn_geti(gfx, g_hash_size, &size)) { *why = "cell_fault"; return false; }
    if (size <= 0) size = 96;

    // width / height are HNULL<I32>. hl_dyn_geti coerces null to 0
    // (HashLink default). Treat any non-positive as "1 cell".
    int w = 0, h = 0;
    if (!seh_dyn_geti(gfx, g_hash_width,  &w)) w = 1;
    if (!seh_dyn_geti(gfx, g_hash_height, &h)) h = 1;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    basename_into(file_buf, out->atlas_filename, sizeof(out->atlas_filename));
    out->x      = x;
    out->y      = y;
    out->size   = size;
    out->width  = w;
    out->height = h;
    if (!out->atlas_filename[0])              { *why = "empty_file"; return false; }
    return true;
}

// Internal worker. No worker-thread bail. Caller MUST be on a safe
// thread (alloc-context inside an hl_alloc_obj watcher callback). Reads
// BaseSkill.inf.gfx (or st.Item.inf.gfx) via the hl_dyn_getp dispatch chain.
bool skill_resolve_query_impl(std::uintptr_t base_skill_ptr, SkillGfx* out,
                              std::size_t inf_offset) {
    bool trace = g_trace_left.fetch_sub(1) > 0;
    const char* why = "";

    void* inf = nullptr;
    if (!inf_ptr_of(base_skill_ptr, inf_offset, &inf, &why)) {
        if (trace) logf("skill_resolve: fail @ %s (obj=0x%llx)", why,
                        (unsigned long long)base_skill_ptr);
        return false;
    }
    if (trace) logf("skill_resolve: bs=0x%llx inf=%p",
                    (unsigned long long)base_skill_ptr, inf);

    // #96: this runs on a safe (alloc-context) thread where dyn is OK
    // (hero_state's worker tick is not). Read the skill's cooldown pair once and
    // fold it into the cooldown cache + the global cooldown-reduction estimate:
    //   base = inf.cooldown (CDB config, dyn), eff = Skill.cooldownDuration@456
    //   (runtime, with all reductions applied). 1 - eff/base is that skill's
    //   total reduction; the value most skills share is the global (gear) CDR.
    // Skills only: the OFF_SKILL_* offsets it reads belong to st.skill.Skill,
    // so running it on an st.Item would read unrelated bytes.
    if (g_dyn_getd && inf_offset == OFF_BS_INF)
        skill_cooldown_capture(base_skill_ptr, inf);

    if (!gfx_of_inf(inf, out, &why)) {
        if (trace) logf("skill_resolve: fail @ %s", why);
        return false;
    }
    if (trace) logf("skill_resolve: OK file=%s xy=(%d,%d) sz=%d wh=(%d,%d)",
                    out->atlas_filename, out->x, out->y, out->size,
                    out->width, out->height);
    return true;
}

}  // namespace (impl)

// #107: the same two reads against any CDB-backed object, for callers that
// key their own cache rather than the skill cache. Both refuse to run on the
// hl_pump worker for the reason described above skill_resolve_query.
bool cdb_gfx_at(std::uintptr_t obj, std::size_t inf_offset, SkillGfx* out) {
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) return false;
    void* inf = nullptr;
    const char* why = "";
    if (!inf_ptr_of(obj, inf_offset, &inf, &why)) return false;
    return gfx_of_inf(inf, out, &why);
}

bool cdb_inf_string(std::uintptr_t obj, std::size_t inf_offset,
                    const char* field, char* out, std::size_t cap) {
    if (!field || !out || cap == 0) return false;
    out[0] = 0;
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) return false;
    void* inf = nullptr;
    const char* why = "";
    if (!inf_ptr_of(obj, inf_offset, &inf, &why)) return false;
    // hl_hash_utf8 interns the name, so the first sight of a new string
    // allocates. Callers here run inside an hl_alloc_obj watcher, where an
    // allocation would re-enter the allocator with the caller's fresh object
    // not yet reachable from any root. The names we actually use are hashed
    // once at init to keep this path allocation-free.
    int hash = 0;
    if      (std::strcmp(field, "inherit") == 0) hash = g_hash_inherit;
    else if (std::strcmp(field, "id")      == 0) hash = g_hash_id;
    else                                         hash = g_hash(field);
    void* str = nullptr;
    if (!seh_dyn_getp(inf, hash, &str) || !str) return false;
    if (!mem_is_userland(reinterpret_cast<std::uintptr_t>(str))) return false;
    return read_haxe_string(reinterpret_cast<std::uintptr_t>(str), out, cap);
}

// Public query. From the hl_pump worker thread we bail out - hl_dyn_getp
// dispatches through HashLink's field lookup and can collide with hxbit
// deserialise on the same object. The caller (damage_tick) then queues
// a deferred resolve via skill_resolve_request_deferred which the
// alloc-context pump drains on the safe thread.
bool skill_resolve_query(std::uintptr_t base_skill_ptr, SkillGfx* out) {
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) {
        return false;
    }
    return skill_resolve_query_impl(base_skill_ptr, out, OFF_BS_INF);
}

namespace {

void request_deferred(const char* kind, std::uintptr_t obj,
                      std::size_t inf_offset) {
    if (!kind || !*kind || !obj) return;
    std::lock_guard<std::mutex> lk(g_pending_mu);
    // Already queued for this kind: refresh the pointer, keep the position.
    for (auto& p : g_pending) {
        if (std::strncmp(p.skill_name, kind, sizeof(p.skill_name) - 1) == 0) {
            p.base_skill_ptr = obj;
            p.inf_offset     = inf_offset;
            return;
        }
    }
    if (g_pending.size() >= kPendingCap) g_pending.pop_front();
    PendingResolve req{};
    req.base_skill_ptr = obj;
    req.inf_offset     = inf_offset;
    std::strncpy(req.skill_name, kind, sizeof(req.skill_name) - 1);
    g_pending.push_back(req);
}

}  // namespace

void skill_resolve_request_deferred(const char* skill_kind,
                                    std::uintptr_t base_skill_ptr) {
    request_deferred(skill_kind, base_skill_ptr, OFF_BS_INF);
}

// #105: same queue, st.Item's inf offset. Items and skills share the icon
// cache, which is what lets an item-granted status fall back to its item's
// artwork below. A kind that names both an item and a skill would collide;
// none do today, and the loser would still get a valid icon.
void skill_resolve_request_item(const char* item_kind,
                                std::uintptr_t item_ptr) {
    request_deferred(item_kind, item_ptr, OFF_ITEM_INF);
}

void skill_resolve_pump_on_game_thread() {
    // Guard: never run the dyn walk on the GC-invisible worker. NOTE that
    // damage_tick runs THERE, not on a separate render thread (the Present
    // hook only drives it), so this guard is what makes a tick-driven drain
    // impossible - the drain has to come from alloc-watcher context.
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) return;

    for (int n = 0; n < kDrainPerPump; ++n) {
        PendingResolve req;
        {
            std::lock_guard<std::mutex> lk(g_pending_mu);
            if (g_pending.empty()) return;
            req = g_pending.front();
            g_pending.pop_front();
        }
        // Someone else may have resolved this kind since it was queued.
        SkillGfx have{};
        if (skill_resolve_lookup_exact(req.skill_name, &have)) continue;

        SkillGfx gfx{};
        if (skill_resolve_query_impl(req.base_skill_ptr, &gfx,
                                     req.inf_offset)) {
            skill_resolve_cache(req.skill_name, gfx);
            // wh belongs in here too: `size` is only the coordinate step, so
            // a line without the extent cannot be checked against the artwork
            // afterwards (a talent reads sz=48 wh=2x2 for a 96 px icon).
            logf("skill_resolve: deferred resolve OK %s='%s' file=%s "
                 "xy=(%d,%d) sz=%d wh=%dx%d",
                 req.inf_offset == OFF_ITEM_INF ? "item" : "skill",
                 req.skill_name, gfx.atlas_filename, gfx.x, gfx.y, gfx.size,
                 gfx.width, gfx.height);
        }
    }
}

// #104: extra drain trigger. The ui.comp.DamageDisplay watcher only fires
// while something is taking damage, so before this every icon lookup outside
// combat returned nil. ent.Foe / ent.Hero allocate as the world streams in
// around the player, which gives an alloc context that does not depend on
// fighting. Throttled so a spawn burst does not become a dyn-walk storm.
//
// Deliberately NOT the hl_hook tail handler: that one fires after EVERY
// allocation and was disabled in v0.6.0 because it raced hxbit and froze the
// game thread. This is a per-class watcher, the same mechanism on_dd_alloc has
// used safely since then.
static void on_entity_alloc(std::uintptr_t /*obj*/) {
    static std::atomic<std::uint64_t> s_last{0};
    std::uint64_t now  = GetTickCount64();
    std::uint64_t last = s_last.load(std::memory_order_relaxed);
    if (now - last < kDrainCooldownMs) return;
    if (!s_last.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;   // another alloc claimed this window
    skill_resolve_pump_on_game_thread();
}

// #104: every skill kind that currently has an icon, so a plugin can see the
// exact ids farever.icons.skill() will answer for instead of guessing them.
std::vector<std::string> skill_resolve_cached_kinds() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> g(g_cache_mu);
    out.reserve(g_cache.size());
    for (const auto& kv : g_cache) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

void skill_resolve_cache(const char* skill_kind, const SkillGfx& gfx) {
    if (!skill_kind || !*skill_kind) return;
    std::lock_guard<std::mutex> g(g_cache_mu);
    g_cache[skill_kind] = gfx;
}

// #105: buff / proc kinds that carry no artwork of their own. The game models
// a status applied by a skill as its own kind, e.g. "Mage_ShieldOfSpark" grants
// "Mage_ShieldOfSpark_Status", and only the skill has a gfx record. A buff bar
// wants the skill's icon in that case, which is also what the game's own UI
// shows.
//
// The markers stack, so they come off one at a time rather than in a single
// pass: "Staff_SummonDemon_Combo_R3_Buff" needs "_Buff" AND the "_R3" rank
// marker removed before it reaches a kind that has artwork. The first version
// stripped exactly one suffix and left that one unresolved (gamersa22).
const char* const kStatusSuffixes[] = {
    "_Status", "_Buff", "_Shield", "_Proc", "_Accum",
};

// Guards against a pathological name eating the whole loop. Four is one more
// than the longest real chain we have seen ("_Accum_Status" plus a rank).
constexpr int kMaxSuffixStrips = 4;

// Takes one trailing marker off `k`. Returns false when the name ends in none
// of them, which is where the walk stops.
bool strip_one_suffix(std::string* k) {
    for (const char* suf : kStatusSuffixes) {
        std::size_t n = std::strlen(suf);
        if (k->size() > n && k->compare(k->size() - n, n, suf) == 0) {
            k->resize(k->size() - n);
            return true;
        }
    }
    // Rank marker: "_R3", "_R12". Ranked variants share the base skill's
    // artwork. Only strip it when everything after the "_R" is digits, so a
    // skill that genuinely ends in "_Rush" keeps its name.
    std::size_t p = k->rfind("_R");
    if (p == std::string::npos || p == 0 || p + 2 >= k->size()) return false;
    for (std::size_t i = p + 2; i < k->size(); ++i)
        if (!std::isdigit((unsigned char)(*k)[i])) return false;
    k->resize(p);
    return true;
}

bool skill_resolve_lookup_exact(const char* skill_kind, SkillGfx* out) {
    if (!skill_kind || !*skill_kind) return false;
    std::lock_guard<std::mutex> g(g_cache_mu);
    auto it = g_cache.find(skill_kind);
    if (it == g_cache.end()) return false;
    *out = it->second;
    return true;
}

bool skill_resolve_lookup(const char* skill_kind, SkillGfx* out) {
    if (!skill_kind || !*skill_kind) return false;
    std::lock_guard<std::mutex> g(g_cache_mu);
    auto it = g_cache.find(skill_kind);
    if (it != g_cache.end()) {
        *out = it->second;
        return true;
    }
    // Fall back to the granting skill's icon, peeling one marker at a time.
    // The cache is re-checked after every strip, so a kind that does have its
    // own artwork always wins over the shortened one.
    std::string k(skill_kind);
    for (int step = 0; step < kMaxSuffixStrips; ++step) {
        if (!strip_one_suffix(&k)) break;
        auto base = g_cache.find(k);
        if (base != g_cache.end()) {
            *out = base->second;
            return true;
        }
    }
    return false;
}

// #96: global (gear) cooldown reduction, estimated as the reduction fraction
// most cooldown-bearing skills share. 0 until skills have been resolved (played
// a little). Pure map read - safe from any thread (no dyn), incl. the worker.
double skill_global_cooldown_reduction() {
    std::lock_guard<std::mutex> lk(g_cd_mu);
    int best_bucket = 0, best_count = 0;
    for (const auto& kv : g_cd_hist)
        if (kv.second > best_count) { best_count = kv.second; best_bucket = kv.first; }
    return best_count > 0 ? best_bucket / 1000.0 : 0.0;
}

// #96: snapshot of every resolved skill's effective/base cooldown + charges.
std::vector<SkillCooldown> skill_cooldown_snapshot() {
    std::lock_guard<std::mutex> lk(g_cd_mu);
    std::vector<SkillCooldown> out;
    out.reserve(g_cd_cache.size());
    for (const auto& kv : g_cd_cache) out.push_back(kv.second);
    return out;
}

}  // namespace farever
