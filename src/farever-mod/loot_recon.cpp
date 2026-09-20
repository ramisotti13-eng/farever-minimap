// Atlas Loot Stage 1 - recon module, chest-centric variant.
//
// Bosses don't drop loot themselves - they spawn a chest, and the
// chest's CDB record carries the loot table. We hook every Chest
// alloc, settle it for ~3 seconds so the constructor + hxbit
// deserialiser finish, then walk:
//
//   chest + 632         -> inf      (HVIRTUAL[9], the CDB Chest row) [v023]
//     dyn_getp("id")           -> Haxe String   (chest id)
//     dyn_getp("props")        -> HVIRTUAL      (loot pointers)
//       dyn_getp("lootTable")        -> LootTable obj
//         dyn_getp("id")               -> Haxe String   (lt id)
//         dyn_getp("loot")             -> hl.types.ArrayObj
//
// First success per `<chest-id, lt-id>` is logged, then deduped.
//
// Field-discovery aid: if `props` resolves but `lootTable` does NOT,
// we additionally probe a list of plausible alternative names so the
// log tells us what the schema actually looks like.

#include "loot_recon.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace farever {
namespace {

// Chest layout, verified via classes.json. v0.5.5: ent.Serializable +8 shift.
constexpr std::size_t OFF_CHEST_INF = 632;   // was 624 [v023 +8]

// Haxe String layout (HOBJ:String).
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

// hl.types.ArrayObj layout:
//   length : i32   @ +8
//   array  : HARRAY @ +16   (varray*)
// varray user data starts at +24 (after t/at/size/__padding).
constexpr std::size_t OFF_AOBJ_LEN   = 8;
constexpr std::size_t OFF_AOBJ_ARR   = 16;
constexpr std::size_t OFF_VARRAY_DATA = 24;

constexpr int kSettleFrames    = 240;  // ~4 s @ 60 Hz before first attempt
constexpr int kMaxRetries      = 600;  // hard cap at ~10 s (was 20 s, too long)
constexpr int kMaxChestsPerTick = 1;   // throttle dyn_getp pressure on Present
constexpr std::size_t kMaxPendingChests = 64; // ring cap so dungeon entry bursts don't pile up

using HlDynGetP  = void*  (*)(void* d, int hash, void* result_type);
using HlDynGetI  = int    (*)(void* d, int hash, void* result_type);
using HlDynGetD  = double (*)(void* d, int hash);
using HlHashUtf8 = int    (*)(const char* utf8);

LibHL       g_libhl{};
HlDynGetP   g_getp = nullptr;
HlDynGetI   g_geti = nullptr;
HlDynGetD   g_getd = nullptr;
HlHashUtf8  g_hash = nullptr;
void*       g_hlt_dyn = nullptr;
void*       g_hlt_i32 = nullptr;

// Cached hashes for the names we look up most often.
int g_h_id           = 0;
int g_h_props        = 0;
int g_h_lootTable    = 0;
int g_h_loot         = 0;
int g_h_proba        = 0;
int g_h_item         = 0;
int g_h_itemMin      = 0;
int g_h_itemMax      = 0;
int g_h_minLvl       = 0;
int g_h_kind         = 0;
int g_h_texts        = 0;
int g_h_name         = 0;

bool g_ready = false;

struct Pending {
    std::uintptr_t obj;
    int            retries;
};

std::atomic<bool>                  g_active{false};
std::mutex                         g_mu;
std::deque<Pending>                g_pending;
std::unordered_set<std::string>    g_done_keys;   // dedupe by chest_id + lt_id

// Schema-discovery probe list. When we can't find lootTable on
// `props`, we try these names and log the ones that resolve.
const char* const kProbeFieldsOnProps[] = {
    "lootTable", "lootTableId",
    "bossLootTable", "bossLootTableId",
    "loot", "lootList", "lootKind",
    "rewards", "reward", "rewardTable",
    "drops", "drop", "dropTable",
    "rewardLootTable", "overrideLootTable",
    "gainItem", "items", "gains",
    nullptr,
};
const char* const kProbeFieldsOnInf[] = {
    "id", "props", "loot", "lootTable", "lootTableId",
    "model", "kind", "type", "name", "rewards",
    nullptr,
};

// Read a Haxe String (HOBJ:String, ASCII-clipped) into out.
bool read_haxe_ascii(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    out[0] = 0;
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_u64))) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 200) return false;

    std::uint8_t buf[512];
    std::size_t nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_u64), buf, nb))
        return false;

    std::size_t w = 0;
    for (int i = 0; i < length && w + 1 < cap; ++i) {
        std::uint16_t c = (std::uint16_t)buf[i * 2] |
                          ((std::uint16_t)buf[i * 2 + 1] << 8);
        if (c >= 128) c = '?';
        out[w++] = (char)c;
    }
    out[w] = 0;
    return true;
}

// Wrapped HashLink accessors. dyn_getp returns the raw pointer for
// pointer-typed fields, NULL for missing or value-typed fields when
// the caller passes hlt_dyn (those box into vdynamic, which here we
// don't use). The userland check rejects obvious garbage.
void* safe_getp(void* obj, int hash) {
    if (!obj) return nullptr;
    void* r = nullptr;
    __try {
        r = g_getp(obj, hash, g_hlt_dyn);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (r && !mem_is_userland(reinterpret_cast<std::uintptr_t>(r)))
        return nullptr;
    return r;
}

int safe_geti(void* obj, int hash) {
    if (!obj) return 0;
    __try { return g_geti(obj, hash, g_hlt_i32); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

double safe_getd(void* obj, int hash) {
    if (!obj || !g_getd) return 0.0;
    __try { return g_getd(obj, hash); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; }
}

// Decode `item.id`. The item field can be a String directly (the
// item id) or an Item virtual whose .id is a String - try both.
void decode_item_id(void* item, char* out, std::size_t cap) {
    out[0] = 0;
    if (!item) return;
    auto item_addr = reinterpret_cast<std::uintptr_t>(item);

    // Path A - item is a String already (HOBJ:String layout).
    if (read_haxe_ascii(item_addr, out, cap) && out[0]) return;

    // Path B - item is a virtual; chase .id.
    void* iid = safe_getp(item, g_h_id);
    if (iid && read_haxe_ascii(reinterpret_cast<std::uintptr_t>(iid),
                               out, cap) && out[0]) return;

    out[0] = 0;
}

void log_loot_table(const char* chest_id, void* lt) {
    if (!lt) return;
    char lt_id[96] = {};
    if (void* lt_id_str = safe_getp(lt, g_h_id)) {
        read_haxe_ascii(reinterpret_cast<std::uintptr_t>(lt_id_str),
                        lt_id, sizeof(lt_id));
    }

    void* loot_arr = safe_getp(lt, g_h_loot);
    if (!loot_arr) {
        logf("loot_recon: chest=%s lt=%s (no .loot array on LT)",
             chest_id, lt_id);
        return;
    }
    auto arr_addr = reinterpret_cast<std::uintptr_t>(loot_arr);
    std::int32_t  length     = 0;
    std::uint64_t varray_u64 = 0;
    if (!mem_read_i32(arr_addr + OFF_AOBJ_LEN, &length)) return;
    if (!mem_read_u64(arr_addr + OFF_AOBJ_ARR, &varray_u64)) return;
    if (length <= 0 || length > 200) {
        logf("loot_recon: chest=%s lt=%s implausible length=%d",
             chest_id, lt_id, length);
        return;
    }
    if (!mem_is_userland(varray_u64)) return;

    logf("loot_recon: chest=%s lt=%s entries=%d",
         chest_id, lt_id, length);

    std::uintptr_t data = static_cast<std::uintptr_t>(varray_u64) +
                          OFF_VARRAY_DATA;
    int cap = length > 100 ? 100 : length;
    for (int i = 0; i < cap; ++i) {
        std::uint64_t entry_u64 = 0;
        if (!mem_read_u64(data + i * 8, &entry_u64)) continue;
        if (!mem_is_userland(entry_u64)) continue;
        void* le = reinterpret_cast<void*>(entry_u64);

        double proba   = safe_getd(le, g_h_proba);
        int    itemMin = safe_geti(le, g_h_itemMin);
        int    itemMax = safe_geti(le, g_h_itemMax);
        int    lvl     = safe_geti(le, g_h_minLvl);
        void*  item    = safe_getp(le, g_h_item);

        char item_id[96] = {};
        decode_item_id(item, item_id, sizeof(item_id));

        logf("  [%2d] item=%-40s proba=%.4f min=%d max=%d lvl=%d",
             i, item_id[0] ? item_id : "<unresolved>",
             proba, itemMin, itemMax, lvl);
    }
}

// Probe-and-log unknown schema. Once we know which names actually
// exist this can be deleted.
void probe_fields(const char* tag, void* obj, const char* const* names) {
    if (!obj) return;
    for (const char* const* p = names; *p; ++p) {
        int h = g_hash(*p);
        void* r = safe_getp(obj, h);
        if (r) {
            logf("loot_recon: %s.%s -> 0x%llx",
                 tag, *p,
                 static_cast<unsigned long long>(
                     reinterpret_cast<std::uintptr_t>(r)));
        }
    }
}

// True = decoded (drop from pending). False = retry next tick.
bool try_walk(std::uintptr_t obj) {
    // Type-anchor defense: every HL object has its hl_type* at +0.
    // If the GC reused this slot for a non-Chest object, the type
    // pointer no longer matches the cached Chest hl_type and we
    // must NOT walk fields - they would all read into wrong layouts.
    std::uintptr_t expected = hl_hook_get_type(L"ent.interactible.Chest");
    if (expected) {
        std::uint64_t actual = 0;
        if (!mem_read_u64(obj, &actual)) return false;
        if (actual != expected) return false;
    }

    std::uint64_t inf_u64 = 0;
    if (!mem_read_u64(obj + OFF_CHEST_INF, &inf_u64)) return false;
    if (!mem_is_userland(inf_u64)) return false;

    void* inf = reinterpret_cast<void*>(inf_u64);
    void* id_str = safe_getp(inf, g_h_id);
    if (!id_str) return false;

    char chest_id[96] = {};
    if (!read_haxe_ascii(reinterpret_cast<std::uintptr_t>(id_str),
                         chest_id, sizeof(chest_id))) {
        return false;
    }
    if (!chest_id[0]) return false;

    // Dedupe by chest_id - log each unique chest type only once.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_done_keys.insert(chest_id).second) return true;
    }

    logf("loot_recon: ==== chest id=%s @0x%llx ====",
         chest_id, static_cast<unsigned long long>(obj));

    // Helpful: log every inf-level field we probe for.
    probe_fields("inf", inf, kProbeFieldsOnInf);

    void* props = safe_getp(inf, g_h_props);
    if (!props) {
        logf("loot_recon: chest=%s props=NULL", chest_id);
        return true;
    }

    void* lt = safe_getp(props, g_h_lootTable);
    if (lt) {
        log_loot_table(chest_id, lt);
    } else {
        // Schema discovery: probe alternative names so the next iter
        // knows what the field is actually called.
        logf("loot_recon: chest=%s lootTable=NULL, probing props ...",
             chest_id);
        probe_fields("props", props, kProbeFieldsOnProps);

        // Some chests put the loot table on inf directly (not nested
        // under props) - try that too.
        if (void* lt2 = safe_getp(inf, g_h_lootTable)) {
            logf("loot_recon: chest=%s found inf.lootTable directly",
                 chest_id);
            log_loot_table(chest_id, lt2);
        }
    }
    return true;
}

void on_chest_alloc(std::uintptr_t obj) {
    if (!obj || !g_active.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending.push_back({obj, 0});
    // Tight cap so dungeon-entry bursts of ~hundreds of chest allocs
    // don't queue up an enormous chain-walk backlog (which is what
    // caused the access violation in DX12Driver.present - we were
    // holding stale pointers across GC cycles, then hammering them
    // with dyn_getp on the render thread).
    while (g_pending.size() > kMaxPendingChests) g_pending.pop_front();
}

}  // namespace

void loot_recon_start(const LibHL& libhl) {
    if (g_active.exchange(true)) return;
    g_libhl = libhl;
    g_getp  = reinterpret_cast<HlDynGetP >(libhl.hl_dyn_getp);
    g_geti  = reinterpret_cast<HlDynGetI >(libhl.hl_dyn_geti);
    g_getd  = reinterpret_cast<HlDynGetD >(libhl.hl_dyn_getd);
    g_hash  = reinterpret_cast<HlHashUtf8>(libhl.hl_hash_utf8);
    g_hlt_dyn = libhl.hlt_dyn;
    g_hlt_i32 = libhl.hlt_i32;

    g_ready = g_getp && g_geti && g_hash && g_hlt_dyn && g_hlt_i32;
    if (!g_ready) {
        logf("loot_recon: missing libhl exports - disabled "
             "(getp=%p geti=%p getd=%p hash=%p hlt_dyn=%p hlt_i32=%p)",
             g_getp, g_geti, g_getd, g_hash, g_hlt_dyn, g_hlt_i32);
        g_active.store(false);
        return;
    }

    g_h_id        = g_hash("id");
    g_h_props     = g_hash("props");
    g_h_lootTable = g_hash("lootTable");
    g_h_loot      = g_hash("loot");
    g_h_proba     = g_hash("proba");
    g_h_item      = g_hash("item");
    g_h_itemMin   = g_hash("itemMin");
    g_h_itemMax   = g_hash("itemMax");
    g_h_minLvl    = g_hash("minLvl");
    g_h_kind      = g_hash("kind");
    g_h_texts     = g_hash("texts");
    g_h_name      = g_hash("name");

    logf("loot_recon: hashes id=%d props=%d lootTable=%d loot=%d "
         "proba=%d item=%d itemMin=%d itemMax=%d minLvl=%d",
         g_h_id, g_h_props, g_h_lootTable, g_h_loot,
         g_h_proba, g_h_item, g_h_itemMin, g_h_itemMax, g_h_minLvl);

    hl_hook_register(L"ent.interactible.Chest", on_chest_alloc);
    logf("loot_recon: watcher registered on ent.interactible.Chest");
}

void loot_recon_stop() {
    g_active.store(false);
}

void loot_recon_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    if (!g_ready) return;

    // Take at most kMaxChestsPerTick ready entries off the front of
    // the queue. Everything else stays put (counters incremented in
    // place). This bounds the per-frame dyn_getp pressure and means
    // a bad pointer can't cascade into hundreds of crashes per frame.
    std::vector<Pending> work;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        int taken = 0;
        for (auto it = g_pending.begin(); it != g_pending.end(); ) {
            it->retries++;
            if (it->retries > kMaxRetries) {
                it = g_pending.erase(it);
                continue;
            }
            if (taken < kMaxChestsPerTick && it->retries >= kSettleFrames) {
                work.push_back(*it);
                it = g_pending.erase(it);
                ++taken;
                continue;
            }
            ++it;
        }
    }

    for (const Pending& p : work) {
        // Validate again on the render thread - the chest may have been
        // GC-replaced or unmapped during the 4 s settle window.
        if (!mem_is_userland(p.obj)) continue;
        try_walk(p.obj);   // single-shot: success or quiet drop, no retry
    }
}

}  // namespace farever
