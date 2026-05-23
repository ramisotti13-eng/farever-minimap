// Pulls BaseSkill.inf.gfx (atlas + cell coords) out of the live
// HashLink heap by calling hl_dyn_get* on the virtual struct chain.
// The chain is:
//
//   BaseSkill   @ HOBJ at  baseSkill_ptr
//     +152     -> inf : HVIRTUAL  ( = vvirtual*, the CDB row record )
//                  .gfx : HVIRTUAL ( file: String, x: I32, y: I32,
//                                    size: I32, width: ?, height: ? )
//
// We cache per-skill-name so the dyn calls only run once per kind.
//
// v0.4.17 backport from v0.6.0: split into worker-aware public
// wrapper + internal impl, plus single-slot deferred queue that
// damage_tick can hand off to when called from the hl_pump worker.
// The pump drains in alloc-hook context (hxbit-safe).

#include "skill_resolve.h"
#include "hl_pump.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace farever {
namespace {

constexpr std::size_t OFF_BS_INF      = 152;  // BaseSkill.inf — v0.4.16 +8 shift
constexpr std::size_t OFF_STR_BYTES   = 8;
constexpr std::size_t OFF_STR_LEN     = 16;

// HashLink-side function pointer types. Mirroring the exports we
// resolved in libhl.cpp.
using HlDynGetP    = void* (*)(void* d, int hash, void* result_type);
using HlDynGetI    = int   (*)(void* d, int hash, void* result_type);
using HlHashUtf8   = int   (*)(const char* utf8);

LibHL          g_libhl{};
HlDynGetP      g_dyn_getp = nullptr;
HlDynGetI      g_dyn_geti = nullptr;
HlHashUtf8     g_hash     = nullptr;
void*          g_hlt_dyn  = nullptr;
void*          g_hlt_i32  = nullptr;
void*          g_hlt_bytes = nullptr;
bool           g_ready    = false;

int            g_hash_gfx    = 0;
int            g_hash_file   = 0;
int            g_hash_x      = 0;
int            g_hash_y      = 0;
int            g_hash_size   = 0;
int            g_hash_width  = 0;
int            g_hash_height = 0;

std::mutex                                g_cache_mu;
std::unordered_map<std::string, SkillGfx> g_cache;

// v0.4.17 deferred slot. Worker sets this when its query bails; the
// alloc-context pump drains it. Single slot, overwrite-latest. Combat
// allocates DDs frequently enough that the pump catches up.
struct PendingResolve {
    std::uintptr_t base_skill_ptr;
    char           skill_name[64];
};
std::mutex     g_pending_mu;
PendingResolve g_pending{};
bool           g_pending_set = false;

// Read a Haxe String (HOBJ:String at `str_ptr` — { bytes: vbytes*, len: i32 })
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
// basename — that's what we use as the atlas key.
void basename_into(const char* full, char* dst, std::size_t cap) {
    const char* slash = std::strrchr(full, '/');
    const char* base  = slash ? slash + 1 : full;
    std::size_t n = std::strlen(base);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, base, n);
    dst[n] = 0;
}

}  // namespace

void skill_resolve_init(const LibHL& libhl) {
    g_libhl     = libhl;
    g_dyn_getp  = reinterpret_cast<HlDynGetP >(libhl.hl_dyn_getp);
    g_dyn_geti  = reinterpret_cast<HlDynGetI >(libhl.hl_dyn_geti);
    g_hash      = reinterpret_cast<HlHashUtf8>(libhl.hl_hash_utf8);
    g_hlt_dyn   = libhl.hlt_dyn;
    g_hlt_i32   = libhl.hlt_i32;
    g_hlt_bytes = libhl.hlt_bytes;

    g_ready = g_dyn_getp && g_dyn_geti && g_hash &&
              g_hlt_dyn && g_hlt_i32 && g_hlt_bytes;
    if (!g_ready) {
        logf("skill_resolve: missing exports — won't auto-map icons");
        return;
    }
    g_hash_gfx    = g_hash("gfx");
    g_hash_file   = g_hash("file");
    g_hash_x      = g_hash("x");
    g_hash_y      = g_hash("y");
    g_hash_size   = g_hash("size");
    g_hash_width  = g_hash("width");
    g_hash_height = g_hash("height");
    logf("skill_resolve: ready (gfx=%d file=%d x=%d y=%d size=%d "
         "width=%d height=%d)",
         g_hash_gfx, g_hash_file, g_hash_x, g_hash_y, g_hash_size,
         g_hash_width, g_hash_height);
}

// First N calls log every step so we can diagnose why resolution
// fails. After that the call is silent (steady state).
std::atomic<int> g_trace_left{8};

namespace {

// Internal worker. No worker-thread bail. Caller MUST be on a safe
// thread (Present hook on render thread, or alloc-context inside an
// hl_alloc_obj watcher callback). Reads BaseSkill.inf.gfx via the
// hl_dyn_getp dispatch chain.
bool skill_resolve_query_impl(std::uintptr_t base_skill_ptr, SkillGfx* out) {
    bool trace = g_trace_left.fetch_sub(1) > 0;
    auto fail = [&](const char* where, std::uint64_t v = 0) {
        if (trace) logf("skill_resolve: fail @ %s (val=0x%llx)",
                        where, (unsigned long long)v);
        return false;
    };

    if (!g_ready)                                  return fail("not_ready");
    if (!base_skill_ptr)                           return fail("bs_null");
    if (!mem_is_userland(base_skill_ptr))          return fail("bs_kernel", base_skill_ptr);

    std::uint64_t inf_u64 = 0;
    if (!mem_read_u64(base_skill_ptr + OFF_BS_INF, &inf_u64))
        return fail("read_inf", base_skill_ptr);
    if (!inf_u64)                                  return fail("inf_null");
    if (!mem_is_userland(inf_u64))                 return fail("inf_kernel", inf_u64);

    void* inf = reinterpret_cast<void*>(inf_u64);
    if (trace) logf("skill_resolve: bs=0x%llx inf=0x%llx",
                    (unsigned long long)base_skill_ptr,
                    (unsigned long long)inf_u64);

    void* gfx = g_dyn_getp(inf, g_hash_gfx, g_hlt_dyn);
    if (trace) logf("skill_resolve: gfx=%p", gfx);
    if (!gfx)                                              return fail("gfx_null");
    if (!mem_is_userland(reinterpret_cast<std::uintptr_t>(gfx)))
        return fail("gfx_kernel");

    void* file_str = g_dyn_getp(gfx, g_hash_file, g_hlt_dyn);
    if (trace) logf("skill_resolve: file_str=%p", file_str);
    if (!file_str)                                         return fail("file_null");
    if (!mem_is_userland(reinterpret_cast<std::uintptr_t>(file_str)))
        return fail("file_kernel");

    char file_buf[160] = {};
    if (!read_haxe_string(reinterpret_cast<std::uintptr_t>(file_str),
                          file_buf, sizeof(file_buf)))
        return fail("decode_file_str",
                    reinterpret_cast<std::uintptr_t>(file_str));

    int x    = g_dyn_geti(gfx, g_hash_x,    g_hlt_i32);
    int y    = g_dyn_geti(gfx, g_hash_y,    g_hlt_i32);
    int size = g_dyn_geti(gfx, g_hash_size, g_hlt_i32);
    if (size <= 0) size = 96;

    int w = g_dyn_geti(gfx, g_hash_width,  g_hlt_i32);
    int h = g_dyn_geti(gfx, g_hash_height, g_hlt_i32);
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    basename_into(file_buf, out->atlas_filename, sizeof(out->atlas_filename));
    out->x      = x;
    out->y      = y;
    out->size   = size;
    out->width  = w;
    out->height = h;
    if (trace) logf("skill_resolve: OK file=%s xy=(%d,%d) sz=%d wh=(%d,%d)",
                    out->atlas_filename, x, y, size, w, h);
    return out->atlas_filename[0] != 0;
}

}  // namespace (impl)

// Public query. From the hl_pump worker thread we bail out — hl_dyn_getp
// dispatches through HashLink's field lookup and can collide with hxbit
// deserialise on the same object. The caller (damage_tick) then queues
// a deferred resolve via skill_resolve_request_deferred which the
// alloc-context pump drains on the safe thread.
bool skill_resolve_query(std::uintptr_t base_skill_ptr, SkillGfx* out) {
    unsigned long worker = hl_pump_worker_tid();
    if (worker != 0 && GetCurrentThreadId() == worker) {
        return false;
    }
    return skill_resolve_query_impl(base_skill_ptr, out);
}

void skill_resolve_request_deferred(const char* skill_kind,
                                    std::uintptr_t base_skill_ptr) {
    if (!skill_kind || !*skill_kind || !base_skill_ptr) return;
    std::lock_guard<std::mutex> lk(g_pending_mu);
    g_pending.base_skill_ptr = base_skill_ptr;
    std::strncpy(g_pending.skill_name, skill_kind,
                 sizeof(g_pending.skill_name) - 1);
    g_pending.skill_name[sizeof(g_pending.skill_name) - 1] = 0;
    g_pending_set = true;
}

void skill_resolve_pump_in_alloc_context() {
    PendingResolve req;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        if (!g_pending_set) return;
        req = g_pending;
        g_pending_set = false;
    }
    SkillGfx gfx{};
    if (skill_resolve_query_impl(req.base_skill_ptr, &gfx)) {
        skill_resolve_cache(req.skill_name, gfx);
        logf("skill_resolve: deferred resolve OK skill='%s' file=%s xy=(%d,%d) sz=%d",
             req.skill_name, gfx.atlas_filename, gfx.x, gfx.y, gfx.size);
    }
}

void skill_resolve_cache(const char* skill_kind, const SkillGfx& gfx) {
    if (!skill_kind || !*skill_kind) return;
    std::lock_guard<std::mutex> g(g_cache_mu);
    g_cache[skill_kind] = gfx;
}

bool skill_resolve_lookup(const char* skill_kind, SkillGfx* out) {
    if (!skill_kind || !*skill_kind) return false;
    std::lock_guard<std::mutex> g(g_cache_mu);
    auto it = g_cache.find(skill_kind);
    if (it == g_cache.end()) return false;
    *out = it->second;
    return true;
}

}  // namespace farever
