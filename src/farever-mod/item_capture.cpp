// Atlas Loot Phase 2 - runtime item capture.
//
// Hooks every `st.Item` allocation the game performs (world drops,
// inventory loads, vendor stock, crafting output, ...). For each
// fresh item we let the Haxe constructor settle, then read its CDB
// `inf` virtual once and persist the static data - id, atlas icon
// coords, level / iLevel / sellPrice / rarity - to
// `data/item_captures.tsv`. The Atlas UI joins this against the
// offline cdb_atlas catalog and renders icons + stats next to the
// names that are already there.
//
// Defensive pattern is identical to loot_recon.cpp:
//   * pending queue capped at 64
//   * type anchor on `*(uintptr_t*)obj` before walking
//   * settle delay (4 s) so the constructor + hxbit have populated
//   * single-shot - success or quiet drop, no retry on the same ptr
//   * one item per tick to bound dyn_getp pressure on Present

#include "item_capture.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace farever {
namespace {

// st.Item.inf is HVIRTUAL[17] at +96 (verified in classes.json).
constexpr std::size_t OFF_ITEM_INF = 96;
// ui.comp.ItemSlot.inf is HVIRTUAL[17] at +1104 -- same Item record
// schema, just reached from the inventory slot UI element. Hooking
// this in addition to st.Item gives us coverage for items that were
// already in the player's inventory when the mod loaded (those
// st.Item allocs fired before hl_hook was installed and are not
// re-emitted afterwards).
constexpr std::size_t OFF_ITEMSLOT_INF = 1104;

// Haxe String layout (matches skill_resolve / damage decoder).
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

constexpr int kSettleFrames    = 240;   // ~4 s @ 60 Hz
constexpr int kMaxRetries      = 600;
constexpr int kMaxItemsPerTick = 1;
constexpr std::size_t kMaxPending = 64;

using HlDynGetP  = void*  (*)(void* d, int hash, void* result_type);
using HlDynGetI  = int    (*)(void* d, int hash, void* result_type);
using HlHashUtf8 = int    (*)(const char* utf8);

LibHL       g_libhl{};
HlDynGetP   g_getp = nullptr;
HlDynGetI   g_geti = nullptr;
HlHashUtf8  g_hash = nullptr;
void*       g_hlt_dyn = nullptr;
void*       g_hlt_i32 = nullptr;

int g_h_id        = 0;
int g_h_gfx       = 0;
int g_h_file      = 0;
int g_h_x         = 0;
int g_h_y         = 0;
int g_h_size      = 0;
int g_h_width     = 0;
int g_h_height    = 0;
int g_h_level     = 0;
int g_h_iLevel    = 0;
int g_h_sellPrice = 0;
int g_h_rarityId  = 0;

bool g_ready = false;

struct Pending {
    std::uintptr_t obj;
    std::size_t    inf_offset;   // 96 for st.Item, 1104 for ui.comp.ItemSlot
    const wchar_t* expected_class;
    int            retries;
};

std::atomic<bool>                          g_active{false};
std::mutex                                 g_mu;
std::deque<Pending>                        g_pending;
std::unordered_map<std::string, ItemCapture> g_captures;

std::wstring my_dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&my_dll_dir),
        &hmod);
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(buf);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    s.resize(pos);
    return s;
}

std::wstring captures_path() {
    return my_dll_dir() + L"\\data\\item_captures.tsv";
}

// Read a Haxe String (HOBJ:String, ASCII-clipped) into out. Returns
// false on any layout or memory anomaly.
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

// Reduce a full path like "UI/icons/atlas_weapon_Sword1H_96PX.png" to
// the basename - that's the key used by the atlas loader.
void basename_into(const char* full, char* dst, std::size_t cap) {
    const char* slash = std::strrchr(full, '/');
    const char* base  = slash ? slash + 1 : full;
    std::size_t n = std::strlen(base);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, base, n);
    dst[n] = 0;
}

void persist(const ItemCapture& cap) {
    // Append a single row to the TSV. Header is written on first
    // append (when the file is empty).
    std::wstring path = captures_path();
    std::wstring dir  = my_dll_dir() + L"\\data";
    CreateDirectoryW(dir.c_str(), nullptr);

    bool need_header = false;
    {
        std::ifstream probe(path);
        need_header = !probe || probe.peek() == std::ifstream::traits_type::eof();
    }
    std::ofstream f(path, std::ios::app);
    if (!f) {
        logf("item_capture: failed to open %ls for append", path.c_str());
        return;
    }
    if (need_header) {
        f << "id\tatlas\tx\ty\tsize\twidth\theight\tlevel\tiLevel\t"
             "sellPrice\trarityId\n";
    }
    f << cap.id << '\t' << cap.atlas << '\t'
      << cap.gfx_x << '\t' << cap.gfx_y << '\t'
      << cap.gfx_size << '\t' << cap.gfx_width << '\t' << cap.gfx_height
      << '\t' << cap.level << '\t' << cap.ilvl << '\t'
      << cap.sell_price << '\t' << cap.rarity_id << '\n';
}

// True = decoded (drop from pending). False = retry next tick.
bool try_walk(std::uintptr_t obj, std::size_t inf_offset,
              const wchar_t* expected_class) {
    std::uintptr_t expected = hl_hook_get_type(expected_class);
    if (expected) {
        std::uint64_t actual = 0;
        if (!mem_read_u64(obj, &actual)) return false;
        if (actual != expected) return false;
    }

    std::uint64_t inf_u64 = 0;
    if (!mem_read_u64(obj + inf_offset, &inf_u64)) return false;
    if (!mem_is_userland(inf_u64)) return false;

    void* inf = reinterpret_cast<void*>(inf_u64);
    void* id_str = safe_getp(inf, g_h_id);
    if (!id_str) return false;

    char item_id[96] = {};
    if (!read_haxe_ascii(reinterpret_cast<std::uintptr_t>(id_str),
                         item_id, sizeof(item_id))) return false;
    if (!item_id[0]) return false;

    // Dedupe - only first encounter per item id is captured.
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_captures.find(item_id) != g_captures.end()) return true;
    }

    ItemCapture cap;
    cap.id = item_id;

    // gfx (HVIRTUAL: file: String, x: i32, y: i32, size: i32, width, height).
    if (void* gfx = safe_getp(inf, g_h_gfx)) {
        if (void* file_str = safe_getp(gfx, g_h_file)) {
            char buf[160] = {};
            if (read_haxe_ascii(reinterpret_cast<std::uintptr_t>(file_str),
                                buf, sizeof(buf))) {
                char base[96] = {};
                basename_into(buf, base, sizeof(base));
                cap.atlas = base;
            }
        }
        cap.gfx_x      = safe_geti(gfx, g_h_x);
        cap.gfx_y      = safe_geti(gfx, g_h_y);
        cap.gfx_size   = safe_geti(gfx, g_h_size);
        int w = safe_geti(gfx, g_h_width);
        int h = safe_geti(gfx, g_h_height);
        cap.gfx_width  = w > 0 ? w : 1;
        cap.gfx_height = h > 0 ? h : 1;
        if (cap.gfx_size <= 0) cap.gfx_size = 96;
    }

    cap.level      = safe_geti(inf, g_h_level);
    cap.ilvl       = safe_geti(inf, g_h_iLevel);
    cap.sell_price = safe_geti(inf, g_h_sellPrice);
    if (void* rarity_str = safe_getp(inf, g_h_rarityId)) {
        char buf[32] = {};
        if (read_haxe_ascii(reinterpret_cast<std::uintptr_t>(rarity_str),
                            buf, sizeof(buf))) {
            cap.rarity_id = buf;
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        // Race: another thread may have inserted while we were reading.
        if (g_captures.emplace(cap.id, cap).second) {
            logf("item_capture: %s atlas=%s xy=(%d,%d) sz=%d wh=(%d,%d) "
                 "lvl=%d ilvl=%d rarity=%s",
                 cap.id.c_str(), cap.atlas.c_str(),
                 cap.gfx_x, cap.gfx_y, cap.gfx_size,
                 cap.gfx_width, cap.gfx_height,
                 cap.level, cap.ilvl, cap.rarity_id.c_str());
            persist(cap);
        }
    }
    return true;
}

// One callback per (class, inf_offset) variant. Subclasses don't get
// caught by parent-class watchers (hl_alloc_obj dispatches by exact
// type), so we have to register each concrete class explicitly.
#define ITEM_CB(SHORT, CLASS, OFFSET)                                        \
    void on_##SHORT##_alloc(std::uintptr_t obj) {                            \
        if (!obj || !g_active.load()) return;                                \
        std::lock_guard<std::mutex> lk(g_mu);                                \
        g_pending.push_back({obj, OFFSET, CLASS, 0});                        \
        while (g_pending.size() > kMaxPending) g_pending.pop_front();        \
    }

// st.Item and its subclasses -- the actual item entities live here.
// Weapons are st.item.Weapon, armor is st.item.Armor etc., and each
// allocates with its own hl_type pointer, so we register them all.
ITEM_CB(stitem,       L"st.Item",          OFF_ITEM_INF)
ITEM_CB(stgear,       L"st.item.Gear",     OFF_ITEM_INF)
ITEM_CB(stweapon,     L"st.item.Weapon",   OFF_ITEM_INF)
ITEM_CB(starmor,      L"st.item.Armor",    OFF_ITEM_INF)
ITEM_CB(stmastery,    L"st.item.Mastery",  OFF_ITEM_INF)
ITEM_CB(strecipe,     L"st.item.Recipe",   OFF_ITEM_INF)

// UI elements that carry an Item.inf reference. These are alloced any
// time the user opens an inventory / vendor / chest preview / weapon
// list etc., so they're the right catch-all for items the player
// already had at character load (those st.Item allocs happened before
// our hook installed and won't fire again).
ITEM_CB(uislot,       L"ui.comp.ItemSlot",          1104)
ITEM_CB(uibutton,     L"ui.comp.ItemButton",        1072)
ITEM_CB(uilister,     L"ui.comp.ItemListerButton",  1104)
ITEM_CB(uiweaponlist, L"ui.comp.WeaponListerButton",1104)

// Ground drops, when a foe / chest spawns a LootDrop entity.
ITEM_CB(lootdrop,     L"ent.interactible.LootDrop", 632)

#undef ITEM_CB

}  // namespace

void item_capture_load() {
    std::wstring path = captures_path();
    std::ifstream f(path);
    if (!f) {
        logf("item_capture: no existing TSV at %ls", path.c_str());
        return;
    }
    std::string line;
    bool first = true;
    int  n     = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) { first = false; continue; }
        // Tab-split, 11 columns:
        // 0:id 1:atlas 2:x 3:y 4:size 5:width 6:height
        // 7:level 8:iLevel 9:sellPrice 10:rarityId
        std::string fields[11];
        std::size_t start = 0;
        int idx = 0;
        for (std::size_t i = 0; i <= line.size() && idx < 11; ++i) {
            if (i == line.size() || line[i] == '\t') {
                fields[idx++] = line.substr(start, i - start);
                start = i + 1;
            }
        }
        if (fields[0].empty()) continue;
        ItemCapture cap;
        cap.id         = std::move(fields[0]);
        cap.atlas      = std::move(fields[1]);
        cap.gfx_x      = std::atoi(fields[2].c_str());
        cap.gfx_y      = std::atoi(fields[3].c_str());
        cap.gfx_size   = std::atoi(fields[4].c_str());
        cap.gfx_width  = std::atoi(fields[5].c_str());
        cap.gfx_height = std::atoi(fields[6].c_str());
        cap.level      = std::atoi(fields[7].c_str());
        cap.ilvl       = std::atoi(fields[8].c_str());
        cap.sell_price = std::atoi(fields[9].c_str());
        cap.rarity_id  = std::move(fields[10]);
        if (cap.gfx_size <= 0)   cap.gfx_size   = 96;
        if (cap.gfx_width <= 0)  cap.gfx_width  = 1;
        if (cap.gfx_height <= 0) cap.gfx_height = 1;
        std::lock_guard<std::mutex> lk(g_mu);
        g_captures.emplace(cap.id, std::move(cap));
        ++n;
    }
    logf("item_capture: loaded %d existing captures from %ls",
         n, path.c_str());
}

void item_capture_start(const LibHL& libhl) {
    if (g_active.exchange(true)) return;
    g_libhl = libhl;
    g_getp  = reinterpret_cast<HlDynGetP >(libhl.hl_dyn_getp);
    g_geti  = reinterpret_cast<HlDynGetI >(libhl.hl_dyn_geti);
    g_hash  = reinterpret_cast<HlHashUtf8>(libhl.hl_hash_utf8);
    g_hlt_dyn = libhl.hlt_dyn;
    g_hlt_i32 = libhl.hlt_i32;

    g_ready = g_getp && g_geti && g_hash && g_hlt_dyn && g_hlt_i32;
    if (!g_ready) {
        logf("item_capture: libhl exports missing - disabled");
        g_active.store(false);
        return;
    }

    g_h_id        = g_hash("id");
    g_h_gfx       = g_hash("gfx");
    g_h_file      = g_hash("file");
    g_h_x         = g_hash("x");
    g_h_y         = g_hash("y");
    g_h_size      = g_hash("size");
    g_h_width     = g_hash("width");
    g_h_height    = g_hash("height");
    g_h_level     = g_hash("level");
    g_h_iLevel    = g_hash("iLevel");
    g_h_sellPrice = g_hash("sellPrice");
    g_h_rarityId  = g_hash("rarityId");

    hl_hook_register(L"st.Item",                    on_stitem_alloc);
    hl_hook_register(L"st.item.Gear",                on_stgear_alloc);
    hl_hook_register(L"st.item.Weapon",              on_stweapon_alloc);
    hl_hook_register(L"st.item.Armor",               on_starmor_alloc);
    hl_hook_register(L"st.item.Mastery",             on_stmastery_alloc);
    hl_hook_register(L"st.item.Recipe",              on_strecipe_alloc);
    hl_hook_register(L"ui.comp.ItemSlot",            on_uislot_alloc);
    hl_hook_register(L"ui.comp.ItemButton",          on_uibutton_alloc);
    hl_hook_register(L"ui.comp.ItemListerButton",    on_uilister_alloc);
    hl_hook_register(L"ui.comp.WeaponListerButton",  on_uiweaponlist_alloc);
    hl_hook_register(L"ent.interactible.LootDrop",   on_lootdrop_alloc);
    logf("item_capture: 11 watchers registered (item subclasses + "
         "inventory UI + loot drops)");
}

void item_capture_stop() {
    g_active.store(false);
}

void item_capture_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    if (!g_ready) return;

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
            if (taken < kMaxItemsPerTick && it->retries >= kSettleFrames) {
                work.push_back(*it);
                it = g_pending.erase(it);
                ++taken;
                continue;
            }
            ++it;
        }
    }
    for (const Pending& p : work) {
        if (!mem_is_userland(p.obj)) continue;
        try_walk(p.obj, p.inf_offset, p.expected_class);
    }
}

const ItemCapture* item_capture_find(const char* item_id) {
    if (!item_id || !*item_id) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_captures.find(item_id);
    if (it == g_captures.end()) return nullptr;
    return &it->second;
}

std::size_t item_capture_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_captures.size();
}

}  // namespace farever
