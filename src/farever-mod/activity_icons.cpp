// #107 - see activity_icons.h for what this is and why.
//
// Threading, which is the whole difficulty here:
//
//   * activity objects are handed to us by the hl_alloc_obj watcher the
//     instant they are allocated, before the game has written a single field.
//     Reading `inf` there gives zero. So the watcher only records the pointer.
//   * Resolving `inf.gfx` needs hl_dyn_getp, which must not run on the
//     hl_pump worker (see the comment above skill_resolve_query). The only
//     safe context is inside an alloc-watcher callback, on whichever thread
//     the game called hl_alloc_obj from.
//
// Hence: record on alloc, resolve a moment later from another alloc callback.
// The drain is hung off ent.Foe / ent.Hero as well as the activity classes,
// because a zone with five activities in it would otherwise never produce a
// sixth allocation to drain the fifth.

#include "activity_icons.h"
#include "hl_hook.h"
#include "log.h"
#include "mem_scan.h"
#include "skill_resolve.h"
#include "user_data.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace farever {
namespace {

constexpr std::size_t OFF_ACTIVITY_INF = 480;   // st.Activity.inf [v023]

// st.Activity itself is never instantiated: the world holds one concrete
// subclass per activity type, and hl_hook matches class names exactly, so a
// watcher on the base class would never fire. All of them keep `inf` at the
// same offset (checked against tools/classes_v023.json), so one callback
// covers the lot. The names happen to match the CDB `inherit` values we key
// the cache by, but we still read `inherit` off the record rather than
// deriving it from the class name - the CDB link is the authoritative one.
const wchar_t* const kActivityClasses[] = {
    L"st.activity.WorldElite",      L"st.activity.FightStone",
    L"st.activity.ChestOrb",        L"st.activity.Ascension",
    L"st.activity.MountRush",       L"st.activity.TimerCollectRun",
    L"st.activity.WorldCamp",       L"st.activity.WorldPlant",
};

// The atlas the activity markers live in. An activity whose gfx points
// somewhere else is not something pois.cpp can draw with its atlas, so we
// ignore it rather than caching a cell index into the wrong image.
constexpr const char* kActivityAtlas = "activities.png";
// pois.cpp slices that atlas as a grid of 128 px cells. If the game ever
// re-cuts it, the cell indices stop meaning what we think they mean, so say so
// in the log and keep the built-in table rather than drawing nonsense.
constexpr int kActivityCellPx = 128;

// A freshly allocated activity needs a moment before its fields are written.
// The other readers in the mod use a retry count on a 40 Hz tick; here the
// drain is alloc-driven and irregular, so it is a wall-clock delay instead.
constexpr std::uint64_t kSettleMs      = 750;
constexpr std::uint64_t kDrainGapMs    = 250;   // min gap between drains
constexpr std::size_t   kPendingCap    = 64;
constexpr int           kDrainPerPump  = 4;
constexpr int           kMaxAttempts   = 4;

struct Pending {
    std::uintptr_t ptr;
    std::uint64_t  seen_ms;
    int            attempts;
};

std::mutex                                        g_mu;
std::deque<Pending>                               g_pending;
std::unordered_map<std::string, std::pair<int,int>> g_cells;
bool                                              g_dirty  = false;
std::atomic<bool>                                 g_loaded{false};

std::wstring cache_path() { return user_data_path(L"activity_icons.txt"); }

void load_cells() {
    std::ifstream f(cache_path());
    if (!f) return;
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        // "<subkind> <col> <row>"; anything malformed is skipped, not fatal.
        char sub[96] = {};
        int  col = -1, row = -1;
        if (std::sscanf(line.c_str(), "%95s %d %d", sub, &col, &row) != 3)
            continue;
        if (!sub[0] || col < 0 || row < 0) continue;
        // Same rule as resolve_one: (0,0) is the unset CDB default. Filter it
        // here too, so a cache written before that was understood does not keep
        // handing back a skull for every subkind whose record has no gfx.
        if (col == 0 && row == 0) continue;
        std::lock_guard<std::mutex> lk(g_mu);
        g_cells[sub] = {col, row};
        ++n;
    }
    if (n) logf("activity_icons: loaded %d learned cell(s)", n);
}

void note_cell(const char* subkind, int col, int row) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cells.find(subkind);
    if (it != g_cells.end() && it->second.first == col &&
        it->second.second == row)
        return;                       // already known and unchanged
    g_cells[subkind] = {col, row};
    g_dirty = true;
    logf("activity_icons: '%s' -> atlas cell (%d,%d)", subkind, col, row);
}

// Resolve one activity. Returns false while the object still looks unwritten,
// so the caller can retry it.
bool resolve_one(std::uintptr_t obj) {
    char sub[96] = {};
    if (!cdb_inf_string(obj, OFF_ACTIVITY_INF, "inherit", sub, sizeof(sub)) ||
        !sub[0])
        return false;

    SkillGfx g{};
    if (!cdb_gfx_at(obj, OFF_ACTIVITY_INF, &g)) return false;
    if (std::strcmp(g.atlas_filename, kActivityAtlas) != 0) {
        // Not the marker atlas - nothing pois.cpp can use. Count it as
        // resolved so we stop retrying it.
        return true;
    }
    // `size` is the coordinate STEP, not the cell edge: `width`/`height` say
    // how many steps the artwork spans. Measured 2026-08-06 on the weapon
    // atlas, which reports sz=48 wh=(2,2) for 96 px icons. So the edge is
    // size * width, and comparing `size` alone against 128 would silently drop
    // any activity authored as, say, 64 x 2.
    const int edge_w = g.size * (g.width  > 0 ? g.width  : 1);
    const int edge_h = g.size * (g.height > 0 ? g.height : 1);
    if (edge_w != kActivityCellPx || edge_h != kActivityCellPx) {
        static std::atomic<bool> s_warned{false};
        if (!s_warned.exchange(true))
            logf("activity_icons: '%s' spans %dx%d px (size=%d wh=%dx%d), "
                 "expected %d - keeping the built-in icon table", sub,
                 edge_w, edge_h, g.size, g.width, g.height, kActivityCellPx);
        return true;
    }
    // The cell index is in STEP units; pois.cpp slices a fixed 128 px grid, so
    // convert while the step is still known.
    g.x = g.x / (g.width  > 0 ? g.width  : 1);
    g.y = g.y / (g.height > 0 ? g.height : 1);
    if (g.x < 0 || g.y < 0) return true;
    // Cell (0,0) is the CDB default, not an answer. Measured 2026-08-06: of the
    // six subkinds seen live, WorldElite AND WorldPlant both reported (0,0),
    // and (0,0) is the skull, which is right for the elite and nonsense for a
    // plant. A record without its own gfx override simply reads zero, and we
    // cannot tell that apart from a deliberate (0,0). Caching it would have
    // replaced the built-in WorldPlant icon with a skull, so the built-in table
    // keeps that one. The cost is that a subkind whose real cell IS (0,0)
    // never gets learned, which is free: the table already answers those.
    if (g.x == 0 && g.y == 0) {
        static std::atomic<bool> s_noted{false};
        if (!s_noted.exchange(true))
            logf("activity_icons: '%s' reports cell (0,0), treating it as an "
                 "unset gfx record and keeping the built-in icon", sub);
        return true;
    }
    note_cell(sub, g.x, g.y);
    return true;
}

void drain() {
    static std::atomic<std::uint64_t> s_last{0};
    std::uint64_t now  = GetTickCount64();
    std::uint64_t last = s_last.load(std::memory_order_relaxed);
    if (now - last < kDrainGapMs) return;
    if (!s_last.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;   // another alloc claimed this window

    for (int i = 0; i < kDrainPerPump; ++i) {
        Pending p{};
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_pending.empty()) return;
            if (now - g_pending.front().seen_ms < kSettleMs) return;  // FIFO: not ripe yet
            p = g_pending.front();
            g_pending.pop_front();
        }
        if (resolve_one(p.ptr)) continue;
        if (++p.attempts >= kMaxAttempts) continue;   // give up on this one
        p.seen_ms = now;
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending.push_back(p);
    }
}

void on_activity_alloc(std::uintptr_t obj) {
    if (!obj) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_pending.size() >= kPendingCap) g_pending.pop_front();
        g_pending.push_back({obj, GetTickCount64(), 0});
    }
    drain();
}

// Activities are allocated in bursts when a zone streams in and then not at
// all, so their own allocations cannot drain the tail of the queue. Foes and
// heroes keep arriving as the player moves, which gives a steady alloc
// context. Same trick skill_resolve uses for the icon queue (#104).
void on_entity_alloc(std::uintptr_t /*obj*/) { drain(); }

}  // namespace

void activity_icons_init(const LibHL& /*libhl*/) {
    if (g_loaded.exchange(true)) return;
    load_cells();
    for (const wchar_t* cls : kActivityClasses)
        hl_hook_register(cls, on_activity_alloc);
    hl_hook_register(L"ent.Foe",  on_entity_alloc);
    hl_hook_register(L"ent.Hero", on_entity_alloc);
    logf("activity_icons: watching %zu activity classes (#107) - map icons "
         "come from the game's own CDB records",
         sizeof(kActivityClasses) / sizeof(kActivityClasses[0]));
}

bool activity_icon_cell(const char* subkind, int* col, int* row) {
    if (!subkind || !*subkind || !col || !row) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cells.find(subkind);
    if (it == g_cells.end()) return false;
    *col = it->second.first;
    *row = it->second.second;
    return true;
}

void activity_icons_flush() {
    std::unordered_map<std::string, std::pair<int,int>> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_dirty) return;
        snapshot = g_cells;
        g_dirty  = false;
    }
    std::wstring final_path = cache_path();
    std::wstring tmp_path   = final_path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            logf("activity_icons: cannot write %ls", tmp_path.c_str());
            std::lock_guard<std::mutex> lk(g_mu);
            g_dirty = true;      // try again next time
            return;
        }
        for (const auto& kv : snapshot)
            f << kv.first << ' ' << kv.second.first << ' '
              << kv.second.second << '\n';
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        logf("activity_icons: rename failed (GLE=%lu)",
             (unsigned long)GetLastError());
        DeleteFileW(tmp_path.c_str());
        std::lock_guard<std::mutex> lk(g_mu);
        g_dirty = true;
        return;
    }
    logf("activity_icons: persisted %zu cell(s)", snapshot.size());
}

}  // namespace farever
