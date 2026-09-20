// ImGui-DX12 overlay for the unified mod. The DX12 init / fence /
// WndProc machinery follows the same pattern as dpsmeter-dll's
// overlay.cpp - see those comments for the deep rationale. The
// farever-mod-specific bits are at the bottom (render_imgui_window).

#include "overlay.h"
#include "anticheat.h"
#include "log.h"
#include "aggregator.h"
#include "damage.h"
#include "hero_state.h"
#include "textures.h"
#include "pois.h"
#include "poi_progress.h"
#include "waypoints.h"
#include "party_state.h"
#include "render_mode.h"
#include "camera_state.h"
#include "compass_heading.h"
#include "boss_timer.h"
#include "boss_names.h"
#include "mob_watch.h"
#include "skill_resolve.h"
#include "entity_state.h"
#include "plugins.h"
#include "user_data.h"

#include <unordered_map>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace farever {
namespace {

constexpr UINT kMaxBackBuffers = 8;

struct FrameContext {
    ID3D12CommandAllocator*     allocator    = nullptr;
    ID3D12GraphicsCommandList*  command_list = nullptr;
    ID3D12Resource*             back_buffer  = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    UINT64                      fence_value  = 0;
};

struct Overlay {
    bool initialized = false;
    bool init_failed = false;

    IDXGISwapChain3*            owned_swap_chain = nullptr;
    ID3D12Device*               device           = nullptr;
    // v0.4.16: own DIRECT command queue. Replaces the game's
    // captured queue (which required hooking ExecuteCommandLists, the
    // highest-frequency D3D12 vtable hook and the most likely AV
    // trigger per the no_d3d12 bisection). We create our own queue
    // at init time and submit overlay command lists on it. The game's
    // own swapchain Present implicitly synchronises with our writes.
    ID3D12CommandQueue*         queue            = nullptr;
    ID3D12DescriptorHeap*       rtv_heap         = nullptr;
    ID3D12DescriptorHeap*       srv_heap         = nullptr;
    UINT                        rtv_descriptor_size = 0;
    UINT                        back_buffer_count   = 0;
    std::vector<FrameContext>   frames;

    ID3D12Fence*                fence            = nullptr;
    UINT64                      next_fence_value = 0;
    HANDLE                      fence_event      = nullptr;

    HWND                        hwnd         = nullptr;
    WNDPROC                     orig_wndproc = nullptr;
    DXGI_FORMAT                 rt_format    = DXGI_FORMAT_R8G8B8A8_UNORM;

    // SRV slot 0 = ImGui's font atlas; 1=mosaic, 2=POI atlas,
    // 3=player arrow. Slots 5..63 are dynamically allocated to the
    // weapon / class skill atlases as the DPS meter encounters new
    // skills (see g_atlas_cache).
    LoadedTexture               mosaic{};
    LoadedTexture               poi_atlas{};
    LoadedTexture               player_arrow{};
};

Overlay           g_overlay;
std::atomic<bool> g_in_render{false};

// Skill-atlas cache: every skill class refers to a specific weapon /
// class atlas (e.g. atlas_class_Mage_96PX.png). We lazy-load each one
// to a fresh SRV slot on first reference. Slots 0-3 are reserved
// (font, mosaic, POI atlas, player arrow); slot 4 is free; 5+ go to
// dynamic atlases.
constexpr UINT kSkillAtlasSlotBase = 5;
constexpr UINT kSkillAtlasSlotMax  = 64;  // upper bound of srv_heap
UINT g_next_atlas_slot = kSkillAtlasSlotBase;
std::unordered_map<std::string, LoadedTexture> g_atlas_cache;

// Per-window visibility toggles (user keys). The overall panic switch
// (auto-disable on GPU stalls) is kept as a separate fail-safe via
// g_overlay_enabled - if a wedged queue forces us off, the user can't
// re-enable just one window because the whole submission path is
// short-circuited.
std::atomic<bool> g_overlay_enabled{true};  // panic / auto-disable
std::atomic<bool> g_dps_visible{true};      // F10
std::atomic<bool> g_minimap_visible{true};  // F8
constexpr int kFenceTimeoutMs        = 50;
constexpr int kAutoDisableSlowFrames = 30;
int g_consecutive_slow_frames = 0;

// WoW-bezel palette.
constexpr ImU32 kColBezel       = IM_COL32(212, 175,  55, 240);
constexpr ImU32 kColBezelShadow = IM_COL32(  0,   0,   0, 160);
constexpr ImU32 kColBtnFill     = IM_COL32( 32,  24,  12, 235);
constexpr ImU32 kColBtnHover    = IM_COL32( 70,  52,  24, 245);
constexpr ImU32 kColBtnActive   = IM_COL32( 18,  12,   6, 255);
constexpr ImU32 kColIcon        = IM_COL32(255, 220, 130, 255);
constexpr ImU32 kColNorth       = IM_COL32(255,  80,  80, 230);
constexpr ImU32 kColPlayer      = IM_COL32(255, 200,  50, 255);
constexpr ImU32 kColText        = IM_COL32(255, 230, 180, 255);

// --- minimap calibration --------------------------------------------
//
// Knobs derived analytically from the mosaic geometry + the engine's
// 256 m/tile constant: px_per_meter = 1024 / 256 = 4, etc. See
// minimap-dll/overlay.cpp for the full derivation. Hot-reloadable via
// data/minimap_calibration.json so the user can adjust without
// rebuilding.
struct Calibration {
    float world_to_full_x_scale  = 4.0f;
    float world_to_full_y_scale  = 4.0f;
    float world_to_full_x_offset = 4096.0f;
    float world_to_full_y_offset = 6144.0f;
    bool  flip_y                 = true;
    float zoom                   = 12.0f;   // 1.0 = whole mosaic visible
};
Calibration g_calib;
constexpr float kFullMosaicPx = 11264.0f;
// #83: floor lowered from 10 so the minus button (and the overview toggle)
// can pull back far enough to show a whole zone at once, for hunting the
// last chest/orb. kZoomOverview is where the "show full zone" button jumps.
constexpr float kZoomMin      = 3.0f;
constexpr float kZoomMax      = 20.0f;
constexpr float kZoomStep     = 1.0f;
constexpr float kZoomOverview = 3.0f;

constexpr float kCompassSizes[3] = { 256.0f, 384.0f, 512.0f };
int g_compass_size_idx = 2;   // start largest

struct PoiFilter {
    bool obelisks   = true;
    bool respawns   = true;
    bool merchants  = true;
    bool dungeons   = true;
    bool activities = true;
    // Collectibles + farming nodes. Off by default - the chest button
    // on the bezel toggles all four at once for quick on/off, and
    // each one can be turned on individually in the filter panel.
    bool chests     = false;
    bool red_orbs   = false;
    bool plants     = false;
    bool ores       = false;
};
PoiFilter g_filter;
bool g_filter_open       = false;
// #254: draw a rim arrow on the minimap pointing at the nearest not-yet-
// collected collectible (of the enabled categories). Helps hunt the last
// chest/orb of an area. Render-thread only, persisted in ui_state.json.
bool g_guide_nearest     = true;
bool g_keys_open         = false;
bool g_compass_collapsed = false;
int  g_selected_fight_id = 0;   // 0 = no fight detail open
std::atomic<bool> g_ui_locked{false};   // minimap lock: pin + size
std::atomic<bool> g_dps_locked{false};  // DPS/fight-history window lock (issue #15: independent)
// #57/#70: meter table view - which breakdown the table shows.
// Persisted to ui_state.json ("meter_view": "dmg"|"heal"|"shield").
enum MeterView { MV_DMG = 0, MV_HEAL = 1, MV_SHIELD = 2 };
std::atomic<int> g_meter_view{MV_DMG};
std::atomic<int> g_fight_detail_view{MV_DMG};   // #57/#70, independent state

// Bezel button layout - angle (rad) around the compass center, one
// entry per button. Right-click + drag in render_compass reassigns
// these; ui_state.json persists them. The default values reproduce
// the pre-customisation layout.
struct BezelLayout {
    float pin      = -3.14159265f * 0.32f;
    float size     = -3.14159265f * 0.20f;
    float collapse = -3.14159265f * 0.50f;
    float filter   =  3.14159265f * 1.20f;
    float lock     =  3.14159265f * 0.84f;
    float keys     =  3.14159265f * 0.68f;
    float chest    =  3.14159265f * 0.50f;
    float plus     =  3.14159265f * 0.20f;
    float minus    =  3.14159265f * 0.32f;
    float overview =  3.14159265f * 0.41f;   // #83 "show full zone" toggle
};
BezelLayout g_bezel;
int         g_bezel_drag = -1;   // 0..9 while a right-drag is in progress

// #83: "show full zone" overview toggle. One click pulls the minimap back to
// a wide zoom (kZoomOverview) so the user can spot the last chest/orb in a
// zone; clicking again restores the zoom they had. Render-thread only and
// transient - the live zoom lives in g_calib.zoom (reloadable calibration),
// so this is not persisted.
bool  g_overview_active     = false;
float g_overview_saved_zoom = 12.0f;

// Right-click waypoint context-menu state (set on right-click, consumed by
// the popup the same frame / following frames). #58
int    g_wp_ctx_id = -1;          // hit waypoint id, or -1 = empty map
double g_wp_ctx_wx = 0.0, g_wp_ctx_wy = 0.0;
float  g_wp_ctx_wz = 0.0f;
int    g_wp_rename_id = -1;        // waypoint being renamed (modal)
char   g_wp_rename_buf[64] = {0};
bool   g_wp_want_rename = false;   // defer OpenPopup to window scope (ID match)

void ui_lock_load();
void ui_lock_save();

float compass_size_px() { return kCompassSizes[g_compass_size_idx]; }

bool poi_passes_filter(const PoiRow& p) {
    // p.cat is precomputed at load (see poi_category), so this is an int
    // switch per frame instead of a strcmp chain over the kind string.
    switch (p.cat) {
        case PoiCat::Obelisk:  return g_filter.obelisks;
        case PoiCat::Respawn:  return g_filter.respawns;
        case PoiCat::Merchant: return g_filter.merchants;
        case PoiCat::Dungeon:  return g_filter.dungeons;
        case PoiCat::Activity: return g_filter.activities;
        case PoiCat::Chest:    return g_filter.chests;
        case PoiCat::RedOrb:   return g_filter.red_orbs;
        case PoiCat::Plant:    return g_filter.plants;
        case PoiCat::Ore:      return g_filter.ores;
        default:               return true;
    }
}

// Resolve a path relative to this DLL (dinput8.dll) so the mod
// works regardless of where the user dropped it (release zips
// land in unpredictable folders).
std::wstring dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&dll_dir),
        &hmod);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(path);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    s.resize(pos);
    return s;
}

std::wstring data_path(const wchar_t* relative) {
    std::wstring s = dll_dir();
    s += L"\\data\\";
    s += relative;
    return s;
}

// Off-screen rescue: if a window's saved position (from ImGui's ini
// file or wherever) puts it outside the current viewport, snap it
// back to a visible spot. Triggered the next time ImGui's
// SetNextWindowPos(..., ImGuiCond_Always) fires below. Issue #1: a
// game crash left the minimap saved at the right edge of the
// screen, the pin handle off-screen, and the user couldn't drag it
// back. The rescue makes that recoverable instead of permanent.
bool window_is_offscreen(ImVec2 pos, ImVec2 size, const ImVec2& vp) {
    // 24 px slack -- the title bar needs to be grabbable even if the
    // window is right at the edge.
    const float slack = 24.0f;
    if (pos.x + size.x < slack)          return true;
    if (pos.y + size.y < slack)          return true;
    if (pos.x > vp.x - slack)            return true;
    if (pos.y > vp.y - slack)            return true;
    return false;
}

// Lazy atlas loader. `filename` is just the basename, e.g.
// "atlas_class_Mage_96PX.png". Looks for the file under data/atlases/
// (mirror of res.pak's UI/icons/ path). Returns nullptr if the file is
// missing or the slot allocator is full.
LoadedTexture* get_or_load_atlas(const char* filename) {
    if (!filename || !*filename) return nullptr;
    std::string key = filename;
    auto it = g_atlas_cache.find(key);
    if (it != g_atlas_cache.end()) {
        return it->second.resource ? &it->second : nullptr;
    }
    if (g_next_atlas_slot >= kSkillAtlasSlotMax) return nullptr;

    std::wstring full = dll_dir();
    full += L"\\data\\atlases\\UI\\icons\\";
    int n = MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, w.data(), n);
    full += w;

    LoadedTexture tex{};
    UINT slot = g_next_atlas_slot;
    if (!load_texture_from_file(g_overlay.device, g_overlay.srv_heap,
                                slot, full.c_str(), &tex)) {
        logf("overlay: atlas %s load failed (slot %u)", filename, slot);
        g_atlas_cache[key] = LoadedTexture{};
        return nullptr;
    }
    g_next_atlas_slot++;
    g_atlas_cache[key] = tex;
    logf("overlay: atlas %s loaded into slot %u (%ux%u)",
         filename, slot, tex.width, tex.height);
    auto& stored = g_atlas_cache[key];
    return stored.resource ? &stored : nullptr;
}

const std::wstring& kCalibPath() {
    static const std::wstring p = data_path(L"minimap_calibration.json");
    return p;
}

bool calib_extract_double(const std::string& json, const char* key,
                          double& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    char* end = nullptr;
    double v = strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

bool calib_extract_bool(const std::string& json, const char* key, bool& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (json.compare(i, 4, "true")  == 0) { out = true;  return true; }
    if (json.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

void calib_maybe_reload() {
    static FILETIME last_write{};
    static bool first_check = true;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(kCalibPath().c_str(), GetFileExInfoStandard, &attr))
        return;
    if (!first_check &&
        attr.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime)
        return;
    last_write  = attr.ftLastWriteTime;
    first_check = false;

    std::ifstream f(kCalibPath());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    Calibration c = g_calib;
    double v;
    if (calib_extract_double(text, "scale_x",  v)) c.world_to_full_x_scale  = (float)v;
    if (calib_extract_double(text, "scale_y",  v)) c.world_to_full_y_scale  = (float)v;
    if (calib_extract_double(text, "offset_x", v)) c.world_to_full_x_offset = (float)v;
    if (calib_extract_double(text, "offset_y", v)) c.world_to_full_y_offset = (float)v;
    // zoom is NOT read here anymore: it is a user-interactive setting now
    // (the +/- and full-zone buttons), persisted in ui_state.json. We keep
    // the current live zoom so a calibration hot-reload does not reset it.
    c.zoom = g_calib.zoom;
    calib_extract_bool(text, "flip_y", c.flip_y);
    g_calib = c;
    logf("overlay: calibration reloaded (zoom=%.1f)", c.zoom);
}

// --- keybinds -------------------------------------------------------
//
// Defaults are the historical F10 (DPS) / F8 (map) / F9 (reset). The
// user can override any of them via data/keybinds.json, accepting
// either a key name ("F11", "M", "INSERT", "NUMPAD0", "OEM_3") or a
// raw Virtual-Key code ("toggle_dps": 121). Hot-reloaded on file
// mtime change, same as the calibration json.
struct Keybinds {
    UINT toggle_dps          = VK_F10;
    UINT toggle_minimap      = VK_F8;
    UINT reset_dps           = VK_F9;
    UINT toggle_clickthru    = VK_F11;   // issues #4 + #7
    UINT toggle_dps_track    = VK_F12;   // issue #13
    UINT toggle_overlay      = VK_F7;    // v0.5 overlay show/hide
    UINT reset_positions     = VK_HOME;  // v0.5.2 issue #11: snap all
                                         // overlay windows back to their
                                         // default positions so a user
                                         // who dragged one off-screen or
                                         // onto a disconnected monitor
                                         // can recover without editing
                                         // imgui.ini or reinstalling.
};
Keybinds g_keybinds;

// Click-through state (issues #4, #7). When true, our wndproc stops
// eating mouse messages even when the cursor is over an ImGui window,
// so attacks / camera rotates aren't intercepted by the overlay.
// Persisted to ui_state.json alongside the lock flag.
std::atomic<bool> g_clickthrough{false};

// DPS tracking pause (issue #13). When true, the DamageDisplay
// alloc-hook callback returns immediately and damage_tick's
// processing loop short-circuits -- the meter's totals freeze and
// the per-allocation overhead is reduced to just the MinHook
// trampoline. Hiding the DPS window via F10 only hides the UI;
// this flag pauses the backend too.
std::atomic<bool> g_dps_tracking_paused{false};

// Minimap mosaic + bezel opacity (issue #2). 0.30..1.00; 1.00 = old
// behaviour. Persisted to ui_state.json.
float g_minimap_alpha = 1.0f;

// v0.5.5: square minimap option. Default false = circular bezel (legacy).
// True swaps the mosaic clip + bezel ring + POI clip from a disc to the
// inscribed square. Bezel buttons keep their ring layout. Persisted.
std::atomic<bool> g_minimap_square{false};

// Heading-up rotation. Default false = north-up (legacy look, untouched
// render path). True rotates the mosaic + POIs so the player's facing
// points up; the player arrow then stays fixed pointing up and the
// north tick rotates to show true north. Persisted.
std::atomic<bool> g_minimap_rotate{false};
// #105: what "up" means while g_minimap_rotate is on. Off (default, the
// behaviour every previous version had) = the player's facing. On = the camera
// yaw, so the map matches what is on screen - the same source the compass strip
// has used since #65. Separate from the compass toggle on purpose: wanting a
// camera-locked compass strip does not mean wanting a camera-locked map.
std::atomic<bool> g_minimap_follow_camera{false};

// Auto-theme community plugins to match our overlay look. Default on; toggled
// in Options, persisted in ui_state.json. A plugin can opt out with a global
// `plugin_theme = false`. Read by plugins' run_render_hooks via
// overlay_plugin_theme_enabled().
std::atomic<bool> g_plugin_theme{true};

// v0.5.2: ImGui window background opacity multiplier (DPS, hotkeys,
// fight detail). 0.30..1.00; 1.00 = unchanged. Persisted alongside
// minimap_alpha. Compass window has its own opacity above, this is
// for the text-content windows.
float g_window_bg_alpha = 1.0f;

// v0.5.3 (issue #22): global UI text scale. Applied to ImGui's
// FontGlobalScale every frame. 0.50..2.50; 1.00 = unchanged. Persisted
// to ui_state.json. For 4K + large display users (EpicTragedy on 48" 4K)
// the default font renders unreadably small; scaling text 1.5x to 2.0x
// makes the DPS table and tooltips legible. Hand-drawn bezel icons keep
// their pixel size by design; this is text-only scaling.
float g_ui_scale = 1.0f;

// v0.5.2.2 (issue #23): per-frame "cursor is over the minimap window"
// cache, set inside render_minimap_window after ImGui::Begin. Consumed
// by the wndproc's RMB auto-clickthrough logic so RMB-on-minimap (POI
// right-click toggle, bezel reposition drag) keeps reaching ImGui
// instead of being auto-forwarded to the game as a camera click.
// Reset to false at the top of overlay_render so a hidden minimap
// (F8 off) does not leave the last hovered value stuck.
std::atomic<bool> g_overlay_minimap_hovered{false};

// v0.5.2.2 follow-up (issue #23): standalone "Loot tracker" window
// that shows chests + red orbs collected counts at a glance. The
// previous under-compass text strip got lost visually; this is its
// own draggable, hideable widget. Default visible, persisted to
// ui_state.json, togglable via the filter tablet checkbox.
std::atomic<bool> g_loot_counter_visible{true};

// Skyrim-style compass strip window (separate from the minimap).
// Default off (opt-in). Persisted in ui_state.json.
std::atomic<bool>  g_compass_visible{false};
std::atomic<float> g_compass_radius_m{150.0f};
std::atomic<bool>  g_compass_show_cardinals{true};
// #65: when true (default), the compass strip recenters on the game camera
// yaw; when false it uses the hero facing (the pre-#65 behaviour).
std::atomic<bool>  g_compass_follow_camera{true};
// Boss speedrun mode (opt-in). Mirrors boss_timer's enabled gate so the choice
// persists; overlay drives boss_timer_set_enabled on load + toggle.
std::atomic<bool>  g_boss_speedrun_enabled{false};

// Party member markers on minimap + compass. Default ON (opt-out);
// persisted in ui_state.json as "party_visible".
std::atomic<bool>  g_party_visible{true};

constexpr std::size_t kMaxCompassPluginMarkers = 128;
struct CompassPluginMarker {
    int     handle;
    float   x, y, z;
    char    name[64];
    uint8_t color;
    uint8_t icon;
    double  expire_at;   // 0 = never; otherwise ImGui::GetTime() based
};
std::vector<CompassPluginMarker> g_compass_plugin_markers;
int g_compass_plugin_marker_next_handle = 1;

}  // namespace

// Public accessors for the Lua plugin API.
bool  compass_is_visible()    { return g_compass_visible.load(); }
float compass_radius_m()      { return g_compass_radius_m.load(); }
bool  compass_cardinals_on()  { return g_compass_show_cardinals.load(); }

int compass_add_marker(float x, float y, float z, const char* name,
                       unsigned char color, unsigned char icon,
                       double ttl_sec) {
    if (g_compass_plugin_markers.size() >= kMaxCompassPluginMarkers) return 0;
    CompassPluginMarker m{};
    m.handle = g_compass_plugin_marker_next_handle++;
    m.x = x; m.y = y; m.z = z;
    const char* nm = (name && *name) ? name : "Marker";
    std::strncpy(m.name, nm, sizeof(m.name) - 1);
    m.name[sizeof(m.name) - 1] = '\0';
    m.color = color < 8 ? color : 0;
    m.icon  = icon  < 6 ? icon  : 0;
    m.expire_at = ttl_sec > 0.0 ? (ImGui::GetTime() + ttl_sec) : 0.0;
    g_compass_plugin_markers.push_back(m);
    return m.handle;
}

bool compass_remove_marker(int handle) {
    auto it = std::find_if(g_compass_plugin_markers.begin(),
                           g_compass_plugin_markers.end(),
                           [handle](const CompassPluginMarker& m) {
                               return m.handle == handle;
                           });
    if (it == g_compass_plugin_markers.end()) return false;
    g_compass_plugin_markers.erase(it);
    return true;
}

namespace {

// Apply g_window_bg_alpha to the cached kColBtnFill (alpha 235/255).
// Returns the same RGB with scaled alpha.
inline ImU32 window_bg_color() {
    int a = (int)(235.0f * g_window_bg_alpha + 0.5f);
    if (a < 0) a = 0; if (a > 255) a = 255;
    return IM_COL32(32, 24, 12, a);
}

// 0.4.12 (issue #12): when true, the minimap renders only the bezel
// ring + player arrow + buttons -- no mosaic image, no POI markers.
// Set by overlay_render for the kSkeletonFrames following the post-
// transition pause, then cleared. Lets us coast past the danger
// window with a near-zero ImGui draw count.
std::atomic<bool> g_skeleton_minimap{false};

// v0.5.2 issue #11: set by the reset-positions hotkey, consumed by each
// of the four user-movable windows (minimap, DPS meter, fight detail,
// hotkeys) on the very next frame to override their stored ImGui
// position with the in-built default. overlay_render clears it after
// the frame so the flag is single-shot.
std::atomic<bool> g_reset_positions_request{false};

// In-game rebind: the user clicks a slot in the keys panel, we set
// this to the slot id, and the next non-modifier key press in
// overlay_wndproc gets written into that slot (ESC cancels).
enum class RebindSlot : int { None = 0, Dps = 1, Map = 2, Reset = 3,
                              ClickThru = 4, DpsTrack = 5,
                              Overlay = 6, ResetPos = 7 };
std::atomic<int> g_rebind_listening{0};

UINT key_from_name(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (s.rfind("VK_", 0) == 0) s = s.substr(3);
    if (s.empty()) return 0;

    if (s.size() == 1) {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    if (s.size() >= 2 && s[0] == 'F') {
        char* end = nullptr;
        long n = strtol(s.c_str() + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 24) {
            return VK_F1 + (UINT)(n - 1);
        }
    }
    if (s.rfind("NUMPAD", 0) == 0 && s.size() == 7) {
        char d = s[6];
        if (d >= '0' && d <= '9') return VK_NUMPAD0 + (UINT)(d - '0');
    }
    struct { const char* name; UINT vk; } table[] = {
        {"HOME", VK_HOME}, {"END", VK_END},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
        {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"SPACE", VK_SPACE},
        {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
        {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE},
        {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK},
        {"CAPSLOCK", VK_CAPITAL}, {"PAUSE", VK_PAUSE},
        {"SCROLLLOCK", VK_SCROLL}, {"NUMLOCK", VK_NUMLOCK},
        {"PRINTSCREEN", VK_SNAPSHOT}, {"PRINT", VK_PRINT},
        {"OEM_PLUS", VK_OEM_PLUS}, {"OEM_MINUS", VK_OEM_MINUS},
        {"OEM_COMMA", VK_OEM_COMMA}, {"OEM_PERIOD", VK_OEM_PERIOD},
        {"OEM_1", VK_OEM_1}, {"OEM_2", VK_OEM_2}, {"OEM_3", VK_OEM_3},
        {"OEM_4", VK_OEM_4}, {"OEM_5", VK_OEM_5}, {"OEM_6", VK_OEM_6},
        {"OEM_7", VK_OEM_7}, {"OEM_8", VK_OEM_8},
        {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT},
        {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
        {"DECIMAL", VK_DECIMAL},
    };
    for (auto& e : table) if (s == e.name) return e.vk;
    return 0;
}

std::string key_to_name(UINT vk) {
    char buf[16];
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%u", (unsigned)(vk - VK_F1 + 1));
        return buf;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof(buf), "Numpad%u", (unsigned)(vk - VK_NUMPAD0));
        return buf;
    }
    switch (vk) {
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_PRIOR:    return "PageUp";
        case VK_NEXT:     return "PageDown";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_SPACE:    return "Space";
        case VK_RETURN:   return "Enter";
        case VK_TAB:      return "Tab";
        case VK_ESCAPE:   return "Esc";
        case VK_BACK:     return "Backspace";
    }
    snprintf(buf, sizeof(buf), "VK_%u", (unsigned)vk);
    return buf;
}

bool keybinds_extract_key(const std::string& json, const char* key, UINT& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() &&
           (json[i] == ' ' || json[i] == '\t' ||
            json[i] == '\r' || json[i] == '\n')) ++i;
    if (i >= json.size()) return false;
    if (json[i] == '"') {
        ++i;
        std::string val;
        while (i < json.size() && json[i] != '"') { val += json[i]; ++i; }
        UINT vk = key_from_name(val);
        if (vk == 0) {
            logf("keybinds: unknown key name \"%s\" for %s",
                 val.c_str(), key);
            return false;
        }
        out = vk;
        return true;
    }
    char* end = nullptr;
    long v = strtol(json.c_str() + i, &end, 0);
    if (end == json.c_str() + i) return false;
    if (v <= 0 || v > 254) return false;
    out = (UINT)v;
    return true;
}

const std::wstring& kKeybindsPath() {
    static const std::wstring p = user_data_path(L"keybinds.json");
    return p;
}

void keybinds_maybe_reload() {
    static FILETIME last_write{};
    static bool logged_missing = false;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(kKeybindsPath().c_str(),
                              GetFileExInfoStandard, &attr)) {
        if (!logged_missing) {
            logf("overlay: no keybinds.json, using defaults "
                 "(dps=F10 map=F8 reset=F9)");
            logged_missing = true;
        }
        last_write = FILETIME{};
        return;
    }
    logged_missing = false;
    if (attr.ftLastWriteTime.dwLowDateTime  == last_write.dwLowDateTime &&
        attr.ftLastWriteTime.dwHighDateTime == last_write.dwHighDateTime)
        return;
    last_write = attr.ftLastWriteTime;

    std::ifstream f(kKeybindsPath());
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    Keybinds kb = g_keybinds;
    keybinds_extract_key(text, "toggle_dps",         kb.toggle_dps);
    keybinds_extract_key(text, "toggle_minimap",     kb.toggle_minimap);
    keybinds_extract_key(text, "reset_dps",          kb.reset_dps);
    keybinds_extract_key(text, "toggle_clickthru",   kb.toggle_clickthru);
    keybinds_extract_key(text, "toggle_dps_track",   kb.toggle_dps_track);
    keybinds_extract_key(text, "toggle_overlay",     kb.toggle_overlay);
    keybinds_extract_key(text, "reset_positions",    kb.reset_positions);
    g_keybinds = kb;
    std::string dps_n         = key_to_name(kb.toggle_dps);
    std::string map_n         = key_to_name(kb.toggle_minimap);
    std::string reset_n       = key_to_name(kb.reset_dps);
    std::string clickthru_n   = key_to_name(kb.toggle_clickthru);
    std::string dps_track_n   = key_to_name(kb.toggle_dps_track);
    std::string overlay_n     = key_to_name(kb.toggle_overlay);
    std::string reset_pos_n   = key_to_name(kb.reset_positions);
    logf("overlay: keybinds reloaded (overlay=%s dps=%s map=%s reset=%s "
         "clickthru=%s dps_track=%s reset_positions=%s)",
         overlay_n.c_str(),
         dps_n.c_str(),
         map_n.c_str(),
         reset_n.c_str(),
         clickthru_n.c_str(),
         dps_track_n.c_str(),
         reset_pos_n.c_str());
}

// #65: per-character UI settings. The active profile key (the sanitized
// character name) selects ui_state__<key>.json; empty selects the shared
// ui_state.json. The profile is switched from hl_pump when the locked
// character changes, at the same point the POI / boss-timer profiles switch.
// A character's file is seeded from the shared ui_state.json the first time,
// so it inherits the user's current setup, then diverges independently.
std::mutex        g_profile_mu;
std::wstring      g_profile_key;            // empty -> shared ui_state.json
std::atomic<bool> g_profile_reload{false};  // render thread reloads on set

std::wstring kUiStatePath() {
    std::lock_guard<std::mutex> lk(g_profile_mu);
    if (g_profile_key.empty()) return user_data_path(L"ui_state.json");
    return user_data_path((L"ui_state__" + g_profile_key + L".json").c_str());
}

// Pull a numeric value off something like "name":  -1.234
bool ui_state_extract_float(const std::string& json, const char* key,
                            float& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    char* end = nullptr;
    float v = (float)std::strtod(json.c_str() + i, &end);
    if (end == json.c_str() + i) return false;
    out = v;
    return true;
}

// Pull a JSON bool off "name": true / "name":false. Leaves `out`
// untouched (keeps the caller's default) when the key is absent, so it
// works for flags whose default is true as well as false.
bool ui_state_extract_bool(const std::string& json, const char* key,
                           bool& out) {
    std::string needle = "\""; needle += key; needle += "\":";
    auto i = json.find(needle);
    if (i == std::string::npos) return false;
    i += needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (json.compare(i, 4, "true")  == 0) { out = true;  return true; }
    if (json.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

void ui_lock_load() {
    std::ifstream f(kUiStatePath());
    if (!f) return;
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    g_ui_locked.store(s.find("\"locked\": true") != std::string::npos ||
                      s.find("\"locked\":true")  != std::string::npos);
    g_dps_locked.store(s.find("\"dps_locked\": true") != std::string::npos ||
                       s.find("\"dps_locked\":true")  != std::string::npos);

    // Bezel angles - each one optional; defaults stay if absent.
    ui_state_extract_float(s, "pin",      g_bezel.pin);
    ui_state_extract_float(s, "size",     g_bezel.size);
    ui_state_extract_float(s, "collapse", g_bezel.collapse);
    ui_state_extract_float(s, "filter",   g_bezel.filter);
    ui_state_extract_float(s, "lock",     g_bezel.lock);
    ui_state_extract_float(s, "keys",     g_bezel.keys);
    ui_state_extract_float(s, "chest",    g_bezel.chest);
    ui_state_extract_float(s, "plus",     g_bezel.plus);
    ui_state_extract_float(s, "minus",    g_bezel.minus);
    ui_state_extract_float(s, "overview", g_bezel.overview);

    // #256: minimap zoom, persisted across restarts. Clamp into the
    // interactive range so a stale/hand-edited value can't break the view.
    float zoom_f = g_calib.zoom;
    if (ui_state_extract_float(s, "zoom", zoom_f)) {
        if (zoom_f < kZoomMin) zoom_f = kZoomMin;
        if (zoom_f > kZoomMax) zoom_f = kZoomMax;
        g_calib.zoom = zoom_f;
    }

    // Compass size (0=small, 1=medium, 2=large). Persisted so the
    // minimap doesn't snap back to "large" every launch (issue #6).
    float size_idx_f = (float)g_compass_size_idx;
    ui_state_extract_float(s, "compass_size", size_idx_f);
    int v = (int)(size_idx_f + 0.5f);
    if (v < 0) v = 0; else if (v > 2) v = 2;
    g_compass_size_idx = v;

    // Click-through (issue #4 / #7) and minimap alpha (issue #2).
    g_clickthrough.store(
        s.find("\"clickthrough\": true") != std::string::npos ||
        s.find("\"clickthrough\":true")  != std::string::npos);
    g_dps_tracking_paused.store(
        s.find("\"dps_tracking_paused\": true") != std::string::npos ||
        s.find("\"dps_tracking_paused\":true")  != std::string::npos);
    float a = g_minimap_alpha;
    if (ui_state_extract_float(s, "minimap_alpha", a)) {
        if (a < 0.30f) a = 0.30f;
        if (a > 1.00f) a = 1.00f;
        g_minimap_alpha = a;
    }
    float wba = g_window_bg_alpha;
    if (ui_state_extract_float(s, "window_bg_alpha", wba)) {
        if (wba < 0.30f) wba = 0.30f;
        if (wba > 1.00f) wba = 1.00f;
        g_window_bg_alpha = wba;
    }
    float uis = g_ui_scale;
    if (ui_state_extract_float(s, "ui_scale", uis)) {
        if (uis < 0.50f) uis = 0.50f;
        if (uis > 2.50f) uis = 2.50f;
        g_ui_scale = uis;
    }
    g_loot_counter_visible.store(
        s.find("\"loot_counter_visible\": false") == std::string::npos &&
        s.find("\"loot_counter_visible\":false")  == std::string::npos);

    g_compass_visible.store(
        s.find("\"compass_visible\": true") != std::string::npos ||
        s.find("\"compass_visible\":true")  != std::string::npos);

    g_compass_show_cardinals.store(
        s.find("\"compass_show_cardinals\": false") == std::string::npos &&
        s.find("\"compass_show_cardinals\":false")  == std::string::npos);
    // #65: default true - camera-relative unless explicitly saved false.
    g_compass_follow_camera.store(
        s.find("\"compass_follow_camera\": false") == std::string::npos &&
        s.find("\"compass_follow_camera\":false")  == std::string::npos);
    // Boss speedrun: default OFF, only on when explicitly saved true.
    g_boss_speedrun_enabled.store(
        s.find("\"boss_speedrun_enabled\": true") != std::string::npos ||
        s.find("\"boss_speedrun_enabled\":true")  != std::string::npos);
    boss_timer_set_enabled(g_boss_speedrun_enabled.load());

    g_party_visible.store(
        s.find("\"party_visible\": false") == std::string::npos &&
        s.find("\"party_visible\":false")  == std::string::npos);

    {
        std::size_t k = s.find("\"compass_radius_m\":");
        if (k != std::string::npos) {
            const char* p = s.c_str() + k + std::strlen("\"compass_radius_m\":");
            float v = std::strtof(p, nullptr);
            if (v >= 0.0f && v <= 2000.0f)
                g_compass_radius_m.store(v);
        }
    }

    g_minimap_square.store(
        s.find("\"minimap_square\": true") != std::string::npos ||
        s.find("\"minimap_square\":true")  != std::string::npos);

    // #47: persist the F10/F8 show-hide state and the POI filter
    // checkboxes so they survive a restart. All default to their
    // compiled-in value when the key is absent (clean-install / older
    // ui_state.json).
    bool dps_vis = g_dps_visible.load();
    if (ui_state_extract_bool(s, "dps_visible", dps_vis))
        g_dps_visible.store(dps_vis);
    bool mm_vis = g_minimap_visible.load();
    if (ui_state_extract_bool(s, "minimap_visible", mm_vis))
        g_minimap_visible.store(mm_vis);

    ui_state_extract_bool(s, "filter_obelisks",   g_filter.obelisks);
    ui_state_extract_bool(s, "filter_respawns",   g_filter.respawns);
    ui_state_extract_bool(s, "filter_merchants",  g_filter.merchants);
    ui_state_extract_bool(s, "filter_dungeons",   g_filter.dungeons);
    ui_state_extract_bool(s, "filter_activities", g_filter.activities);
    ui_state_extract_bool(s, "filter_chests",     g_filter.chests);
    ui_state_extract_bool(s, "filter_red_orbs",   g_filter.red_orbs);
    ui_state_extract_bool(s, "filter_plants",     g_filter.plants);
    ui_state_extract_bool(s, "filter_ores",       g_filter.ores);
    ui_state_extract_bool(s, "guide_nearest",     g_guide_nearest);

    bool rot = g_minimap_rotate.load();
    if (ui_state_extract_bool(s, "minimap_rotate", rot))
        g_minimap_rotate.store(rot);
    bool rot_cam = g_minimap_follow_camera.load();
    if (ui_state_extract_bool(s, "minimap_follow_camera", rot_cam))
        g_minimap_follow_camera.store(rot_cam);

    bool pt = g_plugin_theme.load();
    if (ui_state_extract_bool(s, "plugin_theme", pt))
        g_plugin_theme.store(pt);

    // #57/#70: meter view is a string value, so match the literal.
    if (s.find("\"meter_view\": \"shield\"") != std::string::npos)
        g_meter_view.store(MV_SHIELD);
    else if (s.find("\"meter_view\": \"heal\"") != std::string::npos)
        g_meter_view.store(MV_HEAL);
    else if (s.find("\"meter_view\": \"dmg\"") != std::string::npos)
        g_meter_view.store(MV_DMG);

    // Fight-detail view is independent from the live meter.
    if (s.find("\"fight_detail_view\": \"shield\"") != std::string::npos)
        g_fight_detail_view.store(MV_SHIELD);
    else if (s.find("\"fight_detail_view\": \"heal\"") != std::string::npos)
        g_fight_detail_view.store(MV_HEAL);
    else if (s.find("\"fight_detail_view\": \"dmg\"") != std::string::npos)
        g_fight_detail_view.store(MV_DMG);
}

void ui_lock_save() {
    std::ofstream f(kUiStatePath());
    if (!f) return;
    f << "{\n"
      << "  \"locked\":   " << (g_ui_locked.load() ? "true" : "false") << ",\n"
      << "  \"dps_locked\": " << (g_dps_locked.load() ? "true" : "false") << ",\n"
      << "  \"pin\":      " << g_bezel.pin      << ",\n"
      << "  \"size\":     " << g_bezel.size     << ",\n"
      << "  \"collapse\": " << g_bezel.collapse << ",\n"
      << "  \"filter\":   " << g_bezel.filter   << ",\n"
      << "  \"lock\":     " << g_bezel.lock     << ",\n"
      << "  \"keys\":     " << g_bezel.keys     << ",\n"
      << "  \"chest\":    " << g_bezel.chest    << ",\n"
      << "  \"plus\":     " << g_bezel.plus     << ",\n"
      << "  \"minus\":    " << g_bezel.minus    << ",\n"
      << "  \"overview\": " << g_bezel.overview << ",\n"
      // #256: persist the user's chosen minimap zoom. While the full-zone
      // overview is active we store the zoom they had before, not the wide
      // overview value, so a restart restores their real preference.
      << "  \"zoom\": " << (g_overview_active ? g_overview_saved_zoom : g_calib.zoom) << ",\n"
      << "  \"compass_size\": " << g_compass_size_idx << ",\n"
      << "  \"clickthrough\": " << (g_clickthrough.load() ? "true" : "false") << ",\n"
      << "  \"dps_tracking_paused\": " << (g_dps_tracking_paused.load() ? "true" : "false") << ",\n"
      << "  \"minimap_alpha\": " << g_minimap_alpha << ",\n"
      << "  \"window_bg_alpha\": " << g_window_bg_alpha << ",\n"
      << "  \"ui_scale\": " << g_ui_scale << ",\n"
      << "  \"loot_counter_visible\": "
      << (g_loot_counter_visible.load() ? "true" : "false") << ",\n"
      << "  \"minimap_square\": "
      << (g_minimap_square.load() ? "true" : "false") << ",\n"
      // #47: show-hide state + POI filter checkboxes.
      << "  \"dps_visible\": "
      << (g_dps_visible.load() ? "true" : "false") << ",\n"
      << "  \"minimap_visible\": "
      << (g_minimap_visible.load() ? "true" : "false") << ",\n"
      << "  \"filter_obelisks\": "   << (g_filter.obelisks   ? "true" : "false") << ",\n"
      << "  \"filter_respawns\": "   << (g_filter.respawns   ? "true" : "false") << ",\n"
      << "  \"filter_merchants\": "  << (g_filter.merchants  ? "true" : "false") << ",\n"
      << "  \"filter_dungeons\": "   << (g_filter.dungeons   ? "true" : "false") << ",\n"
      << "  \"filter_activities\": " << (g_filter.activities ? "true" : "false") << ",\n"
      << "  \"filter_chests\": "     << (g_filter.chests     ? "true" : "false") << ",\n"
      << "  \"filter_red_orbs\": "   << (g_filter.red_orbs   ? "true" : "false") << ",\n"
      << "  \"filter_plants\": "     << (g_filter.plants     ? "true" : "false") << ",\n"
      << "  \"filter_ores\": "       << (g_filter.ores       ? "true" : "false") << ",\n"
      << "  \"guide_nearest\": "     << (g_guide_nearest     ? "true" : "false") << ",\n"
      << "  \"minimap_rotate\": "
      << (g_minimap_rotate.load() ? "true" : "false") << ",\n"
      << "  \"minimap_follow_camera\": "
      << (g_minimap_follow_camera.load() ? "true" : "false") << ",\n"
      << "  \"meter_view\": \""
      << (g_meter_view.load() == MV_SHIELD ? "shield"
          : g_meter_view.load() == MV_HEAL ? "heal" : "dmg") << "\",\n"
      << "  \"fight_detail_view\": \""
      << (g_fight_detail_view.load() == MV_SHIELD ? "shield"
          : g_fight_detail_view.load() == MV_HEAL ? "heal" : "dmg") << "\",\n"
      << "  \"compass_visible\": "
      << (g_compass_visible.load() ? "true" : "false") << ",\n"
      << "  \"compass_radius_m\": " << g_compass_radius_m.load() << ",\n"
      << "  \"compass_show_cardinals\": "
      << (g_compass_show_cardinals.load() ? "true" : "false") << ",\n"
      << "  \"compass_follow_camera\": "
      << (g_compass_follow_camera.load() ? "true" : "false") << ",\n"
      << "  \"boss_speedrun_enabled\": "
      << (g_boss_speedrun_enabled.load() ? "true" : "false") << ",\n"
      << "  \"plugin_theme\": "
      << (g_plugin_theme.load() ? "true" : "false") << ",\n"
      << "  \"party_visible\": "
      << (g_party_visible.load() ? "true" : "false") << "\n"
      << "}\n";
}

void keybinds_save() {
    std::ofstream f(kKeybindsPath());
    if (!f) {
        logf("keybinds: cannot open keybinds.json for write");
        return;
    }
    f << "{\n"
      << "  \"_comment\": \"Rebind via the minimap key panel or edit by hand. "
         "Names: F1..F24, A..Z, 0..9, Home, End, Insert, Delete, PageUp, "
         "PageDown, Up, Down, Left, Right, Space, Enter, Tab, Esc, "
         "Backspace, Numpad0..Numpad9, OEM_1..OEM_8.\",\n"
      << "  \"toggle_dps\":         \"" << key_to_name(g_keybinds.toggle_dps)       << "\",\n"
      << "  \"toggle_minimap\":     \"" << key_to_name(g_keybinds.toggle_minimap)   << "\",\n"
      << "  \"reset_dps\":          \"" << key_to_name(g_keybinds.reset_dps)        << "\",\n"
      << "  \"toggle_clickthru\":   \"" << key_to_name(g_keybinds.toggle_clickthru) << "\",\n"
      << "  \"toggle_dps_track\":   \"" << key_to_name(g_keybinds.toggle_dps_track) << "\",\n"
      << "  \"toggle_overlay\":     \"" << key_to_name(g_keybinds.toggle_overlay)   << "\",\n"
      << "  \"reset_positions\":    \"" << key_to_name(g_keybinds.reset_positions)  << "\"\n"
      << "}\n";
}

// world (m) -> mosaic pixel (top-left origin).
ImVec2 world_to_full(double world_x, double world_y) {
    float fx = (float)world_x * g_calib.world_to_full_x_scale
               + g_calib.world_to_full_x_offset;
    float fy = (float)world_y * g_calib.world_to_full_y_scale
               + g_calib.world_to_full_y_offset;
    if (g_calib.flip_y) fy = kFullMosaicPx - fy;
    return ImVec2(fx, fy);
}

struct ViewUV { ImVec2 uv0; ImVec2 uv1; };

ViewUV compute_view_uv(double wx, double wy, bool have_player) {
    float vsize = 1.0f / g_calib.zoom;
    if (vsize >= 1.0f || !have_player) return {ImVec2(0,0), ImVec2(1,1)};
    ImVec2 full = world_to_full(wx, wy);
    float cu = full.x / kFullMosaicPx;
    float cv = full.y / kFullMosaicPx;
    float half = vsize * 0.5f;
    if (cu < half)        cu = half;
    if (cu > 1.0f - half) cu = 1.0f - half;
    if (cv < half)        cv = half;
    if (cv > 1.0f - half) cv = 1.0f - half;
    return {ImVec2(cu - half, cv - half), ImVec2(cu + half, cv + half)};
}

ImVec2 player_to_screen(double wx, double wy, const ViewUV& v, float size_px) {
    ImVec2 full = world_to_full(wx, wy);
    float u = full.x / kFullMosaicPx;
    float vv = full.y / kFullMosaicPx;
    float sx = (u  - v.uv0.x) / (v.uv1.x - v.uv0.x) * size_px;
    float sy = (vv - v.uv0.y) / (v.uv1.y - v.uv0.y) * size_px;
    return ImVec2(sx, sy);
}

// 8-entry palette for user waypoints. Index 0 = cyan, the v1.0.0-beta4
// default; matches the previous draw_waypoint_pin fill so old (style=0)
// waypoints render identically. #229
constexpr ImU32 kWpPalette[8] = {
    IM_COL32( 80, 200, 255, 255),  // 0 cyan    (default)
    IM_COL32(240,  80,  80, 255),  // 1 red
    IM_COL32(255, 140,  40, 255),  // 2 orange
    IM_COL32(240, 220,  60, 255),  // 3 yellow
    IM_COL32( 80, 220, 100, 255),  // 4 green
    IM_COL32( 80, 130, 240, 255),  // 5 blue
    IM_COL32(220,  90, 220, 255),  // 6 magenta
    IM_COL32(240, 240, 240, 255),  // 7 white
};

// All six icon helpers anchor on `tip` so it stays the exact world point
// regardless of icon. `s` is the post-hover size. fill/outline are picked
// by the caller from kWpPalette and a fixed outline.

void draw_icon_pin(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    const ImU32 dot = IM_COL32(20, 40, 60, 255);
    float head_r = s * 0.62f;
    ImVec2 head(tip.x, tip.y - s * 1.1f);
    ImVec2 lshoulder(head.x - head_r * 0.7f, head.y + head_r * 0.2f);
    ImVec2 rshoulder(head.x + head_r * 0.7f, head.y + head_r * 0.2f);
    dl->AddTriangleFilled(lshoulder, rshoulder, tip, fill);
    dl->AddCircleFilled(head, head_r, fill, 16);
    dl->AddCircle(head, head_r, outline, 16, 1.5f);
    dl->AddLine(lshoulder, tip, outline, 1.0f);
    dl->AddLine(rshoulder, tip, outline, 1.0f);
    dl->AddCircleFilled(head, head_r * 0.42f, dot, 12);
}

void draw_icon_flag(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    ImVec2 top(tip.x, tip.y - 2.0f * s);
    dl->AddLine(tip, top, outline, 1.5f);
    ImVec2 a(tip.x,            tip.y - 2.0f * s);
    ImVec2 b(tip.x + 1.4f * s, tip.y - 1.55f * s);
    ImVec2 c(tip.x,            tip.y - 1.1f  * s);
    dl->AddTriangleFilled(a, b, c, fill);
    dl->AddTriangle(a, b, c, outline, 1.0f);
}

void draw_icon_star(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    ImVec2 ctr(tip.x, tip.y - s);
    const float r_out = 0.9f * s;
    const float r_in  = 0.42f * s;
    const float kPi2  = 6.2831853f;
    ImVec2 v[10];
    for (int i = 0; i < 10; ++i) {
        float a = -kPi2 * 0.25f + (kPi2 / 10.0f) * (float)i;  // tip up
        float r = (i & 1) ? r_in : r_out;
        v[i] = ImVec2(ctr.x + std::cosf(a) * r, ctr.y + std::sinf(a) * r);
    }
    // Central pentagon (inner vertices i=1,3,5,7,9) filled as a convex polygon.
    ImVec2 inner[5] = { v[1], v[3], v[5], v[7], v[9] };
    dl->AddConvexPolyFilled(inner, 5, fill);
    // Five tip triangles: outer vertex + its two pentagon-edge neighbors.
    for (int i = 0; i < 5; ++i) {
        ImVec2 outer = v[i * 2];
        ImVec2 prev  = v[(i * 2 + 9) % 10];
        ImVec2 next  = v[(i * 2 + 1) % 10];
        dl->AddTriangleFilled(prev, outer, next, fill);
    }
    // Outline through all 10 alternating vertices.
    dl->AddPolyline(v, 10, outline, 1.0f, ImDrawFlags_Closed);
}

void draw_icon_diamond(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    ImVec2 ctr(tip.x, tip.y - s);
    ImVec2 a(ctr.x,             ctr.y - 0.85f * s);
    ImVec2 b(ctr.x + 0.85f * s, ctr.y            );
    ImVec2 c(ctr.x,             ctr.y + 0.85f * s);
    ImVec2 d(ctr.x - 0.85f * s, ctr.y            );
    dl->AddQuadFilled(a, b, c, d, fill);
    ImVec2 poly[4] = { a, b, c, d };
    dl->AddPolyline(poly, 4, outline, 1.0f, ImDrawFlags_Closed);
}

void draw_icon_circle(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    ImVec2 ctr(tip.x, tip.y - s);
    dl->AddCircleFilled(ctr, 0.75f * s, fill, 16);
    dl->AddCircle(ctr, 0.75f * s, outline, 16, 1.5f);
    dl->AddCircleFilled(ctr, 0.18f * s, IM_COL32(20, 40, 60, 255), 8);
}

void draw_icon_exclamation(ImDrawList* dl, ImVec2 tip, float s, ImU32 fill, ImU32 outline) {
    ImVec2 bar_ctr(tip.x, tip.y - 1.2f * s);
    ImVec2 bar_min(bar_ctr.x - 0.225f * s, bar_ctr.y - 0.7f * s);
    ImVec2 bar_max(bar_ctr.x + 0.225f * s, bar_ctr.y + 0.7f * s);
    dl->AddRectFilled(bar_min, bar_max, fill, 1.0f);
    dl->AddRect(bar_min, bar_max, outline, 1.0f, 0, 1.0f);
    ImVec2 dot_ctr(tip.x, tip.y - 0.15f * s);
    dl->AddCircleFilled(dot_ctr, 0.32f * s, fill, 10);
    dl->AddCircle(dot_ctr, 0.32f * s, outline, 10, 1.0f);
}

// Small heading-rotated arrow for a party member. `tip` is the marker
// anchor; `s` is the arrow half-length. heading_rad is the final
// world-rotated angle (caller applies any heading-up correction).
void draw_party_arrow(ImDrawList* dl, ImVec2 tip, float s,
                      float heading_rad, ImU32 fill, ImU32 outline) {
    float c  = std::cosf(heading_rad);
    float sn = std::sinf(heading_rad);
    auto rot = [&](float lx, float ly) {
        return ImVec2(tip.x + lx * c - ly * sn,
                      tip.y + lx * sn + ly * c);
    };
    ImVec2 p0 = rot(  s,         0.0f);
    ImVec2 p1 = rot(-s * 0.7f, -s * 0.8f);
    ImVec2 p2 = rot(-s * 0.7f,  s * 0.8f);
    dl->AddTriangleFilled(p0, p1, p2, fill);
    dl->AddTriangle(p0, p1, p2, outline, 1.0f);
}

// Centered label with a 1-px shadow for legibility against the map.
void draw_party_label(ImDrawList* dl, ImVec2 anchor, float offset_y,
                      const char* text) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 lp(anchor.x - ts.x * 0.5f, anchor.y + offset_y);
    dl->AddText(ImVec2(lp.x + 1.0f, lp.y + 1.0f),
                IM_COL32(0, 0, 0, 180), text);
    dl->AddText(lp, IM_COL32(220, 240, 220, 255), text);
}

// Clamp a screen point to the inside of the minimap clip region.
ImVec2 clamp_to_minimap(ImVec2 sp, ImVec2 center, float clip_r,
                        bool square, ImVec2 p_min, ImVec2 p_max) {
    if (square) {
        float lo_x = p_min.x + 4.0f, hi_x = p_max.x - 4.0f;
        float lo_y = p_min.y + 4.0f, hi_y = p_max.y - 4.0f;
        if (sp.x < lo_x) sp.x = lo_x;
        if (sp.x > hi_x) sp.x = hi_x;
        if (sp.y < lo_y) sp.y = lo_y;
        if (sp.y > hi_y) sp.y = hi_y;
        return sp;
    }
    float dx = sp.x - center.x, dy = sp.y - center.y;
    float d2 = dx * dx + dy * dy;
    float r = clip_r - 4.0f;
    if (d2 <= r * r) return sp;
    float d = std::sqrtf(d2);
    return ImVec2(center.x + dx * r / d, center.y + dy * r / d);
}

// Forward decl so wp_icon_button can call into the dispatcher below. #229
void draw_waypoint(ImDrawList* dl, ImVec2 tip, float size,
                   bool hovered, uint8_t color, uint8_t icon);

// Picker button: 18x18 cell that renders the actual icon in the actual
// color, with a thin border highlight when `selected`. Click returns true.
bool wp_icon_button(const char* id, uint8_t icon, uint8_t color, bool selected) {
    const float kBtn = 18.0f;
    ImGui::PushID(id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(kBtn, kBtn));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRect(p0, ImVec2(p0.x + kBtn, p0.y + kBtn),
                    IM_COL32(255, 255, 255, 255), 2.0f, 0, 1.0f);
    } else if (ImGui::IsItemHovered()) {
        dl->AddRect(p0, ImVec2(p0.x + kBtn, p0.y + kBtn),
                    IM_COL32(180, 180, 180, 255), 2.0f, 0, 1.0f);
    }
    // Draw the procedural icon centered in the cell. The icon helpers
    // anchor on tip (the bottom), so place tip near the bottom of the cell.
    ImVec2 tip(p0.x + kBtn * 0.5f, p0.y + kBtn - 2.0f);
    draw_waypoint(dl, tip, 6.5f, false, color, icon);
    ImGui::PopID();
    return clicked;
}

// Dispatcher: picks color from the palette + icon helper. `tip` is the
// exact world point on the minimap. #229
void draw_waypoint(ImDrawList* dl, ImVec2 tip, float size,
                   bool hovered, uint8_t color, uint8_t icon) {
    const ImU32 fill    = kWpPalette[color < 8 ? color : 0];
    const ImU32 outline = IM_COL32(0, 0, 0, 230);
    float s = hovered ? size * 1.6f : size;
    switch (icon < 6 ? icon : 0) {
        case 0: draw_icon_pin        (dl, tip, s, fill, outline); break;
        case 1: draw_icon_flag       (dl, tip, s, fill, outline); break;
        case 2: draw_icon_star       (dl, tip, s, fill, outline); break;
        case 3: draw_icon_diamond    (dl, tip, s, fill, outline); break;
        case 4: draw_icon_circle     (dl, tip, s, fill, outline); break;
        case 5: draw_icon_exclamation(dl, tip, s, fill, outline); break;
    }
}

// Inverse of rot_pt(player_to_screen(...)): absolute screen point -> world.
// Only fed into a user-confirmed "Add waypoint here", so a degenerate
// calibration at worst misplaces a pin; scales are non-zero in all shipped
// calibrations. #58
void screen_to_world(ImVec2 abs, ImVec2 center, ImVec2 p_min,
                     const ViewUV& v, float size_px,
                     bool rotate, float rot_ca, float rot_sa,
                     double& wx, double& wy) {
    float ux = abs.x, uy = abs.y;
    if (rotate) {
        float dx = abs.x - center.x, dy = abs.y - center.y;
        ux = center.x + dx * rot_ca + dy * rot_sa;   // rotate by -rot_a
        uy = center.y - dx * rot_sa + dy * rot_ca;
    }
    float sx = ux - p_min.x, sy = uy - p_min.y;
    float u  = v.uv0.x + (sx / size_px) * (v.uv1.x - v.uv0.x);
    float vv = v.uv0.y + (sy / size_px) * (v.uv1.y - v.uv0.y);
    float fx = u  * kFullMosaicPx;
    float fy = vv * kFullMosaicPx;
    float fy0 = g_calib.flip_y ? (kFullMosaicPx - fy) : fy;
    wx = ((double)fx  - g_calib.world_to_full_x_offset) / g_calib.world_to_full_x_scale;
    wy = ((double)fy0 - g_calib.world_to_full_y_offset) / g_calib.world_to_full_y_scale;
}

// --- bezel buttons --------------------------------------------------

struct BezelButton {
    ImVec2 center;
    float  radius;
    bool   clicked;
    bool   hovered;
    bool   active;
};

// Project a polar ray (angle_rad) to a point on the bezel rim. In
// circular mode the rim is a circle of radius bezel_r. In square mode
// the rim is the inscribed square (also half-size bezel_r), so we scale
// the ray out to the square's perimeter. Same input angle => same
// perimeter slot, which means user-customized button angles stay in
// the same relative order when toggling the shape.
ImVec2 bezel_rim_pos(ImVec2 center, float bezel_r, float angle_rad,
                     bool square) {
    float cx = cosf(angle_rad), sy = sinf(angle_rad);
    if (square) {
        float ax = std::fabs(cx), ay = std::fabs(sy);
        float m = (ax > ay) ? ax : ay;
        if (m < 1e-6f) m = 1e-6f;
        float t = bezel_r / m;
        return ImVec2(center.x + cx * t, center.y + sy * t);
    }
    return ImVec2(center.x + cx * bezel_r, center.y + sy * bezel_r);
}

BezelButton bezel_hit(const char* id, ImVec2 center, float bezel_r,
                      float angle_rad, float btn_r, bool square = false) {
    BezelButton b{};
    b.center = bezel_rim_pos(center, bezel_r, angle_rad, square);
    b.radius = btn_r;
    ImGui::SetCursorScreenPos(ImVec2(b.center.x - btn_r, b.center.y - btn_r));
    ImGui::SetNextItemAllowOverlap();
    b.clicked = ImGui::InvisibleButton(id, ImVec2(btn_r * 2, btn_r * 2));
    b.hovered = ImGui::IsItemHovered();
    b.active  = ImGui::IsItemActive();
    return b;
}

void bezel_draw_base(ImDrawList* dl, const BezelButton& b) {
    ImU32 fill = b.active ? kColBtnActive : b.hovered ? kColBtnHover : kColBtnFill;
    dl->AddCircleFilled(b.center, b.radius, fill, 32);
    dl->AddCircle(b.center, b.radius, kColBezel, 32, 2.0f);
}

void bezel_draw_plus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.45f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
    dl->AddLine({b.center.x, b.center.y - a}, {b.center.x, b.center.y + a}, kColIcon, 2.5f);
}
void bezel_draw_minus(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.45f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
}
void bezel_draw_pin(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    ImVec2 head(b.center.x - 1.5f, b.center.y - 3.5f);
    ImVec2 tip (b.center.x + 4.5f, b.center.y + 5.5f);
    dl->AddLine(head, tip, kColIcon, 2.0f);
    dl->AddCircleFilled(head, 3.0f, kColIcon, 12);
}
void bezel_draw_size(ImDrawList* dl, const BezelButton& b, int idx) {
    bezel_draw_base(dl, b);
    const float steps[3] = { 4.0f, 6.0f, 8.0f };
    float h = steps[idx] * 0.5f;
    dl->AddRect({b.center.x - h, b.center.y - h},
                {b.center.x + h, b.center.y + h}, kColIcon, 1.0f, 0, 1.8f);
}
void bezel_draw_collapse(ImDrawList* dl, const BezelButton& b) {
    bezel_draw_base(dl, b);
    float a = b.radius * 0.5f;
    dl->AddLine({b.center.x - a, b.center.y}, {b.center.x + a, b.center.y}, kColIcon, 2.5f);
}
void bezel_draw_filter(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 255, 200, 255) : kColIcon;
    // Gear / cog: a ring body, eight chunky radial teeth, and a hub.
    const float kPi    = 3.14159265f;
    const float r_body = 3.4f;   // gear-ring radius
    const float r_tip  = 5.4f;   // tooth-tip radius
    const int   teeth  = 8;
    for (int i = 0; i < teeth; ++i) {
        float a  = (kPi * 2.0f / (float)teeth) * (float)i;
        float ca = cosf(a), sa = sinf(a);
        dl->AddLine({b.center.x + ca * r_body, b.center.y + sa * r_body},
                    {b.center.x + ca * r_tip,  b.center.y + sa * r_tip},
                    c, 2.4f);
    }
    dl->AddCircle(b.center, r_body, c, 20, 1.8f);   // gear body
    dl->AddCircleFilled(b.center, 1.4f, c, 10);     // hub
}
void bezel_draw_key(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 255, 200, 255) : kColIcon;
    ImVec2 bow(b.center.x - 3.0f, b.center.y - 1.0f);
    dl->AddCircle(bow, 3.0f, c, 14, 1.6f);
    dl->AddLine({bow.x + 3.0f, bow.y},
                {b.center.x + 5.0f, bow.y}, c, 1.8f);
    dl->AddLine({b.center.x + 3.0f, bow.y},
                {b.center.x + 3.0f, bow.y + 3.0f}, c, 1.8f);
    dl->AddLine({b.center.x + 5.0f, bow.y},
                {b.center.x + 5.0f, bow.y + 3.0f}, c, 1.8f);
}
void bezel_draw_lock(ImDrawList* dl, const BezelButton& b, bool locked) {
    bezel_draw_base(dl, b);
    ImU32 c = locked ? IM_COL32(255, 220, 100, 255) : kColIcon;
    ImVec2 tl(b.center.x - 4.0f, b.center.y);
    ImVec2 br(b.center.x + 4.0f, b.center.y + 4.5f);
    dl->AddRect(tl, br, c, 0.8f, 0, 1.5f);
    dl->AddCircleFilled({b.center.x, b.center.y + 2.0f}, 0.9f, c, 8);
    ImVec2 lShank(b.center.x - 2.5f, b.center.y);
    ImVec2 lTop  (b.center.x - 2.5f, b.center.y - 3.5f);
    ImVec2 rTop  (b.center.x + 2.5f, b.center.y - 3.5f);
    ImVec2 rShank(b.center.x + 2.5f, b.center.y);
    dl->AddLine(lShank, lTop, c, 1.5f);
    dl->AddLine(lTop,   rTop, c, 1.5f);
    if (locked) dl->AddLine(rTop, rShank, c, 1.5f);
}
void bezel_draw_chest(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 220, 100, 255) : kColIcon;
    float w = 5.5f, hUp = 4.0f, hDn = 2.5f;
    ImVec2 tl(b.center.x - w, b.center.y - hUp);
    ImVec2 tr(b.center.x + w, b.center.y - hUp);
    ImVec2 bl(b.center.x - w, b.center.y + hDn);
    ImVec2 br(b.center.x + w, b.center.y + hDn);
    dl->AddRect(tl, br, c, 0.5f, 0, 1.6f);
    dl->AddLine({tl.x, b.center.y - 1.0f},
                {tr.x, b.center.y - 1.0f}, c, 1.4f);
    dl->AddRectFilled(
        {b.center.x - 1.0f, b.center.y - 1.5f},
        {b.center.x + 1.0f, b.center.y + 1.0f}, c);
}
// #83: "show full zone" toggle. Four outward diagonal arrows (the universal
// expand glyph). Lit gold while the overview view is active.
void bezel_draw_overview(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 220, 100, 255) : kColIcon;
    const float in = 1.6f, out = 4.6f, head = 2.0f;
    const float sgn[4][2] = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    for (const auto& s : sgn) {
        ImVec2 ip(b.center.x + s[0] * in,  b.center.y + s[1] * in);
        ImVec2 op(b.center.x + s[0] * out, b.center.y + s[1] * out);
        dl->AddLine(ip, op, c, 1.6f);                       // shaft
        dl->AddLine(op, {op.x - s[0] * head, op.y}, c, 1.6f);  // barb (x)
        dl->AddLine(op, {op.x, op.y - s[1] * head}, c, 1.6f);  // barb (y)
    }
}

// Draw the minimap mosaic with heading-up rotation applied. Square mode
// oversamples a rotated quad (rect-clipped to the viewport); circular
// mode draws a textured triangle-fan disc so anything outside the disc
// stays see-through. Factored out of render_compass to keep that
// function readable; only called when rotation is on. rot_ca/rot_sa are
// cos/sin of the scene rotation angle.
void draw_mosaic_rotated(ImDrawList* dl, bool square, ImVec2 center, float r,
                         ImVec2 p_min, ImVec2 p_max, float size,
                         const ViewUV& view, ImU32 tint,
                         float rot_ca, float rot_sa) {
    constexpr float kPi = 3.14159265358979323846f;
    const ImTextureID tex = (ImTextureID)g_overlay.mosaic.srv_gpu.ptr;

    if (square) {
        // Oversample by ~sqrt(2) so the rotated quad always covers the
        // viewport; the screen quad rotates while UVs stay axis-aligned
        // (same uv-per-pixel scale at center, so POI alignment holds).
        auto rot_pt = [&](ImVec2 p) -> ImVec2 {
            float dx = p.x - center.x, dy = p.y - center.y;
            return ImVec2(center.x + dx * rot_ca - dy * rot_sa,
                          center.y + dx * rot_sa + dy * rot_ca);
        };
        const float S = 1.45f;
        ImVec2 cuv((view.uv0.x + view.uv1.x) * 0.5f,
                   (view.uv0.y + view.uv1.y) * 0.5f);
        float uhx = (view.uv1.x - view.uv0.x) * 0.5f * S;
        float uhy = (view.uv1.y - view.uv0.y) * 0.5f * S;
        dl->PushClipRect(p_min, p_max, true);
        dl->AddImageQuad(tex,
                         rot_pt({center.x - r * S, center.y - r * S}),
                         rot_pt({center.x + r * S, center.y - r * S}),
                         rot_pt({center.x + r * S, center.y + r * S}),
                         rot_pt({center.x - r * S, center.y + r * S}),
                         {cuv.x - uhx, cuv.y - uhy}, {cuv.x + uhx, cuv.y - uhy},
                         {cuv.x + uhx, cuv.y + uhy}, {cuv.x - uhx, cuv.y + uhy},
                         tint);
        dl->PopClipRect();
        return;
    }

    // Circular: textured triangle-fan disc. uv_for inverse-rotates a
    // screen point back to the north-up sampling box, then maps it
    // linearly into the mosaic UVs.
    auto uv_for = [&](ImVec2 P) -> ImVec2 {
        float dx = P.x - center.x, dy = P.y - center.y;
        float nx = center.x + dx * rot_ca + dy * rot_sa;   // R(-angle)
        float ny = center.y - dx * rot_sa + dy * rot_ca;
        return ImVec2(
            view.uv0.x + (nx - p_min.x) / size * (view.uv1.x - view.uv0.x),
            view.uv0.y + (ny - p_min.y) / size * (view.uv1.y - view.uv0.y));
    };
    const int NSEG = 96;
    const int vtx_count = NSEG + 2;     // center + (NSEG+1) rim
    const int idx_count = NSEG * 3;
    dl->PushTextureID(tex);
    dl->PrimReserve(idx_count, vtx_count);
    const unsigned int base = dl->_VtxCurrentIdx;
    ImDrawVert* vp = dl->_VtxWritePtr;
    ImDrawIdx*  ip = dl->_IdxWritePtr;
    vp[0].pos = center; vp[0].uv = uv_for(center); vp[0].col = tint;
    for (int i = 0; i <= NSEG; ++i) {
        float th = (float)i / (float)NSEG * 2.0f * kPi;
        ImVec2 P(center.x + cosf(th) * r, center.y + sinf(th) * r);
        vp[1 + i].pos = P; vp[1 + i].uv = uv_for(P); vp[1 + i].col = tint;
    }
    for (int i = 0; i < NSEG; ++i) {
        ip[i * 3 + 0] = (ImDrawIdx)(base);
        ip[i * 3 + 1] = (ImDrawIdx)(base + 1 + i);
        ip[i * 3 + 2] = (ImDrawIdx)(base + 2 + i);
    }
    dl->_VtxWritePtr   += vtx_count;
    dl->_IdxWritePtr   += idx_count;
    dl->_VtxCurrentIdx += vtx_count;
    dl->PopTextureID();
}

void render_compass(const HeroSnapshot& h) {
    constexpr float kPi = 3.14159265358979323846f;
    const float size = compass_size_px();
    const float r    = size * 0.5f;
    const float btn_r =
        (size <= 256.0f) ? 11.0f : (size <= 384.0f) ? 13.0f : 14.0f;

    ImVec2 p_min  = ImGui::GetCursorScreenPos();
    ImVec2 p_max(p_min.x + size, p_min.y + size);
    ImVec2 center(p_min.x + r,   p_min.y + r);

    // Body hit-area first (AllowOverlap so the bezel buttons can take
    // priority over the disc).
    ImGui::SetCursorScreenPos(p_min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##compass_body", ImVec2(size, size));

    // v0.5.5: square mode sampled here so bezel buttons + the rim/
    // image swap below all stay in sync within a single frame.
    const bool square_bezel = g_minimap_square.load(std::memory_order_acquire);
    BezelButton pin      = bezel_hit("##pin",        center, r, g_bezel.pin,      btn_r, square_bezel);
    BezelButton sizeb    = bezel_hit("##size_cycle", center, r, g_bezel.size,     btn_r, square_bezel);
    BezelButton collapse = bezel_hit("##collapse",   center, r, g_bezel.collapse, btn_r, square_bezel);
    BezelButton filter   = bezel_hit("##filter",     center, r, g_bezel.filter,   btn_r, square_bezel);
    BezelButton lockb    = bezel_hit("##lock",       center, r, g_bezel.lock,     btn_r, square_bezel);
    BezelButton keys     = bezel_hit("##keys",       center, r, g_bezel.keys,     btn_r, square_bezel);
    BezelButton chest    = bezel_hit("##chest",      center, r, g_bezel.chest,    btn_r, square_bezel);
    BezelButton plus     = bezel_hit("##zoom_plus",  center, r, g_bezel.plus,     btn_r, square_bezel);
    BezelButton minus    = bezel_hit("##zoom_minus", center, r, g_bezel.minus,    btn_r, square_bezel);
    BezelButton overview = bezel_hit("##overview",   center, r, g_bezel.overview, btn_r, square_bezel);

    auto* dl = ImGui::GetWindowDrawList();

    // Render-thread motion smoothing (no extra HL reads). The worker
    // refreshes the hero snapshot at 40 Hz but we render at frame rate,
    // so without this the player dot / map pan / heading step in ~25 ms
    // jumps. Critically-damped exponential lerp toward the latest
    // snapshot each frame keeps it smooth. Snap (don't smooth) on a big
    // jump (teleport / fresh lock) or while unlocked.
    static bool   s_sm_init = false;
    static double s_px = 0.0, s_py = 0.0;
    static float  s_heading = 0.0f;   // hero facing - drives the player arrow
    static float  s_map_up  = 0.0f;   // what the map rotates to (see below)
    {
        // #105: the map's "up" is the hero facing by default and the camera
        // yaw when the user asked for that. The arrow keeps tracking the hero
        // either way, so with camera-up it correctly shows the facing relative
        // to the camera rather than always pointing straight up.
        double cam_yaw = 0.0;
        const bool cam_ok = camera_state_yaw(cam_yaw);
        const float map_up_target = compass_heading_ref(
            g_minimap_follow_camera.load(std::memory_order_acquire),
            cam_ok, cam_yaw, h.rot_z);

        float dt = ImGui::GetIO().DeltaTime;   // ImGui's per-frame delta
        if (dt < 0.0f)  dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;   // clamp after a stall
        double dx = h.x - s_px, dy = h.y - s_py;
        bool snap = !s_sm_init || !h.locked ||
                    (dx * dx + dy * dy) > 500.0 * 500.0;
        if (snap) {
            s_px = h.x; s_py = h.y;
            s_heading = (float)h.rot_z;
            s_map_up  = map_up_target;
            s_sm_init = true;
        } else {
            // Position follows quickly; heading is damped much harder so
            // the map does not chase every facing wiggle while strafe-
            // walking (W+A / W+D). Separate time constants.
            constexpr float kTauPos     = 0.06f;   // ~60 ms, snappy pan
            constexpr float kTauHeading = 0.25f;   // ~250 ms, calm rotation
            float ap = 1.0f - std::exp(-dt / kTauPos);
            float ah = 1.0f - std::exp(-dt / kTauHeading);
            s_px += (h.x - s_px) * ap;
            s_py += (h.y - s_py) * ap;
            auto lerp_angle = [&](float& cur, float target) {
                float d = target - cur;             // shortest-angle lerp
                while (d >  kPi) d -= 2.0f * kPi;
                while (d < -kPi) d += 2.0f * kPi;
                cur += d * ah;
            };
            lerp_angle(s_heading, (float)h.rot_z);
            lerp_angle(s_map_up,  map_up_target);
        }
    }
    const double sm_x = s_px, sm_y = s_py;
    const float  sm_heading = s_heading;   // hero facing
    const float  sm_map_up  = s_map_up;    // map rotation reference

    ViewUV view = compute_view_uv(sm_x, sm_y, h.locked);

    // Apply minimap alpha (issue #2). 1.0 = original behaviour, lower
    // values fade the mosaic / bezel toward the game underneath. We
    // tint the texture and scale the bezel alpha channel.
    std::uint8_t alpha8 = (std::uint8_t)(g_minimap_alpha * 255.0f + 0.5f);
    ImU32 tint_mosaic = IM_COL32(255, 255, 255, alpha8);
    auto scale_alpha = [alpha8](ImU32 c) -> ImU32 {
        std::uint32_t a = (c >> 24) & 0xff;
        a = (a * alpha8) / 255;
        return (c & 0x00ffffff) | (a << 24);
    };
    // Skeleton mode (issue #12): during the post-pause cool-down we
    // skip the mosaic image entirely. Bezel + player arrow + buttons
    // still draw so the user can see *something*, just no map content.
    const bool skeleton = g_skeleton_minimap.load(std::memory_order_acquire);
    // v0.5.5: square minimap shape. Sampled once above as square_bezel
    // (so bezel-button placement stays in sync); reuse the same value
    // here for the mosaic clip + bezel border + POI clipping below.
    const bool square = square_bezel;

    // Heading-up rotation setup. rot_a brings the reference angle
    // (sm_map_up: the player's facing, or the camera yaw when the user
    // picked that) to straight up (-pi/2), so what is ahead lands at the
    // top. rot_pt rotates any screen point around the compass center by
    // rot_a; every world-anchored angle drawn below adds rot_a for the
    // same reason. Off => no-op and every draw takes the legacy
    // (north-up) path unchanged.
    const bool  rotate = g_minimap_rotate.load(std::memory_order_acquire) &&
                         h.locked;
    const float rot_a  = rotate ? (-sm_map_up - kPi * 0.5f) : 0.0f;
    const float rot_ca = cosf(rot_a), rot_sa = sinf(rot_a);
    auto rot_pt = [&](ImVec2 p) -> ImVec2 {
        if (!rotate) return p;
        float dx = p.x - center.x, dy = p.y - center.y;
        return ImVec2(center.x + dx * rot_ca - dy * rot_sa,
                      center.y + dx * rot_sa + dy * rot_ca);
    };

    if (!skeleton && g_overlay.mosaic.resource) {
        if (rotate) {
            draw_mosaic_rotated(dl, square, center, r, p_min, p_max, size,
                                view, tint_mosaic, rot_ca, rot_sa);
        } else if (square) {
            dl->AddImage((ImTextureID)g_overlay.mosaic.srv_gpu.ptr,
                         p_min, p_max, view.uv0, view.uv1, tint_mosaic);
        } else {
            dl->AddImageRounded((ImTextureID)g_overlay.mosaic.srv_gpu.ptr,
                                p_min, p_max, view.uv0, view.uv1,
                                tint_mosaic, r);
        }
    } else {
        if (square) {
            dl->AddRectFilled(p_min, p_max,
                              scale_alpha(IM_COL32(20, 22, 30, 255)));
        } else {
            dl->AddCircleFilled(center, r,
                                scale_alpha(IM_COL32(20, 22, 30, 255)), 64);
        }
    }
    if (square) {
        dl->AddRect(p_min, p_max,
                    scale_alpha(kColBezel),       0.0f, 0, 3.0f);
        dl->AddRect({p_min.x + 2.0f, p_min.y + 2.0f},
                    {p_max.x - 2.0f, p_max.y - 2.0f},
                    scale_alpha(kColBezelShadow), 0.0f, 0, 1.0f);
    } else {
        dl->AddCircle(center, r,        scale_alpha(kColBezel),       96, 3.0f);
        dl->AddCircle(center, r - 2.0f, scale_alpha(kColBezelShadow), 96, 1.0f);
    }
    // North tick. North is "up" only in north-up mode; when rotating it
    // moves around the rim to point at true north.
    dl->AddLine(rot_pt({center.x, p_min.y}),
                rot_pt({center.x, p_min.y + 10.0f}),
                kColNorth, 2.5f);

    // POIs (clipped to the bezel disc). Skipped entirely in skeleton
    // mode (issue #12) so the post-transition draw call count stays
    // tiny while the game's DX12 driver settles.
    if (!skeleton) {
        const auto& pois = pois_get();
        const float clip_r  = r - 4.0f;
        const float clip_r2 = clip_r * clip_r;

        // World-space pre-cull radius: skip POIs far outside the visible
        // area BEFORE the (more expensive) projection + rotation, so the
        // per-frame loop scales with the visible subset instead of all
        // ~800 markers. Conservative (3x the half-view, covers the
        // edge-clamped case + corners) so nothing visible is dropped; the
        // exact screen clip below stays the final test. Centered on the
        // smoothed player position to match the view.
        double cull_r2 = 1e30;   // default: no cull (whole map visible)
        {
            float vsize = 1.0f / g_calib.zoom;   // visible fraction of mosaic
            if (vsize < 1.0f) {
                float half_px = kFullMosaicPx * vsize * 0.5f;
                double wppx = (g_calib.world_to_full_x_scale != 0.0f)
                    ? 1.0 / std::fabs((double)g_calib.world_to_full_x_scale)
                    : 0.0;
                double wppy = (g_calib.world_to_full_y_scale != 0.0f)
                    ? 1.0 / std::fabs((double)g_calib.world_to_full_y_scale)
                    : 0.0;
                double half_world =
                    (double)half_px * (wppx > wppy ? wppx : wppy);
                double cull_r = half_world * 3.0 + 50.0;
                cull_r2 = cull_r * cull_r;
            }
        }
        const float icon_size =
            (size <= 256.0f) ? 9.0f : (size <= 384.0f) ? 11.0f : 13.0f;
        const float shape_size = icon_size * 0.45f;
        ImTextureID atlas = g_overlay.poi_atlas.resource
            ? (ImTextureID)g_overlay.poi_atlas.srv_gpu.ptr : (ImTextureID)0;
        // Hover scaling: any POI within this many pixels of the mouse
        // grows ~80% so it's much easier to right-click. Only applies
        // while the cursor is over the compass disc.
        ImVec2 mouse_pos     = ImGui::GetMousePos();
        bool   compass_hover = ImGui::IsMouseHoveringRect(p_min, p_max);
        const float kHoverRadius = 16.0f;

        // Track candidates for right-click hit-testing below.
        struct ClickCand { ImVec2 sp; const PoiRow* p; };
        std::vector<ClickCand> click_cands;
        click_cands.reserve(pois.size() / 4);

        // v0.5.5: rectangular POI clip when square minimap is on. Keep
        // a 4-pixel inset matching the disc's clip_r so markers don't
        // touch the bezel edge.
        const float rect_inset = 4.0f;
        const float rx_lo = p_min.x + rect_inset;
        const float rx_hi = p_max.x - rect_inset;
        const float ry_lo = p_min.y + rect_inset;
        const float ry_hi = p_max.y - rect_inset;
        for (const auto& poi : pois) {
            // World-space pre-cull first (2 mults): the cheapest reject,
            // so the category filter + projection only run on the
            // visible subset, not all ~800 markers.
            double cdx = poi.x - sm_x, cdy = poi.y - sm_y;
            if (cdx * cdx + cdy * cdy > cull_r2) continue;
            if (!poi_passes_filter(poi)) continue;
            ImVec2 sp_local = player_to_screen(poi.x, poi.y, view, size);
            ImVec2 sp(p_min.x + sp_local.x, p_min.y + sp_local.y);
            sp = rot_pt(sp);   // heading-up: rotate marker position
            if (square) {
                if (sp.x < rx_lo || sp.x > rx_hi ||
                    sp.y < ry_lo || sp.y > ry_hi) continue;
            } else {
                float ddx = sp.x - center.x, ddy = sp.y - center.y;
                if (ddx * ddx + ddy * ddy > clip_r2) continue;
            }

            // User-marked-done collectibles are dimmed (alpha-tinted)
            // rather than hidden so the user always sees the total
            // layout. Right-click toggles below.
            bool is_done = poi.id[0] && poi_progress_is_done(poi.id);

            // Hover test: enlarge on hover for easier targeting.
            float hover_scale = 1.0f;
            if (compass_hover) {
                float mdx = sp.x - mouse_pos.x;
                float mdy = sp.y - mouse_pos.y;
                if (mdx * mdx + mdy * mdy < kHoverRadius * kHoverRadius)
                    hover_scale = 1.8f;
            }

            const float icon_sz  = icon_size  * hover_scale;
            const float shape_sz = shape_size * hover_scale;

            // Per-kind drawing path:
            // 1) Collectible (chest/red_orb/plant/ore): custom glyph
            //    drawn at the kind's signature color, alpha-dimmed
            //    if marked done.
            // 2) Activity / dungeon / obelisk etc: atlas-icon if a
            //    UV mapping exists, else fall back to shape marker.
            ImU32 fill = pois_style(poi).color;
            if (is_done) {
                std::uint32_t a = (fill >> 24) & 0xff;
                a = (a * 70) / 255;
                fill = (fill & 0x00ffffff) | (a << 24);
            }
            if (pois_draw_collectible(dl, sp, shape_sz, poi.kind, fill)) {
                // done - collectible glyph drawn
            } else {
                ImU32 tint = is_done ? IM_COL32(255, 255, 255, 70)
                                     : IM_COL32_WHITE;
                ImVec2 uv0, uv1;
                if (atlas && pois_atlas_uv(poi, uv0, uv1)) {
                    ImVec2 a(sp.x - icon_sz, sp.y - icon_sz);
                    ImVec2 b(sp.x + icon_sz, sp.y + icon_sz);
                    dl->AddImage(atlas, a, b, uv0, uv1, tint);
                } else {
                    PoiStyle st = pois_style(poi);
                    st.color = fill;
                    pois_draw_marker(dl, sp, st, shape_sz);
                }
            }

            // Right-click target: collectibles and activities (#78) can be
            // toggled done -> dimmed, not story POIs (dungeon, obelisk,
            // merchant ...). The done state + dimming are generic by id.
            if (std::strcmp(poi.kind, "chest")    == 0 ||
                std::strcmp(poi.kind, "red_orb")  == 0 ||
                std::strcmp(poi.kind, "activity") == 0) {
                click_cands.push_back({sp, &poi});
            }
        }

        // Right-click handling: priority 1 is bezel-button drag (so
        // the user can rearrange the rim layout), priority 2 is the
        // collectible-toggle. Drag is only enabled while UI is
        // unlocked - locking pins the layout in place.
        bool right_consumed = false;
        if (!g_ui_locked.load()) {
            // Pointer to each angle in g_bezel + button positions we
            // already computed via bezel_hit, ordered to match.
            float* angles[] = { &g_bezel.pin, &g_bezel.size,
                                &g_bezel.collapse, &g_bezel.filter,
                                &g_bezel.lock,    &g_bezel.keys,
                                &g_bezel.chest,   &g_bezel.plus,
                                &g_bezel.minus,   &g_bezel.overview };
            ImVec2 btn_pos[] = { pin.center, sizeb.center, collapse.center,
                                 filter.center, lockb.center, keys.center,
                                 chest.center, plus.center, minus.center,
                                 overview.center };
            constexpr int kNumBezel = 10;

            if (ImGui::IsMouseHoveringRect(p_min, p_max) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                g_bezel_drag < 0) {
                ImVec2 m = ImGui::GetMousePos();
                for (int i = 0; i < kNumBezel; ++i) {
                    float dx = btn_pos[i].x - m.x;
                    float dy = btn_pos[i].y - m.y;
                    if (dx * dx + dy * dy < btn_r * btn_r) {
                        g_bezel_drag = i;
                        right_consumed = true;
                        break;
                    }
                }
            }

            if (g_bezel_drag >= 0) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    ImVec2 m = ImGui::GetMousePos();
                    float a = std::atan2f(m.y - center.y, m.x - center.x);
                    *angles[g_bezel_drag] = a;
                    right_consumed = true;
                } else {
                    g_bezel_drag = -1;
                    ui_lock_save();   // persists locked + bezel angles
                }
            }
        }

        // Right-click on a collectible POI -> toggle done. Skipped if
        // a bezel drag just started on this same right-click.
        // #86: collectibles can sit on top of each other (red orbs stacked on
        // the same map column project onto nearly the same minimap pixel), so
        // the nearest-only hit-test made them impossible to mark individually.
        // A right-click now marks EVERY candidate within the click radius. The
        // target state is "done" unless the whole cluster is already done, in
        // which case it un-marks (so a second right-click clears the cluster).
        bool collectible_hit = false;
        if (!right_consumed &&
            ImGui::IsMouseHoveringRect(p_min, p_max) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImVec2 m = ImGui::GetMousePos();
            const float kClickR2 = 14.0f * 14.0f;
            std::vector<const char*> hit_ids;
            bool all_done = true;
            for (const auto& c : click_cands) {
                float dx = c.sp.x - m.x, dy = c.sp.y - m.y;
                if (dx * dx + dy * dy >= kClickR2) continue;
                if (!c.p->id[0]) continue;
                hit_ids.push_back(c.p->id);
                if (!poi_progress_is_done(c.p->id)) all_done = false;
            }
            if (!hit_ids.empty()) {
                collectible_hit = true;
                const bool target = !all_done;   // mark done unless all done
                poi_progress_set_ids(hit_ids.data(),
                                     static_cast<int>(hit_ids.size()), target);
                logf("poi_progress: right-click marked %zu collectible(s) -> %s",
                     hit_ids.size(), target ? "done" : "not done");
            }
        }

        // --- user waypoints (#58) -------------------------------------
        const auto& wps = waypoints_get();
        const float wp_size =
            (size <= 256.0f) ? 7.0f : (size <= 384.0f) ? 9.0f : 11.0f;
        for (const auto& wp : wps) {
            ImVec2 sp_local = player_to_screen(wp.x, wp.y, view, size);
            ImVec2 sp(p_min.x + sp_local.x, p_min.y + sp_local.y);
            sp = rot_pt(sp);
            if (square) {
                if (sp.x < rx_lo || sp.x > rx_hi ||
                    sp.y < ry_lo || sp.y > ry_hi) continue;
            } else {
                float ddx = sp.x - center.x, ddy = sp.y - center.y;
                if (ddx * ddx + ddy * ddy > clip_r2) continue;
            }
            bool hov = false;
            if (compass_hover) {
                float mdx = sp.x - mouse_pos.x, mdy = sp.y - mouse_pos.y;
                hov = (mdx * mdx + mdy * mdy < kHoverRadius * kHoverRadius);
            }
            // Primary "target" waypoint pulses a halo ring underneath.
            if (wp.id == waypoints_get_primary()) {
                float t = (float)ImGui::GetTime();
                float pulse = 0.55f + 0.45f * std::sinf(t * 4.0f);
                ImU32 halo = IM_COL32(255, 230, 120, (int)(180 * pulse));
                dl->AddCircle(ImVec2(sp.x, sp.y - wp_size),
                              wp_size * 1.4f, halo, 18, 2.0f);
            }
            draw_waypoint(dl, sp, wp_size, hov, wp.color, wp.icon);
            if (hov) {
                ImVec2 ts = ImGui::CalcTextSize(wp.name);
                ImVec2 lp(sp.x + wp_size * 1.2f, sp.y - wp_size * 2.0f);
                dl->AddRectFilled(
                    ImVec2(lp.x - 3.0f, lp.y - 2.0f),
                    ImVec2(lp.x + ts.x + 3.0f, lp.y + ts.y + 2.0f),
                    IM_COL32(0, 0, 0, 200), 3.0f);
                dl->AddText(lp, IM_COL32(220, 240, 255, 255), wp.name);
            }
        }

        // ---- party members ------------------------------------------
        if (g_party_visible.load()) {
            const PartySnapshot& psnap = party_state_read();
            const float party_size =
                (size <= 256.0f) ? 6.0f : (size <= 384.0f) ? 7.0f : 9.0f;
            const ImU32 party_fill    = IM_COL32(80, 220, 100, 255);
            const ImU32 party_outline = IM_COL32(0, 0, 0, 230);
            for (int i = 0; i < psnap.count; ++i) {
                const PartyMember& pmem = psnap.members[i];
                if (!pmem.hero_valid) continue;
                ImVec2 sp_local = player_to_screen((float)pmem.x, (float)pmem.y,
                                                    view, size);
                ImVec2 sp(p_min.x + sp_local.x, p_min.y + sp_local.y);
                sp = rot_pt(sp);
                sp = clamp_to_minimap(sp, center, clip_r, square,
                                      p_min, p_max);
                // + rot_a, matching rot_pt: a party member's arrow is a
                // world angle and turns with the map. This was a minus,
                // which pointed party arrows the wrong way (mirrored about
                // the map's up axis) whenever rotation was on.
                float heading = (float)pmem.rot_z + rot_a;
                draw_party_arrow(dl, sp, party_size, heading,
                                  party_fill, party_outline);
                draw_party_label(dl, sp, -party_size * 2.2f, pmem.name);
                if (compass_hover) {
                    float mdx = sp.x - mouse_pos.x;
                    float mdy = sp.y - mouse_pos.y;
                    if (mdx * mdx + mdy * mdy < kHoverRadius * kHoverRadius) {
                        double dxw = pmem.x - h.x, dyw = pmem.y - h.y;
                        double dist = std::sqrt(dxw * dxw + dyw * dyw);
                        ImGui::SetTooltip("%s  -  %.0f m", pmem.name, dist);
                    }
                }
            }
        }

        // ---- rare-mob alert markers (#65) ---------------------------
        {
            const MobWatchSnapshot& msnap = mob_watch_read();
            if (msnap.enabled && msnap.count > 0) {
                float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
                const float mob_size =
                    ((size <= 256.0f) ? 5.5f : (size <= 384.0f) ? 6.5f : 8.0f)
                    * (0.85f + 0.5f * pulse);
                const ImU32 mob_fill = IM_COL32(255, 90, 60, 255);
                const ImU32 mob_out  = IM_COL32(0, 0, 0, 230);
                for (int i = 0; i < msnap.count; ++i) {
                    const MobHit& mh = msnap.hits[i];
                    ImVec2 sl = player_to_screen((float)mh.x, (float)mh.y,
                                                  view, size);
                    ImVec2 sp(p_min.x + sl.x, p_min.y + sl.y);
                    sp = rot_pt(sp);
                    sp = clamp_to_minimap(sp, center, clip_r, square,
                                          p_min, p_max);
                    dl->AddCircleFilled(sp, mob_size, mob_fill);
                    dl->AddCircle(sp, mob_size, mob_out, 0, 1.5f);
                    if (compass_hover) {
                        float mdx = sp.x - mouse_pos.x, mdy = sp.y - mouse_pos.y;
                        if (mdx * mdx + mdy * mdy < kHoverRadius * kHoverRadius)
                            ImGui::SetTooltip("%s  -  %.0f m", mh.kind, mh.dist);
                    }
                }
            }
        }

        // Right-click context menu: only if neither a bezel drag nor a
        // collectible toggle consumed this click. #58
        if (!right_consumed && !collectible_hit &&
            ImGui::IsMouseHoveringRect(p_min, p_max) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImVec2 m = ImGui::GetMousePos();
            float best = 14.0f * 14.0f;
            int hit_id = -1;
            for (const auto& wp : wps) {
                ImVec2 l = player_to_screen(wp.x, wp.y, view, size);
                ImVec2 sp = rot_pt(ImVec2(p_min.x + l.x, p_min.y + l.y));
                float dx = sp.x - m.x, dy = sp.y - m.y;
                float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; hit_id = wp.id; }
            }
            g_wp_ctx_id = hit_id;
            screen_to_world(m, center, p_min, view, size,
                            rotate, rot_ca, rot_sa, g_wp_ctx_wx, g_wp_ctx_wy);
            g_wp_ctx_wz = (float)h.z;   // 2D map: use the player's current z
            ImGui::OpenPopup("##wp_ctx");
        }

        if (ImGui::BeginPopup("##wp_ctx")) {
            if (g_wp_ctx_id >= 0) {
                // Look up the current style for the highlighted waypoint.
                uint8_t cur_color = 0, cur_icon = 0;
                for (const auto& wp : wps)
                    if (wp.id == g_wp_ctx_id) {
                        cur_color = wp.color;
                        cur_icon  = wp.icon;
                        break;
                    }

                // Color row: 8 ColorButtons, 14x14 each. Click applies
                // immediately via waypoints_set_style.
                for (int i = 0; i < 8; ++i) {
                    ImGui::PushID(i);
                    ImVec4 col = ImGui::ColorConvertU32ToFloat4(kWpPalette[i]);
                    bool is_cur = (cur_color == (uint8_t)i);
                    if (is_cur)
                        ImGui::PushStyleColor(ImGuiCol_Border,
                                              IM_COL32(255, 255, 255, 255));
                    ImGuiColorEditFlags flags =
                        ImGuiColorEditFlags_NoTooltip |
                        ImGuiColorEditFlags_NoDragDrop;
                    if (ImGui::ColorButton("##c", col, flags, ImVec2(14, 14)))
                        waypoints_set_style(g_wp_ctx_id, (uint8_t)i, cur_icon);
                    if (is_cur) ImGui::PopStyleColor();
                    if (i < 7) ImGui::SameLine(0.0f, 2.0f);
                    ImGui::PopID();
                }

                // Icon row: 6 cells, each rendering the actual icon in the
                // currently-selected color.
                for (int i = 0; i < 6; ++i) {
                    bool is_cur = (cur_icon == (uint8_t)i);
                    char idbuf[8];
                    std::snprintf(idbuf, sizeof(idbuf), "i%d", i);
                    if (wp_icon_button(idbuf, (uint8_t)i, cur_color, is_cur))
                        waypoints_set_style(g_wp_ctx_id, cur_color, (uint8_t)i);
                    if (i < 5) ImGui::SameLine(0.0f, 2.0f);
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Rename")) {
                    g_wp_rename_id = g_wp_ctx_id;
                    g_wp_rename_buf[0] = '\0';
                    for (const auto& wp : wps)
                        if (wp.id == g_wp_ctx_id) {
                            std::strncpy(g_wp_rename_buf, wp.name,
                                         sizeof(g_wp_rename_buf) - 1);
                            g_wp_rename_buf[sizeof(g_wp_rename_buf) - 1] = '\0';
                            break;
                        }
                    g_wp_want_rename = true;   // opened at window scope below
                    ImGui::CloseCurrentPopup();
                }
                bool is_primary = (waypoints_get_primary() == g_wp_ctx_id);
                if (ImGui::MenuItem(is_primary ? "Clear target" : "Set as target")) {
                    waypoints_set_primary(is_primary ? 0 : g_wp_ctx_id);
                }
                if (ImGui::MenuItem("Delete")) {
                    waypoints_remove(g_wp_ctx_id);
                }
            } else {
                bool can_add = wps.size() < kMaxWaypoints;
                if (ImGui::MenuItem("Add waypoint here", nullptr, false, can_add)) {
                    waypoints_add((float)g_wp_ctx_wx, (float)g_wp_ctx_wy,
                                  g_wp_ctx_wz, "Waypoint");
                }
            }
            ImGui::EndPopup();
        }

        // Open the rename popup at window scope so its ID matches the
        // BeginPopup below (OpenPopup hashes against the current ID stack).
        if (g_wp_want_rename) {
            ImGui::OpenPopup("##wp_rename");
            g_wp_want_rename = false;
        }

        // Rename modal, opened from the Rename menu item above.
        if (ImGui::BeginPopup("##wp_rename")) {
            ImGui::TextUnformatted("Rename waypoint");
            ImGui::SetNextItemWidth(180.0f);
            bool enter = ImGui::InputText("##wp_name", g_wp_rename_buf,
                                          sizeof(g_wp_rename_buf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("OK") || enter) {
                if (g_wp_rename_id >= 0)
                    waypoints_rename(g_wp_rename_id, g_wp_rename_buf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    if (!h.locked) {
        dl->AddText({center.x - 60.0f, center.y - 6.0f}, kColText,
                    "Waiting for Hero alloc...");
    } else {
        ImVec2 dot_local = player_to_screen(sm_x, sm_y, view, size);
        ImVec2 dot(p_min.x + dot_local.x, p_min.y + dot_local.y);
        dot = rot_pt(dot);   // stays on the player when panning at map edges
        // The arrow is a world-anchored angle like any other, so it gets the
        // same +rot_a the marker positions get. Rotating to the player's
        // facing that lands on straight up, exactly as before; rotating to
        // the camera it shows the facing relative to the camera instead.
        float aang = rotate ? (sm_heading + rot_a) : sm_heading;
        float c = cosf(aang), s = sinf(aang);
        float arr =
            (size <= 256.0f) ? 9.0f : (size <= 384.0f) ? 11.0f : 13.0f;
        if (g_overlay.player_arrow.resource) {
            auto rot = [&](float lx, float ly) {
                return ImVec2(dot.x + lx * c - ly * s,
                              dot.y + lx * s + ly * c);
            };
            ImVec2 p0 = rot(-arr, -arr), p1 = rot(arr, -arr);
            ImVec2 p2 = rot( arr,  arr), p3 = rot(-arr,  arr);
            dl->AddImageQuad(
                (ImTextureID)g_overlay.player_arrow.srv_gpu.ptr,
                p0, p1, p2, p3,
                ImVec2(0, 0), ImVec2(1, 0),
                ImVec2(1, 1), ImVec2(0, 1));
        } else {
            dl->AddLine(dot, {dot.x + c * 14.0f, dot.y + s * 14.0f},
                        kColPlayer, 2.0f);
            dl->AddCircleFilled(dot, 5.0f, kColPlayer, 16);
        }
    }

    // "Any collectible category on" reflects the chest button's lit state.
    bool any_collectible_on = g_filter.chests || g_filter.red_orbs ||
                              g_filter.plants || g_filter.ores;

    // #254: guide arrow to the nearest uncollected collectible. One pass
    // over the loaded POIs (only when collectibles are shown + the guide is
    // on) for the nearest not-done collectible of an enabled category, then
    // a rim arrow pointing its way with the distance. The direction reuses
    // the same projection + heading-up rotation as the POI markers, so it
    // stays correct even when the target is off the visible map.
    if (g_guide_nearest && any_collectible_on && h.locked && !skeleton) {
        const PoiRow* nearest = nullptr;
        double best_d2 = 1e30;
        for (const auto& p : pois_get()) {
            bool on = (p.cat == PoiCat::Chest  && g_filter.chests)  ||
                      (p.cat == PoiCat::RedOrb && g_filter.red_orbs) ||
                      (p.cat == PoiCat::Plant  && g_filter.plants)   ||
                      (p.cat == PoiCat::Ore    && g_filter.ores);
            if (!on) continue;
            if (poi_progress_is_done(p.id)) continue;
            double ddx = p.x - sm_x, ddy = p.y - sm_y;
            double d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) { best_d2 = d2; nearest = &p; }
        }
        if (nearest) {
            ImVec2 sl = player_to_screen(nearest->x, nearest->y, view, size);
            ImVec2 sp = rot_pt(ImVec2(p_min.x + sl.x, p_min.y + sl.y));
            float dx = sp.x - center.x, dy = sp.y - center.y;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) {
                float ux = dx / len, uy = dy / len;
                float rr = r * 0.82f;            // inside the bezel button ring
                ImVec2 tip(center.x + ux * rr, center.y + uy * rr);
                float a = 9.0f;
                ImVec2 px(-uy, ux);              // perpendicular
                ImVec2 t0(tip.x + ux * a,                  tip.y + uy * a);
                ImVec2 t1(tip.x - ux * a * 0.5f + px.x * a * 0.6f,
                          tip.y - uy * a * 0.5f + px.y * a * 0.6f);
                ImVec2 t2(tip.x - ux * a * 0.5f - px.x * a * 0.6f,
                          tip.y - uy * a * 0.5f - px.y * a * 0.6f);
                const ImU32 col = IM_COL32(120, 230, 255, 235);
                dl->AddTriangleFilled(t0, t1, t2, col);
                dl->AddTriangle(t0, t1, t2, IM_COL32(10, 30, 45, 220), 1.2f);
                char dist[24];
                std::snprintf(dist, sizeof(dist), "%.0f m", std::sqrt(best_d2));
                ImVec2 ts = ImGui::CalcTextSize(dist);
                ImVec2 lp(center.x + ux * (rr - 16.0f) - ts.x * 0.5f,
                          center.y + uy * (rr - 16.0f) - ts.y * 0.5f);
                dl->AddText(ImVec2(lp.x + 1.0f, lp.y + 1.0f),
                            IM_COL32(0, 0, 0, 200), dist);
                dl->AddText(lp, col, dist);
            }
        }
    }

    bezel_draw_pin     (dl, pin);
    bezel_draw_size    (dl, sizeb, g_compass_size_idx);
    bezel_draw_collapse(dl, collapse);
    bezel_draw_filter  (dl, filter, g_filter_open);
    bezel_draw_lock    (dl, lockb,  g_ui_locked.load());
    bezel_draw_key     (dl, keys,   g_keys_open);
    bezel_draw_chest   (dl, chest,  any_collectible_on);
    bezel_draw_plus    (dl, plus);
    bezel_draw_minus   (dl, minus);
    bezel_draw_overview(dl, overview, g_overview_active);

    // #255: hover tooltips for the hand-drawn bezel buttons (discoverability,
    // especially the gear/filter and full-zone overview). Suppressed while a
    // button is being right-drag repositioned.
    if (g_bezel_drag < 0) {
        const char* tip = nullptr;
        if      (pin.hovered)      tip = "Drag to move the minimap";
        else if (sizeb.hovered)    tip = "Cycle minimap size";
        else if (collapse.hovered) tip = "Collapse to a puck";
        else if (filter.hovered)   tip = "Filters & options";
        else if (lockb.hovered)    tip = "Lock / unlock layout";
        else if (keys.hovered)     tip = "Hotkeys";
        else if (chest.hovered)    tip = "Toggle collectibles";
        else if (plus.hovered)     tip = "Zoom in";
        else if (minus.hovered)    tip = "Zoom out";
        else if (overview.hovered) tip = "Show full zone";
        if (tip) ImGui::SetTooltip("%s", tip);
    }

    // Manual zoom leaves the overview state (the user took over the zoom).
    // #256: persist the chosen zoom so it survives a restart.
    if (plus.clicked)  { g_calib.zoom += kZoomStep; if (g_calib.zoom > kZoomMax) g_calib.zoom = kZoomMax; g_overview_active = false; ui_lock_save(); }
    if (minus.clicked) { g_calib.zoom -= kZoomStep; if (g_calib.zoom < kZoomMin) g_calib.zoom = kZoomMin; g_overview_active = false; ui_lock_save(); }
    if (overview.clicked) {
        if (!g_overview_active) {
            // Jump out to the full-zone view, remembering the prior zoom.
            g_overview_saved_zoom = g_calib.zoom;
            g_calib.zoom          = kZoomOverview;
            g_overview_active     = true;
        } else {
            g_calib.zoom      = g_overview_saved_zoom;
            g_overview_active = false;
        }
        ui_lock_save();
    }
    if (sizeb.clicked) {
        g_compass_size_idx = (g_compass_size_idx + 1) % 3;
        ui_lock_save();   // persist compass size (issue #6)
    }
    if (filter.clicked)   g_filter_open      = !g_filter_open;
    if (lockb.clicked) {
        g_ui_locked.store(!g_ui_locked.load());
        ui_lock_save();
        logf("ui: locked = %d", (int)g_ui_locked.load());
    }
    if (keys.clicked)     g_keys_open        = !g_keys_open;
    if (chest.clicked) {
        // Quick toggle: if any category was on, turn them all off;
        // otherwise turn all four on at once.
        bool turn_on = !any_collectible_on;
        g_filter.chests   = turn_on;
        g_filter.red_orbs = turn_on;
        g_filter.plants   = turn_on;
        g_filter.ores     = turn_on;
        ui_lock_save();   // #47: persist filter selection
    }
    if (collapse.clicked) g_compass_collapsed = true;
    if (pin.active && !g_ui_locked.load()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (d.x != 0.0f || d.y != 0.0f) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImGui::SetWindowPos({wp.x + d.x, wp.y + d.y});
        }
    }
}

void render_minimap_window() {
    if (!g_minimap_visible.load()) return;
    HeroSnapshot h = hero_state_read();
    if (!h.locked) return;
    calib_maybe_reload();

    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 2));
    ImGui::SetNextWindowPos(ImVec2(600, 20), ImGuiCond_FirstUseEver);
    ImGui::Begin("minimap", nullptr, wflags);
    // v0.5.2.2 (issue #23): broadcast minimap-hovered state so the
    // wndproc can let RMB on the minimap reach ImGui (collectible
    // toggle + bezel reposition) instead of auto-CT'ing it as a
    // camera click. AllowWhenBlockedByActiveItem so the flag stays
    // true while a bezel drag is in progress.
    g_overlay_minimap_hovered.store(
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
        std::memory_order_release);
    // Off-screen rescue (issue #1): if the saved window position is
    // outside the current viewport, snap it back to a safe spot. Runs
    // every frame but only acts when the check trips.
    {
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        // #47: skip on the appearing frame / degenerate size. These
        // windows use AlwaysAutoResize, so GetWindowSize() is ~0 before
        // content is measured, which made the pos+size<slack edge test
        // false-trip for any window near the top/left and reset it.
        if (!ImGui::IsWindowAppearing() && ws.x > 32.0f && ws.y > 32.0f &&
            window_is_offscreen(wp, ws, vp)) {
            logf("overlay: minimap was off-screen at (%.0f,%.0f) "
                 "vp=(%.0f,%.0f), snapping back",
                 wp.x, wp.y, vp.x, vp.y);
            ImGui::SetWindowPos(ImVec2(40, 40));
        }
    }

    if (g_compass_collapsed) {
        const float puck = 36.0f;
        ImVec2 cmin = ImGui::GetCursorScreenPos();
        ImVec2 cc(cmin.x + puck * 0.5f, cmin.y + puck * 0.5f);
        ImGui::InvisibleButton("##puck", ImVec2(puck, puck));
        if (ImGui::IsItemActive() && !g_ui_locked.load()) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            if (d.x != 0.0f || d.y != 0.0f) {
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetWindowPos({wp.x + d.x, wp.y + d.y});
            }
        }
        if (ImGui::IsItemDeactivated()) {
            ImVec2 dd = ImGui::GetMouseDragDelta(0);
            if (std::fabs(dd.x) + std::fabs(dd.y) < 4.0f)
                g_compass_collapsed = false;
        }
        auto* dl = ImGui::GetWindowDrawList();
        float pr = puck * 0.5f;
        dl->AddCircleFilled(cc, pr, kColBtnFill, 32);
        dl->AddCircle(cc, pr, kColBezel, 32, 2.5f);
        float a = 5.0f;
        dl->AddQuad({cc.x, cc.y - a}, {cc.x + a, cc.y},
                    {cc.x, cc.y + a}, {cc.x - a, cc.y},
                    kColIcon, 1.8f);
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    render_compass(h);

    if (g_filter_open) {
        // #65: the options panel is its own movable top-level window (a
        // separate ImGui::Begin, not a child of the minimap), so it drags
        // anywhere like the Hotkeys window. The bezel filter button and the
        // window's X both toggle g_filter_open.
        ImGui::PushStyleColor(ImGuiCol_WindowBg,        window_bg_color());
        ImGui::PushStyleColor(ImGuiCol_Border,          kColBezel);
        ImGui::PushStyleColor(ImGuiCol_TitleBg,         kColBtnFill);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,   kColBtnFill);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,         IM_COL32(22, 16, 8, 235));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,       kColIcon);
        ImGui::PushStyleColor(ImGuiCol_Text,            kColText);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    4.0f);
        ImGui::SetNextWindowPos(ImVec2(360, 40), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags opt_flags = ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_AlwaysAutoResize;
        if (g_ui_locked.load())
            opt_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        if (ImGui::Begin("Options", &g_filter_open, opt_flags)) {
        bool filt_changed = false;
        filt_changed |= ImGui::Checkbox("Obelisks",   &g_filter.obelisks);
        filt_changed |= ImGui::Checkbox("Respawns",   &g_filter.respawns);
        filt_changed |= ImGui::Checkbox("Dungeons",   &g_filter.dungeons);
        filt_changed |= ImGui::Checkbox("Merchants",  &g_filter.merchants);
        filt_changed |= ImGui::Checkbox("Activities", &g_filter.activities);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Collectibles  (right-click on map to toggle)");
        auto labelled = [](const char* base, const char* kind,
                           char* out, std::size_t cap) {
            int done = 0, total = 0;
            poi_progress_counts(kind, &done, &total);
            std::snprintf(out, cap, "%s  %d/%d", base, done, total);
        };
        char lbl[64];
        labelled("Chests",   "chest",   lbl, sizeof(lbl));
        filt_changed |= ImGui::Checkbox(lbl, &g_filter.chests);
        labelled("Red Orbs", "red_orb", lbl, sizeof(lbl));
        filt_changed |= ImGui::Checkbox(lbl, &g_filter.red_orbs);
        // Plants / Ores respawn, so a "done X of Y" counter would be
        // misleading. Plain labels.
        filt_changed |= ImGui::Checkbox("Plants", &g_filter.plants);
        filt_changed |= ImGui::Checkbox("Ores",   &g_filter.ores);
        if (filt_changed) ui_lock_save();   // #47: persist filter selection

        // #65: bulk "mark all collected" for chests + red orbs. Non-destructive
        // (marking, not clearing); individual right-click on the map still
        // toggles any single one back. Only the currently loaded world's POIs.
        ImGui::Spacing();
        if (ImGui::SmallButton("Mark all collected")) {
            poi_progress_mark_all("chest",   true);
            poi_progress_mark_all("red_orb", true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(chests + orbs, this map)");

        // #254: arrow on the minimap rim pointing at the nearest not-yet-
        // collected collectible (enabled categories only). For hunting the
        // last chest/orb of an area.
        if (ImGui::Checkbox("Arrow to nearest uncollected", &g_guide_nearest))
            ui_lock_save();

        // v0.5.2.2 (issue #23 follow-up): show / hide the standalone
        // loot tracker window. Default on; users who do not care
        // about completion progress can turn it off here.
        bool loot_on = g_loot_counter_visible.load();
        if (ImGui::Checkbox("Show loot counter", &loot_on)) {
            g_loot_counter_visible.store(loot_on);
            ui_lock_save();
        }

        bool compass_on = g_compass_visible.load();
        if (ImGui::Checkbox("Show compass", &compass_on)) {
            g_compass_visible.store(compass_on);
            ui_lock_save();
        }

        bool party_on = g_party_visible.load();
        if (ImGui::Checkbox("Show party", &party_on)) {
            g_party_visible.store(party_on);
            ui_lock_save();
        }

        // #226: render-mode selector. Fast = render into the game swap chain;
        // Compatibility = our own DirectComposition layer. The backend is
        // bound at boot, so a change here only takes effect after a restart.
        // We reflect the backend running THIS session, then track the pending
        // pick locally so the radio shows the user's selection immediately.
        {
            static int  s_rmode  = (overlay_backend() == RenderBackend::GameSwapchain)
                                        ? 0 : 1;
            static bool s_rmode_changed = false;
            ImGui::TextUnformatted("Render mode (restart to apply):");
            if (ImGui::RadioButton("Fast##rmode", &s_rmode, 0)) {
                render_mode_save(true);
                s_rmode_changed = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Compatibility##rmode", &s_rmode, 1)) {
                render_mode_save(false);
                s_rmode_changed = true;
            }
            ImGui::TextDisabled("Turn OFF NVIDIA Smooth Motion / Frame Gen / "
                                "DLSS / Lossless Scaling - they can crash the game.");
            if (s_rmode_changed)
                ImGui::TextDisabled("Restart the game to apply.");
        }

        bool boss_on = g_boss_speedrun_enabled.load();
        if (ImGui::Checkbox("Boss speedrun mode", &boss_on)) {
            g_boss_speedrun_enabled.store(boss_on);
            boss_timer_set_enabled(boss_on);
            ui_lock_save();
        }
        ImGui::TextDisabled("Times boss fights, tracks your best per boss.");

        // #65: rare / sparkling-mob proximity alert. Beep + banner + map
        // marker when a foe whose name contains one of your watch terms comes
        // within range. Matching is on the internal mob name; the log lists the
        // exact names seen in range so you know what to type.
        {
            bool mob_on = mob_watch_enabled();
            if (ImGui::Checkbox("Rare mob alert", &mob_on))
                mob_watch_set_enabled(mob_on);
            ImGui::TextDisabled("Beep + banner + map marker for nearby watched mobs.");

            // #78: one-click toggle for the rare sparkling boss variants.
            bool spark_on = mob_watch_sparkling();
            if (ImGui::Checkbox("Sparkling bosses on map", &spark_on))
                mob_watch_set_sparkling(spark_on);
            ImGui::TextDisabled("Live marker for nearby sparkling boss spawns.");

            if (mob_on) {
                ImGui::Indent();
                float range = (float)mob_watch_range();
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat("Range##mob", &range, 20.0f, 400.0f, "%.0f m"))
                    mob_watch_set_range((double)range);

                static char s_mobterm[32] = "";
                ImGui::SetNextItemWidth(140.0f);
                bool entered = ImGui::InputText("##mobterm", s_mobterm,
                                                sizeof(s_mobterm),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::SmallButton("Add##mobterm") || entered) && s_mobterm[0]) {
                    mob_watch_add_term(s_mobterm);
                    s_mobterm[0] = 0;
                }
                ImGui::TextDisabled("Name contains, e.g. Buttontail.");
                ImGui::TextDisabled("Tip: with this on, nearby mob names are");
                ImGui::TextDisabled("written to farever-mod.log so you can copy them.");

                std::vector<std::string> terms = mob_watch_terms();
                if (terms.empty()) {
                    ImGui::TextDisabled("(no watch terms yet)");
                } else {
                    for (const auto& t : terms) {
                        char rm[48];
                        std::snprintf(rm, sizeof(rm), "x##rm_%s", t.c_str());
                        if (ImGui::SmallButton(rm)) mob_watch_remove_term(t.c_str());
                        ImGui::SameLine();
                        ImGui::TextUnformatted(t.c_str());
                    }
                }
                ImGui::Unindent();
            }
        }

        bool plugins_on = plugins_manager_visible();
        if (ImGui::Checkbox("Show plugin manager", &plugins_on)) {
            plugins_manager_toggle();
        }

        // Auto-theme community plugins to match the overlay look. On by
        // default; a plugin can still opt out with a `plugin_theme = false`
        // global.
        bool theme_on = g_plugin_theme.load();
        if (ImGui::Checkbox("Match plugin theme", &theme_on)) {
            g_plugin_theme.store(theme_on);
            ui_lock_save();
        }

        // v0.5.5: square minimap toggle. Off (default) = circular disc,
        // on = inscribed rectangle. Mosaic + bezel + POI clip switch
        // together; bezel buttons keep their ring layout either way.
        bool square_on = g_minimap_square.load();
        if (ImGui::Checkbox("Square minimap", &square_on)) {
            g_minimap_square.store(square_on);
            ui_lock_save();
        }

        // Heading-up rotation: map turns so the player's facing points
        // up. Off (default) = north-up.
        bool rotate_on = g_minimap_rotate.load();
        if (ImGui::Checkbox("Rotate with player", &rotate_on)) {
            g_minimap_rotate.store(rotate_on);
            ui_lock_save();
        }
        // #105: rotate to the camera instead of the player's facing. Only
        // meaningful while the rotation above is on, so it greys out with it.
        ImGui::BeginDisabled(!rotate_on);
        ImGui::Indent();
        bool rotate_cam = g_minimap_follow_camera.load();
        if (ImGui::Checkbox("...to the camera, not the player", &rotate_cam)) {
            g_minimap_follow_camera.store(rotate_cam);
            ui_lock_save();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Up on the map follows where the camera looks, matching the\n"
                "screen, instead of where your character faces. The player\n"
                "arrow then shows your facing relative to the camera.");
        }
        ImGui::Unindent();
        ImGui::EndDisabled();

        // Opacity slider (issue #2). Affects mosaic + bezel ring;
        // the player arrow, POI markers and bezel buttons stay full
        // alpha so they don't fade away. Persists on release.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Minimap opacity");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##minimap_alpha", &g_minimap_alpha,
                           0.30f, 1.00f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui_lock_save();
        }
        // v0.5.2: DPS / hotkeys / fight-detail window background opacity
        ImGui::TextDisabled("Window opacity");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##window_bg_alpha", &g_window_bg_alpha,
                           0.30f, 1.00f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui_lock_save();
        }
        // v0.5.3 issue #22: UI text scale. Useful on 4K + large display
        // setups where the default font is unreadably small. Applied
        // every frame via FontGlobalScale (see overlay_render). Bezel
        // icons keep their pixel size; this is text-only.
        ImGui::TextDisabled("UI scale (text)");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##ui_scale", &g_ui_scale,
                           0.50f, 2.50f, "%.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui_lock_save();
        }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(7);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void render_fight_detail_window() {
    if (g_selected_fight_id == 0) return;
    if (!hero_state_read().locked) return;

    const AggSnapshot& snap = aggregator_snapshot();
    const FightLogEntry* f = nullptr;
    for (std::size_t i = 0; i < snap.history_count; ++i) {
        if (snap.history[i].id == g_selected_fight_id) {
            f = &snap.history[i];
            break;
        }
    }
    if (!f) {
        // Selected fight rolled off the history ring.
        g_selected_fight_id = 0;
        return;
    }

    // Local hh:mm:ss from the saved unix-ms timestamp.
    std::int64_t ft100ns =
        f->ended_unix_ms * 10000LL + 116444736000000000LL;
    FILETIME ft;
    ft.dwLowDateTime  = (DWORD)(ft100ns & 0xffffffff);
    ft.dwHighDateTime = (DWORD)(ft100ns >> 32);
    SYSTEMTIME utc, lt;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &lt);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,         window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,           kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,          kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,             kColText);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,    IM_COL32(48, 36, 16, 240));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,       IM_COL32( 0,  0,  0, 60));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt,    IM_COL32( 0,  0,  0, 110));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColBezel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    char title[80];
    std::snprintf(title, sizeof(title),
                  "Fight #%d  %02u:%02u:%02u###fight_detail",
                  f->id, lt.wHour, lt.wMinute, lt.wSecond);

    ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_FirstUseEver);
    // Issue #5: default position used to be (80, 80) which overlapped
    // the DPS meter -- users couldn't see this was a separate window
    // they could drag. Bumped to (240, 240) so it's clearly its own
    // window the first time it opens.
    ImGui::SetNextWindowPos(ImVec2(240, 240), ImGuiCond_FirstUseEver);

    bool open = true;
    ImGuiWindowFlags fdetail_flags = ImGuiWindowFlags_NoScrollbar;
    if (g_ui_locked.load())
        fdetail_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (ImGui::Begin(title, &open, fdetail_flags)) {
        // Off-screen rescue (issue #1) - runs inside the Begin block
        // so we only touch ImGui state when the window actually exists.
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        // #47: skip on the appearing frame / degenerate size. These
        // windows use AlwaysAutoResize, so GetWindowSize() is ~0 before
        // content is measured, which made the pos+size<slack edge test
        // false-trip for any window near the top/left and reset it.
        if (!ImGui::IsWindowAppearing() && ws.x > 32.0f && ws.y > 32.0f &&
            window_is_offscreen(wp, ws, vp)) {
            logf("overlay: fight detail was off-screen, snapping back");
            ImGui::SetWindowPos(ImVec2(240, 240));
        }
        ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                           "duration %.1fs", f->duration_sec);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Text("total %.0f", f->total_damage);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                           "DPS %.1f", f->dps);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                           "HPS %.1f", f->hps);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.72f, 0.98f, 1.0f),
                           "SPS %.1f", f->sps);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::TextDisabled("%d hits", f->hit_count);

        // #57/#70: same DMG/HEAL/SHIELD toggle as the live meter, but with
        // its own state so flipping the live view doesn't disturb someone
        // looking at a sealed fight.
        int view = g_fight_detail_view.load();
        if (ImGui::SmallButton(view == MV_DMG ? "[DMG]" : "DMG")) {
            g_fight_detail_view.store(MV_DMG); ui_lock_save();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton(view == MV_HEAL ? "[HEAL]" : "HEAL")) {
            g_fight_detail_view.store(MV_HEAL); ui_lock_save();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton(view == MV_SHIELD ? "[SHIELD]" : "SHIELD")) {
            g_fight_detail_view.store(MV_SHIELD); ui_lock_save();
        }
        view = g_fight_detail_view.load();

        const SkillRow* view_rows  = view == MV_SHIELD ? f->shield_rows
                                   : view == MV_HEAL   ? f->heal_rows : f->rows;
        std::size_t     view_count = view == MV_SHIELD ? f->shield_row_count
                                   : view == MV_HEAL   ? f->heal_row_count
                                                       : f->row_count;
        double          view_total = view == MV_SHIELD ? f->total_shield
                                   : view == MV_HEAL   ? f->total_heal
                                                       : f->total_damage;
        const char*     rate_hdr   = view == MV_SHIELD ? "SPS"
                                   : view == MV_HEAL   ? "HPS" : "DPS";

        ImGui::Spacing();

        if (view != MV_DMG && view_count == 0) {
            ImGui::TextDisabled(view == MV_SHIELD ? "no shields in this fight"
                                                  : "no heals in this fight");
        } else
        {
        constexpr ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_SizingStretchProp |
            ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("##fight_detail_skills", 7, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Skill", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Hits",  ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Max",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("Crit%", ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableSetupColumn(rate_hdr, ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("%",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableHeadersRow();

            const float kIconPx = 22.0f;
            for (std::size_t i = 0; i < view_count; ++i) {
                const SkillRow& r = view_rows[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                SkillGfx sgfx{};
                LoadedTexture* atlas_tex = nullptr;
                if (skill_resolve_lookup(r.skill, &sgfx)) {
                    atlas_tex = get_or_load_atlas(sgfx.atlas_filename);
                }
                if (atlas_tex && atlas_tex->resource && sgfx.size > 0 &&
                    atlas_tex->width > 0 && atlas_tex->height > 0) {
                    float aw  = (float)atlas_tex->width;
                    float ah  = (float)atlas_tex->height;
                    float px0 = (float)(sgfx.x * sgfx.size);
                    float py0 = (float)(sgfx.y * sgfx.size);
                    float pw  = (float)(sgfx.width  * sgfx.size);
                    float ph  = (float)(sgfx.height * sgfx.size);
                    ImVec2 uv0(px0 / aw, py0 / ah);
                    ImVec2 uv1((px0 + pw) / aw, (py0 + ph) / ah);
                    ImGui::Image((ImTextureID)atlas_tex->srv_gpu.ptr,
                                 ImVec2(kIconPx, kIconPx), uv0, uv1);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", r.skill);
                } else {
                    ImGui::TextUnformatted(r.skill);
                }

                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.hit_count);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f", r.total);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.0f", r.max_hit);

                ImGui::TableSetColumnIndex(4);
                float crit_pct = r.hit_count > 0
                    ? 100.0f * (float)r.crit_count / (float)r.hit_count : 0.0f;
                if (crit_pct > 0.0f) {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                       "%.0f%%", crit_pct);
                } else {
                    ImGui::TextUnformatted("-");
                }

                ImGui::TableSetColumnIndex(5);
                double row_rate = r.total / f->duration_sec;
                ImGui::Text("%.1f", row_rate);

                ImGui::TableSetColumnIndex(6);
                float pct = view_total > 0.001
                    ? 100.0f * (float)(r.total / view_total) : 0.0f;
                ImGui::Text("%.0f%%", pct);
            }
            ImGui::EndTable();
        }
        }  // end !(view != MV_DMG && view_count == 0)
    }
    ImGui::End();
    if (!open) g_selected_fight_id = 0;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(10);
}

// Skyrim-style compass strip window. Horizontal bar that shows POIs and
// waypoints as icons positioned by their bearing relative to the player's
// facing. Separate window from the minimap; the two can be visible
// independently.
void render_compass_strip_window() {
    if (!g_compass_visible.load()) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,        kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColText);
    ImGui::PushStyleVar  (ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar  (ImGuiStyleVar_WindowBorderSize, 2.0f);

    ImGui::SetNextWindowSize(ImVec2(480, 56), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(720, 12), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse  |
        ImGuiWindowFlags_NoTitleBar;
    if (g_ui_locked.load())
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (ImGui::Begin("##compass_strip", nullptr, flags)) {
        constexpr float kPiF = 3.14159265358979323846f;
        const HeroSnapshot h = hero_state_read();

        // #65: recenter on the camera yaw by default (smooth, matches the
        // screen); fall back to the hero facing when the toggle is off or no
        // camera is anchored. project() below normalises any unbounded yaw.
        double cam_yaw = 0.0;
        const bool  cam_ok      = camera_state_yaw(cam_yaw);
        const float heading_ref = compass_heading_ref(
            g_compass_follow_camera.load(), cam_ok, cam_yaw, h.rot_z);

        // Strip geometry inside the window content area.
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 avail  = ImGui::GetContentRegionAvail();
        const float strip_w = avail.x;
        const float strip_h = avail.y > 40.0f ? avail.y : 40.0f;
        const float pad_x   = 16.0f;
        const float inner_l = origin.x + pad_x;
        const float inner_r = origin.x + strip_w - pad_x;
        const float center_x = (inner_l + inner_r) * 0.5f;
        const float half_w   = (inner_r - inner_l) * 0.5f;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background bar.
        dl->AddRectFilled(
            origin, ImVec2(origin.x + strip_w, origin.y + strip_h),
            IM_COL32(20, 14, 8, 150), 4.0f);
        // Baseline tick under the marker row.
        dl->AddLine(
            ImVec2(inner_l, origin.y + strip_h - 4.0f),
            ImVec2(inner_r, origin.y + strip_h - 4.0f),
            IM_COL32(120, 100, 60, 180), 1.0f);

        // Player tick at center: small upward triangle, gold.
        {
            float ty = origin.y + strip_h - 5.0f;
            float tx = center_x;
            dl->AddTriangleFilled(
                ImVec2(tx - 5.0f, ty),
                ImVec2(tx + 5.0f, ty),
                ImVec2(tx,        ty - 8.0f),
                IM_COL32(255, 210, 80, 255));
        }

        // Helper: project bearing -> (strip_x, alpha). Returns false if
        // outside the 180-deg FOV.
        auto project = [&](float rel_bearing, float& out_x, float& out_a) {
            while (rel_bearing >  kPiF) rel_bearing -= 2.0f * kPiF;
            while (rel_bearing < -kPiF) rel_bearing += 2.0f * kPiF;
            if (std::fabs(rel_bearing) > kPiF * 0.5f) return false;
            out_x = center_x + (rel_bearing / (kPiF * 0.5f)) * half_w;
            float t = (kPiF * 0.5f - std::fabs(rel_bearing)) / 0.15f;
            out_a = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return true;
        };

        // Build the candidate list: POIs that pass distance + filter +
        // FOV, plus waypoints (added in Task 6). Sorted by distance
        // descending so closer markers paint last (on top).
        struct Marker {
            float                       strip_x;
            float                       alpha;
            float                       distance;
            float                       dz;
            const PoiRow*               poi;
            const Waypoint*             wp;
            const CompassPluginMarker*  pm;
            const PartyMember*          party;
            const MobHit*               mob;
        };
        std::vector<Marker> markers;
        markers.reserve(64);

        if (h.locked) {
            const float radius  = g_compass_radius_m.load();
            const float radius2 = radius * radius;
            const float heading = heading_ref;
            const float px      = (float)h.x;
            const float py      = (float)h.y;

            const float pz = (float)h.z;
            const auto& pois = pois_get();
            for (const auto& poi : pois) {
                float dx = poi.x - px;
                float dy = poi.y - py;
                float d2 = dx * dx + dy * dy;
                if (d2 > radius2) continue;
                if (!poi_passes_filter(poi)) continue;
                // Items marked done on the minimap are hidden from the compass
                // strip to declutter it (they stay dimmed on the minimap).
                if (poi.id[0] && poi_progress_is_done(poi.id)) continue;
                float world_bearing = std::atan2f(dy, dx);
                float rel = world_bearing - heading;
                float rx, alpha;
                if (!project(rel, rx, alpha)) continue;
                markers.push_back({rx, alpha, std::sqrtf(d2),
                                    poi.z - pz, &poi, nullptr, nullptr, nullptr,
                                    nullptr});
            }

            const int primary_id = waypoints_get_primary();
            const auto& wps = waypoints_get();
            for (const auto& wp : wps) {
                float dx = wp.x - px;
                float dy = wp.y - py;
                float d2 = dx * dx + dy * dy;
                if (d2 > radius2) continue;
                float world_bearing = std::atan2f(dy, dx);
                float rel = world_bearing - heading;
                float rx, alpha;
                if (!project(rel, rx, alpha)) continue;
                // Primary stays fully visible even at the fade edges.
                if (wp.id == primary_id) alpha = 1.0f;
                markers.push_back({rx, alpha, std::sqrtf(d2),
                                    wp.z - pz, nullptr, &wp, nullptr, nullptr,
                                    nullptr});
            }

            // Plugin-supplied markers (farever.compass.add_marker).
            // Prune expired entries first so we never render stale state.
            double now = ImGui::GetTime();
            g_compass_plugin_markers.erase(
                std::remove_if(g_compass_plugin_markers.begin(),
                               g_compass_plugin_markers.end(),
                               [now](const CompassPluginMarker& pm) {
                                   return pm.expire_at != 0.0 && now > pm.expire_at;
                               }),
                g_compass_plugin_markers.end());
            for (const auto& pm : g_compass_plugin_markers) {
                float dx = pm.x - px;
                float dy = pm.y - py;
                float d2 = dx * dx + dy * dy;
                if (d2 > radius2) continue;
                float world_bearing = std::atan2f(dy, dx);
                float rel = world_bearing - heading;
                float rx, alpha;
                if (!project(rel, rx, alpha)) continue;
                markers.push_back({rx, alpha, std::sqrtf(d2),
                                    pm.z - pz, nullptr, nullptr, &pm, nullptr,
                                    nullptr});
            }

            // Party members: in-FOV ones go into the marker list; the
            // out-of-FOV ones get an edge-arrow pass after the loop.
            if (g_party_visible.load()) {
                const PartySnapshot& psnap = party_state_read();
                for (int i = 0; i < psnap.count; ++i) {
                    const PartyMember& pmem = psnap.members[i];
                    if (!pmem.hero_valid) continue;
                    float dx = (float)pmem.x - px;
                    float dy = (float)pmem.y - py;
                    float d2 = dx * dx + dy * dy;
                    float world_bearing = std::atan2f(dy, dx);
                    float rel = world_bearing - heading;
                    float rx, alpha;
                    if (project(rel, rx, alpha)) {
                        markers.push_back({rx, alpha, std::sqrtf(d2),
                                            (float)pmem.z - pz,
                                            nullptr, nullptr, nullptr,
                                            &pmem, nullptr});
                    }
                }
            }

            // Rare-mob alert markers on the strip (#65).
            {
                const MobWatchSnapshot& msnap = mob_watch_read();
                for (int i = 0; i < msnap.count; ++i) {
                    const MobHit& mh = msnap.hits[i];
                    float dx = (float)mh.x - px;
                    float dy = (float)mh.y - py;
                    float d2 = dx * dx + dy * dy;
                    float world_bearing = std::atan2f(dy, dx);
                    float rel = world_bearing - heading;
                    float rx, alpha;
                    if (project(rel, rx, alpha)) {
                        markers.push_back({rx, alpha, std::sqrtf(d2),
                                            (float)mh.z - pz,
                                            nullptr, nullptr, nullptr,
                                            nullptr, &mh});
                    }
                }
            }
        }

        std::sort(markers.begin(), markers.end(),
                  [](const Marker& a, const Marker& b) {
                      return a.distance > b.distance;
                  });

        // Skyrim-style tick band: tick marks every 15deg across the
        // strip top. Cardinals (N/E/S/W) get a taller tick + bold label;
        // intermediate cardinals (NE/SE/SW/NW) a medium tick + dim
        // label; all others a short tick only. World +y = SOUTH (the game's
        // latitude axis is inverted vs the compass), so the cardinals below
        // swap N<->S and the diagonals; E/W are unchanged.
        if (g_compass_show_cardinals.load()) {
            const float deg2rad = kPiF / 180.0f;
            const float heading = h.locked ? heading_ref : 0.0f;
            const float y_tick  = origin.y + 3.0f;

            for (int deg = 0; deg < 360; deg += 15) {
                float wb = (float)deg * deg2rad;   // [0, 2PI), project normalises
                float rx, alpha;
                if (!project(wb - heading, rx, alpha)) continue;

                const char* label = nullptr;
                bool bold = false;
                switch (deg) {
                    case 0:   label = "E";  bold = true;  break;
                    case 45:  label = "SE"; bold = false; break;
                    case 90:  label = "S";  bold = true;  break;
                    case 135: label = "SW"; bold = false; break;
                    case 180: label = "W";  bold = true;  break;
                    case 225: label = "NW"; bold = false; break;
                    case 270: label = "N";  bold = true;  break;
                    case 315: label = "NE"; bold = false; break;
                }

                float tick_h;
                ImU32 col;
                if (bold) {
                    tick_h = 9.0f;
                    col = IM_COL32(245, 225, 165, (int)(235 * alpha));
                } else if (label) {
                    tick_h = 7.0f;
                    col = IM_COL32(210, 190, 140, (int)(200 * alpha));
                } else {
                    tick_h = 4.0f;
                    col = IM_COL32(170, 150, 110, (int)(150 * alpha));
                }

                dl->AddLine(ImVec2(rx, y_tick),
                            ImVec2(rx, y_tick + tick_h), col, 1.2f);
                if (label) {
                    ImVec2 ts = ImGui::CalcTextSize(label);
                    dl->AddText(
                        ImVec2(rx - ts.x * 0.5f, y_tick + tick_h + 1.0f),
                        col, label);
                }
            }
        }

        // Draw markers, near-on-top order (already sorted far-first).
        ImTextureID poi_atlas = g_overlay.poi_atlas.resource
            ? (ImTextureID)g_overlay.poi_atlas.srv_gpu.ptr
            : (ImTextureID)0;
        const float marker_size = 18.0f;
        const float marker_y    = origin.y + strip_h * 0.5f + 4.0f;
        ImVec2 mouse = ImGui::GetMousePos();
        const Marker* hovered = nullptr;
        for (const Marker& m : markers) {
            ImVec2 pos(m.strip_x, marker_y);
            float dx = mouse.x - m.strip_x;
            float dy = mouse.y - marker_y;
            bool hit = std::fabs(dx) <= 9.0f && std::fabs(dy) <= 12.0f;
            if (hit) hovered = &m;

            if (m.poi) {
                ImVec2 uv0, uv1;
                if (poi_atlas && pois_atlas_uv(*m.poi, uv0, uv1)) {
                    pois_draw_atlas(dl, poi_atlas, pos, uv0, uv1, marker_size);
                } else {
                    ImU32 fill = IM_COL32(220, 200, 140,
                                          (int)(255 * m.alpha));
                    if (!pois_draw_collectible(dl, pos, marker_size * 0.6f,
                                               m.poi->kind, fill)) {
                        PoiStyle st = pois_style(*m.poi);
                        st.color = (st.color & 0x00FFFFFFu) |
                                   ((ImU32)(255 * m.alpha) << 24);
                        pois_draw_marker(dl, pos, st, marker_size * 0.6f);
                    }
                }
            } else if (m.wp) {
                ImVec2 tip(m.strip_x, origin.y + strip_h - 6.0f);
                draw_waypoint(dl, tip, 12.0f, hit, m.wp->color, m.wp->icon);
            } else if (m.pm) {
                ImVec2 tip(m.strip_x, origin.y + strip_h - 6.0f);
                draw_waypoint(dl, tip, 12.0f, hit, m.pm->color, m.pm->icon);
            } else if (m.party) {
                float arrow_heading = (float)(m.party->rot_z - h.rot_z);
                ImVec2 tip(m.strip_x, origin.y + strip_h - 12.0f);
                draw_party_arrow(dl, tip, 8.0f, arrow_heading,
                                  IM_COL32(80, 220, 100,
                                           (int)(255 * m.alpha)),
                                  IM_COL32(0, 0, 0,
                                           (int)(230 * m.alpha)));
                draw_party_label(dl, ImVec2(m.strip_x, marker_y),
                                  6.0f, m.party->name);
            } else if (m.mob) {
                // Rare-mob alert: pulsing red dot + name (#65).
                float pulse =
                    0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
                float r = (6.0f + 2.0f * pulse);
                dl->AddCircleFilled(pos, r,
                                    IM_COL32(255, 90, 60, (int)(255 * m.alpha)));
                dl->AddCircle(pos, r,
                              IM_COL32(0, 0, 0, (int)(230 * m.alpha)), 0, 1.5f);
                draw_party_label(dl, ImVec2(m.strip_x, marker_y),
                                  6.0f, m.mob->kind);
            }

            // Vertical indicator: small chevron above the icon if the
            // target is meaningfully above (blue) or below (red) the
            // player. <5 m is treated as "same height" -- no indicator.
            if (std::fabs(m.dz) > 5.0f) {
                bool up = m.dz > 0.0f;
                ImU32 zc = up
                    ? IM_COL32(120, 180, 255, (int)(220 * m.alpha))
                    : IM_COL32(255, 120, 120, (int)(220 * m.alpha));
                float cy = marker_y - 13.0f;
                if (up) {
                    dl->AddTriangleFilled(
                        ImVec2(m.strip_x,        cy - 4.0f),
                        ImVec2(m.strip_x - 4.0f, cy + 2.0f),
                        ImVec2(m.strip_x + 4.0f, cy + 2.0f),
                        zc);
                } else {
                    dl->AddTriangleFilled(
                        ImVec2(m.strip_x,        cy + 4.0f),
                        ImVec2(m.strip_x - 4.0f, cy - 2.0f),
                        ImVec2(m.strip_x + 4.0f, cy - 2.0f),
                        zc);
                }
            }
        }

        // Primary waypoint out-of-FOV edge arrow.
        if (h.locked) {
            int primary_id = waypoints_get_primary();
            if (primary_id != 0) {
                const auto& all_wps = waypoints_get();
                const Waypoint* p = nullptr;
                for (const auto& wp : all_wps)
                    if (wp.id == primary_id) { p = &wp; break; }
                if (p) {
                    float dx = p->x - (float)h.x;
                    float dy = p->y - (float)h.y;
                    float dist = std::sqrtf(dx * dx + dy * dy);
                    float world_b = std::atan2f(dy, dx);
                    float rel = world_b - (float)h.rot_z;
                    while (rel > kPiF)  rel -= 2.0f * kPiF;
                    while (rel < -kPiF) rel += 2.0f * kPiF;
                    if (std::fabs(rel) > kPiF * 0.5f) {
                        bool right_side = rel > 0.0f;
                        float ax = right_side ? inner_r - 4.0f : inner_l + 4.0f;
                        float ay = marker_y;
                        float t = (float)ImGui::GetTime();
                        float pulse = 0.55f + 0.45f * std::sinf(t * 4.0f);
                        ImU32 col = IM_COL32(255, 230, 120, (int)(255 * pulse));
                        float dir = right_side ? 1.0f : -1.0f;
                        dl->AddTriangleFilled(
                            ImVec2(ax + dir * 10.0f, ay),
                            ImVec2(ax, ay - 7.0f),
                            ImVec2(ax, ay + 7.0f),
                            col);
                        char dbuf[16];
                        std::snprintf(dbuf, sizeof(dbuf), "%.0fm", dist);
                        ImVec2 ts = ImGui::CalcTextSize(dbuf);
                        dl->AddText(
                            ImVec2(ax - ts.x * 0.5f, ay + 9.0f),
                            col, dbuf);
                    }
                }
            }
        }

        // Party members outside the 180-deg FOV get a small green
        // edge arrow + name on whichever rim they fall past.
        if (h.locked && g_party_visible.load()) {
            const PartySnapshot& psnap = party_state_read();
            for (int i = 0; i < psnap.count; ++i) {
                const PartyMember& pmem = psnap.members[i];
                if (!pmem.hero_valid) continue;
                float dx = (float)pmem.x - (float)h.x;
                float dy = (float)pmem.y - (float)h.y;
                float dist = std::sqrtf(dx * dx + dy * dy);
                float world_b = std::atan2f(dy, dx);
                float rel = world_b - (float)h.rot_z;
                while (rel >  kPiF) rel -= 2.0f * kPiF;
                while (rel < -kPiF) rel += 2.0f * kPiF;
                if (std::fabs(rel) <= kPiF * 0.5f) continue;   // in FOV
                bool right_side = rel > 0.0f;
                float ax = right_side ? inner_r - 4.0f : inner_l + 4.0f;
                float ay = marker_y;
                ImU32 col = IM_COL32(80, 220, 100, 230);
                float dir = right_side ? 1.0f : -1.0f;
                dl->AddTriangleFilled(
                    ImVec2(ax + dir * 8.0f, ay),
                    ImVec2(ax, ay - 5.0f),
                    ImVec2(ax, ay + 5.0f),
                    col);
                char lbl[80];
                std::snprintf(lbl, sizeof(lbl), "%s %.0fm",
                              pmem.name, dist);
                ImVec2 ts = ImGui::CalcTextSize(lbl);
                dl->AddText(
                    ImVec2(ax - ts.x * 0.5f, ay + 9.0f),
                    col, lbl);
            }
        }

        if (hovered) {
            const char* name = hovered->poi
                ? (hovered->poi->name[0] ? hovered->poi->name : hovered->poi->id)
                : (hovered->wp ? hovered->wp->name
                                : (hovered->pm ? hovered->pm->name
                                               : (hovered->party ? hovered->party->name
                                                                  : (hovered->mob ? hovered->mob->kind
                                                                                   : "?"))));
            if (std::fabs(hovered->dz) > 1.0f) {
                const char* dir = hovered->dz > 0.0f ? "up" : "down";
                ImGui::SetTooltip("%s  -  %.0f m  -  %s %.0f m",
                                  name, hovered->distance,
                                  dir, std::fabs(hovered->dz));
            } else {
                ImGui::SetTooltip("%s  -  %.0f m", name, hovered->distance);
            }
        }

        ImGui::Dummy(ImVec2(strip_w, strip_h));

        if (ImGui::TreeNodeEx("Settings", 0)) {
            float radius = g_compass_radius_m.load();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("Radius", &radius,
                                   0.0f, 500.0f, "%.0f m")) {
                g_compass_radius_m.store(radius);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) ui_lock_save();

            bool show_card = g_compass_show_cardinals.load();
            if (ImGui::Checkbox("Show cardinals", &show_card)) {
                g_compass_show_cardinals.store(show_card);
                ui_lock_save();
            }

            bool follow_cam = g_compass_follow_camera.load();
            if (ImGui::Checkbox("Compass follows camera", &follow_cam)) {
                g_compass_follow_camera.store(follow_cam);
                ui_lock_save();
            }

            ImGui::TextDisabled(
                "Type filters are shared with the minimap.");
            ImGui::TreePop();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}

// Boss speedrun panel (opt-in). Own movable window like the loot tracker; only
// drawn when the mode is on. Reads boss_timer_snapshot() only (no game memory).
void render_boss_timer_window() {
    if (!boss_timer_enabled()) return;
    BossTimerSnapshot s = boss_timer_snapshot();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,   kColBezel);
    ImGui::PushStyleColor(ImGuiCol_Text,     kColText);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);

    ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos (ImVec2(40, 360), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoTitleBar;
    if (g_ui_locked.load())
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;

    if (ImGui::Begin("Boss Timer", nullptr, flags)) {
        if (s.status == BossRunStatus::Running) {
            ImGui::TextUnformatted(
                s.current_boss.empty() ? "Boss" : s.current_boss.c_str());
            ImGui::Separator();
            ImGui::Text("%s", format_run_time(s.elapsed_s).c_str());
            if (s.has_pb) {
                ImGui::Text("PB  %s", format_run_time(s.current_pb_s).c_str());
                double delta = s.elapsed_s - s.current_pb_s;
                ImU32 col = (delta <= 0.0) ? IM_COL32(120, 230, 120, 255)
                                           : IM_COL32(235, 120, 110, 255);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::Text("%+.2f", delta);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("no PB yet");
            }
        } else {
            ImGui::TextUnformatted("Boss Timer");
            if (s.session_kills > 0) {
                ImGui::SameLine();
                if (s.session_fastest_s > 0.0)
                    ImGui::TextDisabled("  (session: %d, fastest %s)",
                        s.session_kills,
                        format_run_time(s.session_fastest_s).c_str());
                else
                    ImGui::TextDisabled("  (session: %d)", s.session_kills);
            }
            ImGui::Separator();

            if (!s.last_boss.empty()) {
                bool flash = s.last_was_pb && s.last_clear_age_s < 6.0;
                if (flash)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          IM_COL32(120, 230, 120, 255));
                ImGui::Text("Last: %s  %s%s", s.last_boss.c_str(),
                            format_run_time(s.last_clear_s).c_str(),
                            s.last_was_pb ? "  (New PB!)" : "");
                if (flash) ImGui::PopStyleColor();
            }

            if (s.records.empty()) {
                ImGui::TextDisabled("No boss kills yet.");
            } else if (ImGui::BeginTable("##boss_records", 6,
                                         ImGuiTableFlags_SizingFixedFit |
                                         ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Boss");
                ImGui::TableSetupColumn("Best");
                ImGui::TableSetupColumn("Avg");
                ImGui::TableSetupColumn("Last");
                ImGui::TableSetupColumn("Kills");
                ImGui::TableSetupColumn("Win%");
                ImGui::TableHeadersRow();
                for (const BossRecord& r : s.records) {
                    int attempts = r.kills + r.deaths;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::Text("Worst: %s",
                                    format_run_time(r.worst_s).c_str());
                        std::string rec = "Recent:";
                        for (double t : r.recent) {
                            rec += " ";
                            rec += format_run_time(t);
                        }
                        if (r.recent.empty()) rec += " -";
                        ImGui::TextUnformatted(rec.c_str());
                        ImGui::Text("Deaths: %d", r.deaths);
                        ImGui::EndTooltip();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(format_run_time(r.best_s).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(
                        format_run_time(boss_avg_s(r.sum_s, r.kills)).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(format_run_time(r.last_s).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", r.kills);
                    ImGui::TableNextColumn();
                    if (attempts > 0)
                        ImGui::Text("%.0f%%", boss_win_pct(r.kills, r.deaths));
                    else
                        ImGui::TextUnformatted("-");
                }
                ImGui::EndTable();
            }

            // Record management (two-step confirm).
            if (!s.records.empty()) {
                ImGui::Separator();
                static bool s_manage = false;
                ImGui::Checkbox("Manage", &s_manage);
                if (s_manage) {
                    static std::string s_arm;   // armed name, or "*" for all
                    for (const BossRecord& r : s.records) {
                        ImGui::PushID(r.name.c_str());
                        if (s_arm == r.name) {
                            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1),
                                               "Delete %s?", r.name.c_str());
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Yes")) {
                                boss_timer_reset(r.name.c_str());
                                s_arm.clear();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("No")) s_arm.clear();
                        } else {
                            if (ImGui::SmallButton("Reset")) s_arm = r.name;
                            ImGui::SameLine();
                            ImGui::TextUnformatted(r.name.c_str());
                        }
                        ImGui::PopID();
                    }
                    if (s_arm == "*") {
                        ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1),
                                           "Delete ALL records?");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Yes##all")) {
                            boss_timer_reset_all();
                            s_arm.clear();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("No##all")) s_arm.clear();
                    } else if (ImGui::SmallButton("Reset all")) {
                        s_arm = "*";
                    }
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
}

// v0.5.2.2 (issue #23 follow-up): standalone draggable "Loot tracker"
// window. Replaces the under-compass text strip which got lost
// visually. Default visible, toggleable via filter-tablet checkbox,
// snaps back via the reset-positions hotkey.
// #65: rare/sparkling-mob proximity alert banner. Top-center, click-through,
// pulsing border. Reads mob_watch_read() only (no game memory).
void render_mob_alert_banner() {
    const MobWatchSnapshot& s = mob_watch_read();
    if (!s.enabled || s.count <= 0) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 12.0f),
        ImGuiCond_Always, ImVec2(0.5f, 0.0f));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;

    float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
    ImU32 border = IM_COL32(255, (int)(110 + 90 * pulse), 60, 255);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(30, 12, 6, 235));
    ImGui::PushStyleColor(ImGuiCol_Border,   border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    if (ImGui::Begin("##mob_alert", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 120, 255));
        for (int i = 0; i < s.count; ++i) {
            bool dup = false;
            for (int j = 0; j < i; ++j)
                if (std::strcmp(s.hits[j].kind, s.hits[i].kind) == 0) {
                    dup = true; break;
                }
            if (dup) continue;
            ImGui::Text("Rare mob nearby:  %s   (%.0f m)",
                        s.hits[i].kind, s.hits[i].dist);
        }
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void render_loot_counter_window() {
    if (!g_loot_counter_visible.load()) return;
    if (!hero_state_read().locked) return;

    int c_done = 0, c_total = 0;
    int o_done = 0, o_total = 0;
    poi_progress_counts("chest",   &c_done, &c_total);
    poi_progress_counts("red_orb", &o_done, &o_total);
    if (c_total == 0 && o_total == 0) return;  // POI data not loaded yet

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,        kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColText);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10, 8));

    ImGui::SetNextWindowPos(ImVec2(40, 240), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags lf = ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoCollapse;
    if (g_ui_locked.load()) lf |= ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Loot", nullptr, lf)) {
        // Off-screen rescue.
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        // #47: skip on the appearing frame / degenerate size. These
        // windows use AlwaysAutoResize, so GetWindowSize() is ~0 before
        // content is measured, which made the pos+size<slack edge test
        // false-trip for any window near the top/left and reset it.
        if (!ImGui::IsWindowAppearing() && ws.x > 32.0f && ws.y > 32.0f &&
            window_is_offscreen(wp, ws, vp)) {
            logf("overlay: loot tracker was off-screen, snapping back");
            ImGui::SetWindowPos(ImVec2(40, 240));
        }

        ImDrawList* dl  = ImGui::GetWindowDrawList();
        const float fh  = ImGui::GetFontSize();
        const float gly = fh * 0.65f;  // glyph half-size

        auto row = [&](ImU32 color, int shape /*1=square,0=circle*/,
                       int done, int total, const char* label) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 c(p.x + gly, p.y + fh * 0.5f);
            constexpr ImU32 outline = IM_COL32(0, 0, 0, 230);
            if (shape == 1) {
                ImVec2 a(c.x - gly,  c.y - gly);
                ImVec2 b(c.x + gly,  c.y + gly);
                dl->AddRectFilled(a, b, color, 1.5f);
                dl->AddRect(a, b, outline, 1.5f, 0, 1.2f);
            } else {
                dl->AddCircleFilled(c, gly, color, 16);
                dl->AddCircle(c, gly, outline, 16, 1.2f);
            }
            ImGui::Dummy(ImVec2(gly * 2.0f + 6.0f, fh));
            ImGui::SameLine();
            // Progress bar look: dimmed if not complete, brighter as
            // ratio fills. Pure text would have been the previous
            // under-compass version, this one is the schicker variant.
            float ratio = total > 0 ? (float)done / (float)total : 0.0f;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d / %d", done, total);
            // Right-aligned-ish numbers in a fixed-width-feeling slot:
            ImGui::Text("%-9s", buf);
            ImGui::SameLine();
            ImGui::TextColored(done == total && total > 0
                                   ? ImVec4(0.55f, 1.0f, 0.55f, 1.0f)
                                   : ImVec4(0.85f, 0.78f, 0.55f, 1.0f),
                               "%s", label);
            // Thin progress sliver under the row.
            ImVec2 bar_min = ImGui::GetItemRectMin();
            ImVec2 bar_max = ImGui::GetItemRectMax();
            float track_y = bar_max.y + 1.0f;
            float track_x0 = p.x + gly * 2.0f + 6.0f;
            float track_x1 = bar_max.x;
            dl->AddRectFilled(ImVec2(track_x0, track_y),
                              ImVec2(track_x1, track_y + 2.0f),
                              IM_COL32(60, 50, 30, 220));
            dl->AddRectFilled(ImVec2(track_x0, track_y),
                              ImVec2(track_x0 +
                                     (track_x1 - track_x0) * ratio,
                                     track_y + 2.0f),
                              color);
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
        };

        row(IM_COL32(255, 200, 60, 255), 1, c_done, c_total, "Chests");
        row(IM_COL32(255,  60, 60, 255), 0, o_done, o_total, "Orbs");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

void render_keys_window() {
    if (!g_keys_open) return;
    if (!hero_state_read().locked) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,        kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColText);
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(60, 44, 20, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(96, 70, 32, 245));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(40, 28, 12, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    4.0f);

    ImGui::SetNextWindowPos(ImVec2(620, 540), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags keys_flags = ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoScrollbar;
    if (g_ui_locked.load()) keys_flags |= ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("Hotkeys", &g_keys_open, keys_flags)) {
        // Off-screen rescue (issue #1).
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        // #47: skip on the appearing frame / degenerate size. These
        // windows use AlwaysAutoResize, so GetWindowSize() is ~0 before
        // content is measured, which made the pos+size<slack edge test
        // false-trip for any window near the top/left and reset it.
        if (!ImGui::IsWindowAppearing() && ws.x > 32.0f && ws.y > 32.0f &&
            window_is_offscreen(wp, ws, vp)) {
            logf("overlay: hotkeys window was off-screen, snapping back");
            ImGui::SetWindowPos(ImVec2(620, 540));
        }
        auto row = [](const char* label, RebindSlot slot, UINT vk) {
            int active  = g_rebind_listening.load();
            bool listen = (active == (int)slot);
            ImGui::PushID((int)slot);
            ImGui::TextUnformatted(label);
            ImGui::SameLine(140);
            // v0.5.1: bind the string to a named local so its
            // lifetime extends through the Button call. Pre-v0.5.1
            // the rhs of the ternary was key_to_name(vk).c_str(),
            // which returns a pointer into a std::string temporary
            // that dies at the end of the full expression - Button
            // then read garbage (often empty for issue #17).
            std::string btn_text = listen ? std::string("press a key...")
                                          : key_to_name(vk);
            if (listen) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      IM_COL32(96, 64, 24, 245));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      IM_COL32(120, 84, 32, 245));
            }
            if (ImGui::Button(btn_text.c_str(), ImVec2(120, 0))) {
                g_rebind_listening.store(listen ? 0 : (int)slot);
            }
            if (listen) ImGui::PopStyleColor(2);
            ImGui::PopID();
        };
        row("Toggle Overlay",   RebindSlot::Overlay,   g_keybinds.toggle_overlay);
        row("Toggle DPS",       RebindSlot::Dps,       g_keybinds.toggle_dps);
        row("Toggle Minimap",   RebindSlot::Map,       g_keybinds.toggle_minimap);
        row("Reset DPS",        RebindSlot::Reset,     g_keybinds.reset_dps);
        row("Click-through",    RebindSlot::ClickThru, g_keybinds.toggle_clickthru);
        row("Pause DPS track",  RebindSlot::DpsTrack,  g_keybinds.toggle_dps_track);
        row("Reset window pos", RebindSlot::ResetPos,  g_keybinds.reset_positions);

        // Live status (issue #4 / #7 + #13).
        ImGui::Spacing();
        bool ct = g_clickthrough.load();
        ImGui::TextColored(
            ct ? ImVec4(0.5f, 1.0f, 0.6f, 1.0f)
               : ImVec4(0.8f, 0.75f, 0.6f, 1.0f),
            ct ? "Click-through is ON (overlay does not eat mouse clicks)"
               : "Click-through is OFF (mouse over overlay = overlay reacts)");
        bool dpaused = g_dps_tracking_paused.load();
        ImGui::TextColored(
            dpaused ? ImVec4(1.0f, 0.7f, 0.5f, 1.0f)
                    : ImVec4(0.8f, 0.75f, 0.6f, 1.0f),
            dpaused ? "DPS tracking PAUSED (alloc hook + tick short-circuit)"
                    : "DPS tracking ACTIVE (every damage event processed)");

        if (g_rebind_listening.load() != 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.5f, 1.0f),
                               "Press any key (Esc = cancel)");
        } else {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.8f, 0.75f, 0.6f, 1.0f),
                               "Click a slot, then press a key.");
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}

// Defined further down; forward declared here so the wndproc smart-
// hover logic (issue #7) can read it.
extern std::atomic<bool> g_overlay_wants_real_input;

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto is_press = [lp]() { return (lp & (1u << 30)) == 0; };

    // Rebind capture: while listening, the next non-modifier keydown
    // becomes the new binding (ESC cancels). Must run before the
    // normal hotkey logic so pressing e.g. F10 while listening rebinds
    // rather than toggling DPS.
    int listening = g_rebind_listening.load();
    if (listening != 0) {
        bool capture = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
                       is_press();
        if (msg == WM_KEYUP || msg == WM_SYSKEYUP) return 0;
        if (capture) {
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) {
                g_rebind_listening.store(0);
                logf("keybinds: rebind cancelled");
                return 0;
            }
            // Skip pure modifier keys - wait for the real key.
            if (vk == VK_SHIFT   || vk == VK_CONTROL || vk == VK_MENU ||
                vk == VK_LSHIFT  || vk == VK_RSHIFT  ||
                vk == VK_LCONTROL|| vk == VK_RCONTROL||
                vk == VK_LMENU   || vk == VK_RMENU   ||
                vk == VK_LWIN    || vk == VK_RWIN) {
                return 0;
            }
            switch ((RebindSlot)listening) {
                case RebindSlot::Dps:       g_keybinds.toggle_dps       = vk; break;
                case RebindSlot::Map:       g_keybinds.toggle_minimap   = vk; break;
                case RebindSlot::Reset:     g_keybinds.reset_dps        = vk; break;
                case RebindSlot::ClickThru: g_keybinds.toggle_clickthru = vk; break;
                case RebindSlot::DpsTrack:  g_keybinds.toggle_dps_track = vk; break;
                case RebindSlot::Overlay:   g_keybinds.toggle_overlay   = vk; break;
                case RebindSlot::ResetPos:  g_keybinds.reset_positions  = vk; break;
                default: break;
            }
            g_rebind_listening.store(0);
            keybinds_save();
            logf("keybinds: slot %d bound to %s",
                 listening, key_to_name(vk).c_str());
            return 0;
        }
    }

    // VK_F10 is the only key that arrives as WM_SYSKEYDOWN without
    // Alt being held (Windows reserves it for the menu accelerator),
    // so accept WM_SYSKEYDOWN only when the bound key is F10.
    bool is_keydown =
        (msg == WM_KEYDOWN) ||
        (msg == WM_SYSKEYDOWN && wp == VK_F10);

    if (is_keydown && is_press()) {
        UINT vk = (UINT)wp;
        if (vk == g_keybinds.toggle_dps) {
            bool now = !g_dps_visible.load();
            g_dps_visible.store(now);
            ui_lock_save();   // #47: persist show/hide across restarts
            logf("overlay: %s DPS -> %s",
                 key_to_name(vk).c_str(), now ? "VISIBLE" : "HIDDEN");
            return 0;
        }
        if (vk == g_keybinds.toggle_minimap) {
            bool now = !g_minimap_visible.load();
            g_minimap_visible.store(now);
            ui_lock_save();   // #47: persist show/hide across restarts
            logf("overlay: %s minimap -> %s",
                 key_to_name(vk).c_str(), now ? "VISIBLE" : "HIDDEN");
            return 0;
        }
        if (vk == g_keybinds.reset_dps) {
            aggregator_reset();
            return 0;
        }
        if (vk == g_keybinds.toggle_clickthru) {
            bool now = !g_clickthrough.load();
            g_clickthrough.store(now);
            ui_lock_save();
            logf("overlay: %s clickthrough -> %s",
                 key_to_name(vk).c_str(),
                 now ? "ON (mouse passes through)"
                     : "OFF (mouse interacts with windows)");
            return 0;
        }
        if (vk == g_keybinds.toggle_dps_track) {
            bool now = !g_dps_tracking_paused.load();
            g_dps_tracking_paused.store(now);
            ui_lock_save();
            logf("overlay: %s DPS tracking -> %s",
                 key_to_name(vk).c_str(),
                 now ? "PAUSED (alloc hook + damage tick short-circuit)"
                     : "ACTIVE (tracking damage events again)");
            return 0;
        }
        if (vk == g_keybinds.reset_positions) {
            g_reset_positions_request.store(true,
                                            std::memory_order_release);
            logf("overlay: %s reset_positions -> snapping all windows "
                 "to defaults", key_to_name(vk).c_str());
            return 0;
        }
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        // Issues #4 / #7 / #14: click-through has two halves and the
        // first version only did one of them.
        //   - Eat mouse messages from the game when ImGui wants them
        //     (= cursor over our window AND click-through OFF). Stops
        //     the overlay from intercepting attacks / camera rotate.
        //   - Stop ImGui from REACTING to those messages internally.
        //     The old code always called ImGui's wndproc handler first
        //     and then decided whether to forward to the game, but
        //     ImGui's state-machine had already recorded the click, so
        //     clicking the minimap zoom while click-through was on
        //     still zoomed.
        //
        // 0.4.11: when click-through is on, mouse moves still go to
        // ImGui so hover visuals work, but button / wheel events are
        // hidden from ImGui entirely. The game's wndproc always
        // receives them in this mode.
        // v0.5.2 (#13 ProsPurity): when the user is holding the
        // right mouse button (Farever's camera-control gesture) we
        // force click-through. Otherwise camera-rotation drags the
        // cursor across the overlay and accidentally clicks/drags
        // UI elements instead of attacking. With auto-clickthrough,
        // any mouse activity while RMB is held passes straight to
        // the game.
        //
        // v0.5.2.2 (issue #23): exception, RMB started OVER the
        // minimap window must reach ImGui so the right-click POI
        // toggle and the bezel reposition drag keep working. Track
        // ownership via a static set at WM_RBUTTONDOWN edge based on
        // the per-frame g_overlay_minimap_hovered cache; cleared on
        // WM_RBUTTONUP. While owned, RMB does NOT auto-CT.
        static bool s_rmb_owned_by_overlay = false;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) {
            s_rmb_owned_by_overlay =
                g_overlay_minimap_hovered.load(std::memory_order_acquire);
        }
        // #88 (left-handed mouse): GetAsyncKeyState polls the PHYSICAL
        // buttons, but Farever's camera gesture and our WM_*BUTTON routing run
        // on the LOGICAL secondary button. With a swapped (left-handed) mouse
        // the logical secondary button IS the physical LEFT button, so a
        // swapped user's every primary click would read here as "RMB camera
        // held", force click-through, and pass straight to the game - leaving
        // the overlay visible but completely unclickable (exactly the #88
        // report). Poll whichever physical button currently maps to the logical
        // secondary button. Right-handed (SM_SWAPBUTTON == 0) is unchanged.
        const int vk_secondary =
            GetSystemMetrics(SM_SWAPBUTTON) ? VK_LBUTTON : VK_RBUTTON;
        const bool rmb_held = (GetAsyncKeyState(vk_secondary) & 0x8000) != 0;
        const bool ct = g_clickthrough.load() ||
                        (rmb_held && !s_rmb_owned_by_overlay);
        bool is_mouse_button =
            (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
             msg == WM_LBUTTONDBLCLK ||
             msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ||
             msg == WM_RBUTTONDBLCLK ||
             msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
             msg == WM_MBUTTONDBLCLK ||
             msg == WM_MOUSEWHEEL  || msg == WM_MOUSEHWHEEL ||
             msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ||
             msg == WM_XBUTTONDBLCLK);
        if (msg == WM_RBUTTONUP) {
            // Clear after the ct check used it; UP itself is not
            // auto-CT'd since rmb_held is already false by message
            // arrival time.
            s_rmb_owned_by_overlay = false;
        }

        // #84/#19: a button DOWN and its matching UP must take the SAME
        // route to the game. The eat decision below reads a per-frame
        // wants_real_input snapshot that can flip between the DOWN and the
        // UP; if a real DOWN reached the game but we then eat the UP, the
        // game's attack/button latch sticks "pressed" (continuous auto-
        // attack until the next click). Track, per button, whether we ate
        // the DOWN, and only eat the UP if we did. Read-then-clear so the
        // latch is still readable by the eat block below.
        static bool s_lmb_down_eaten = false, s_mmb_down_eaten = false;
        const bool ate_l = s_lmb_down_eaten, ate_m = s_mmb_down_eaten;
        if (msg == WM_LBUTTONUP) s_lmb_down_eaten = false;
        if (msg == WM_MBUTTONUP) s_mmb_down_eaten = false;

        if (ct && is_mouse_button) {
            // Hide click / wheel events from ImGui entirely. Mouse
            // moves still flow through so hover/POI scaling work.
            return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
        }

        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (!ct) {
            // v0.5.1 smart hover (issue #7): only consume the click
            // when the cursor is over an ACTUAL interactive widget
            // (button, selectable, title bar drag) - not just over
            // an ImGui window's decorative pixels (the compass
            // bezel + mosaic image, empty DPS table area, etc).
            // io.WantCaptureMouse was the old check but it's too
            // coarse: it returned true for any cursor pos over any
            // ImGui window, eating clicks meant for the game world
            // behind the overlay. g_overlay_wants_real_input is set
            // each frame from IsAnyItemHovered + IsAnyItemActive.
            // #84/#19: never eat a button UP whose DOWN we let through to
            // the game; that would leave the game holding a DOWN with no
            // UP, which sticks as a continuous auto-attack. (See the latch
            // notes above.)
            const bool skip_eat = (msg == WM_LBUTTONUP && !ate_l) ||
                                  (msg == WM_MBUTTONUP && !ate_m);
            if (skip_eat) {
                static bool s_logged_skip = false;
                if (!s_logged_skip) {
                    s_logged_skip = true;
                    logf("overlay: #257 matched-pair guard passed a button "
                         "UP to the game (prevents a stuck auto-attack)");
                }
            }
            if (!skip_eat &&
                g_overlay_wants_real_input.load(std::memory_order_acquire)) {
                // v0.5.2 (issue #19): when we eat a button-DOWN, the
                // user is physically still holding the mouse button.
                // Farever uses an ALT-toggle to flip between cursor-
                // visible (UI clicks) and camera mode (LMB held =
                // attack). If the user clicks a UI element, then
                // toggles ALT, the game checks LMB state via raw
                // input + its own wndproc tracking and sees "held",
                // triggering continuous auto-attack.
                //
                // Fix: forward a synthetic matching BUTTON-UP to the
                // game's wndproc immediately after eating the DOWN.
                // The game's tracked button-state stays "released"
                // no matter what the physical mouse is doing, so the
                // ALT-toggle doesn't trigger an attack from a
                // captured click. (Doesn't help when smart hover lets
                // a click pass through to a decorative area; that's
                // by design.)
                UINT up_msg = 0;
                switch (msg) {
                    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
                        up_msg = WM_LBUTTONUP; s_lmb_down_eaten = true; break;
                    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
                        up_msg = WM_RBUTTONUP; break;
                    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
                        up_msg = WM_MBUTTONUP; s_mmb_down_eaten = true; break;
                }
                if (up_msg) {
                    CallWindowProcW(g_overlay.orig_wndproc, hwnd,
                                    up_msg, 0, lp);
                }
                switch (msg) {
                    case WM_MOUSEMOVE:
                    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
                        return 0;
                }
            }
        }
    }
    return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
}

void release_frame_targets() {
    for (auto& f : g_overlay.frames) {
        if (f.back_buffer) { f.back_buffer->Release(); f.back_buffer = nullptr; }
    }
}

void release_all() {
    release_frame_targets();
    for (auto& f : g_overlay.frames) {
        if (f.command_list) { f.command_list->Release(); f.command_list = nullptr; }
        if (f.allocator)    { f.allocator->Release();    f.allocator    = nullptr; }
    }
    g_overlay.frames.clear();
    if (g_overlay.rtv_heap)  { g_overlay.rtv_heap->Release();  g_overlay.rtv_heap  = nullptr; }
    if (g_overlay.srv_heap)  { g_overlay.srv_heap->Release();  g_overlay.srv_heap  = nullptr; }
    if (g_overlay.fence)     { g_overlay.fence->Release();     g_overlay.fence     = nullptr; }
    if (g_overlay.fence_event) {
        CloseHandle(g_overlay.fence_event);
        g_overlay.fence_event = nullptr;
    }
    if (g_overlay.queue)  { g_overlay.queue->Release();  g_overlay.queue  = nullptr; }
    if (g_overlay.device) { g_overlay.device->Release(); g_overlay.device = nullptr; }
}

bool wait_for_frame(FrameContext& frame, DWORD timeout_ms) {
    if (frame.fence_value == 0) return true;
    if (g_overlay.fence->GetCompletedValue() >= frame.fence_value) return true;
    g_overlay.fence->SetEventOnCompletion(frame.fence_value,
                                          g_overlay.fence_event);
    return WaitForSingleObject(g_overlay.fence_event, timeout_ms) ==
           WAIT_OBJECT_0;
}

bool create_back_buffer_targets(IDXGISwapChain3* swap_chain) {
    auto rtv_cpu_start =
        g_overlay.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(swap_chain->GetBuffer(
                i, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&f.back_buffer)))) {
            logf("overlay: GetBuffer(%u) failed", i);
            return false;
        }
        f.rtv_handle.ptr =
            rtv_cpu_start.ptr + i * g_overlay.rtv_descriptor_size;
        g_overlay.device->CreateRenderTargetView(f.back_buffer, nullptr,
                                                  f.rtv_handle);
    }
    return true;
}

// v0.4.17 Option B: tells overlay_init to skip the wndproc subclass.
// Set by overlay_window.cpp before the first overlay_on_present call.
std::atomic<bool> g_overlay_standalone_window{false};

// Render backend, resolved once at boot in dllmain worker_thread.
// The only place this changes overlay_render's behaviour is the
// transparent clear (DCOMP composites its own surface; game-swapchain
// draws on top of the game's existing frame). File-local here; the
// public overlay_backend()/overlay_set_backend() accessors live in the
// farever:: section below.
std::atomic<RenderBackend> g_render_backend{RenderBackend::Dcomp};

// v0.4.17 Option B: HWND override for composition swap chains where
// swap_chain->GetDesc().OutputWindow is NULL. Set by overlay_window.
std::atomic<HWND> g_overlay_hwnd_override{nullptr};

// v0.5.1 smart hover (issue #7): true iff the cursor is over an
// actually interactive ImGui widget (button, drag handle, etc).
// Set per-frame from IsAnyItemHovered + IsAnyItemActive at the end
// of overlay_render; read by the wndproc click-eating logic so
// clicks on decorative overlay pixels (compass mosaic, table padding)
// pass through to the game instead of being eaten.
std::atomic<bool> g_overlay_wants_real_input{false};

bool overlay_init(IDXGISwapChain3* swap_chain,
                  ID3D12CommandQueue* caller_queue) {
    g_overlay.owned_swap_chain = swap_chain;
    if (FAILED(swap_chain->GetDevice(
            __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&g_overlay.device)))) {
        logf("overlay: GetDevice failed");
        return false;
    }

    // v0.4.17 Option B: caller (overlay_window) can pass its own queue
    // - required because the swap chain is bound to one queue at
    // creation, and we need to submit on that same queue for Present
    // ordering. If caller passes nullptr we create our own (legacy
    // path from v0.4.16 game-swapchain mode, kept for safety).
    if (caller_queue) {
        g_overlay.queue = caller_queue;
        g_overlay.queue->AddRef();
        logf("overlay: using caller-provided queue %p",
             static_cast<void*>(g_overlay.queue));
    } else {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(g_overlay.device->CreateCommandQueue(
                &queue_desc, __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(&g_overlay.queue)))) {
            logf("overlay: CreateCommandQueue failed");
            return false;
        }
        logf("overlay: own DIRECT queue created at %p",
             static_cast<void*>(g_overlay.queue));
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) {
        logf("overlay: swap_chain->GetDesc failed");
        return false;
    }
    // v0.4.17 Option B: composition swap chains have no OutputWindow
    // (it's NULL). Use the override HWND that overlay_window set
    // from its own CreateWindowEx. ImGui-Win32 needs a real HWND to
    // compute IO.DisplaySize each frame, otherwise the GUI is empty.
    HWND override_hwnd = g_overlay_hwnd_override.load();
    g_overlay.hwnd              = override_hwnd ? override_hwnd : desc.OutputWindow;
    g_overlay.back_buffer_count = desc.BufferCount;
    g_overlay.rt_format         = desc.BufferDesc.Format;
    if (g_overlay.back_buffer_count == 0 ||
        g_overlay.back_buffer_count > kMaxBackBuffers) {
        logf("overlay: unexpected back-buffer count %u",
             g_overlay.back_buffer_count);
        return false;
    }
    g_overlay.frames.assign(g_overlay.back_buffer_count, FrameContext{});

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = g_overlay.back_buffer_count;
    rtv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &rtv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.rtv_heap)))) {
        logf("overlay: CreateDescriptorHeap(RTV) failed");
        return false;
    }
    g_overlay.rtv_descriptor_size =
        g_overlay.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 64;  // 0 = ImGui font; 1-3 = minimap textures; rest reserved
    srv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_overlay.device->CreateDescriptorHeap(
            &srv_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_overlay.srv_heap)))) {
        logf("overlay: CreateDescriptorHeap(SRV) failed");
        return false;
    }

    for (UINT i = 0; i < g_overlay.back_buffer_count; ++i) {
        auto& f = g_overlay.frames[i];
        if (FAILED(g_overlay.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&f.allocator)))) {
            logf("overlay: CreateCommandAllocator(%u) failed", i);
            return false;
        }
        if (FAILED(g_overlay.device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.allocator, nullptr,
                __uuidof(ID3D12GraphicsCommandList),
                reinterpret_cast<void**>(&f.command_list)))) {
            logf("overlay: CreateCommandList(%u) failed", i);
            return false;
        }
        f.command_list->Close();
    }

    if (FAILED(g_overlay.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&g_overlay.fence)))) {
        logf("overlay: CreateFence failed");
        return false;
    }
    g_overlay.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_overlay.fence_event) {
        logf("overlay: CreateEventW failed");
        return false;
    }
    g_overlay.next_fence_value = 0;

    if (!create_back_buffer_targets(swap_chain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize  = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                             static_cast<float>(desc.BufferDesc.Height));
    io.DeltaTime    = 1.0f / 60.0f;
    io.LogFilename  = nullptr;

    // Persist window positions / sizes / collapsed state to disk.
    // Pointer lifetime is the whole process so we stash a static
    // UTF-8 copy. Path is user_data_path("farever_layout.ini") in
    // %LOCALAPPDATA% - a dedicated file (never clashes with the game's
    // own imgui.ini) that now survives a reinstall like the rest of the
    // user state (#65).
    {
        std::wstring wpath = user_data_path(L"farever_layout.ini");
        static std::string s_ini_path;
        int n = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            s_ini_path.resize(static_cast<std::size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1,
                                s_ini_path.data(), n, nullptr, nullptr);
            io.IniFilename = s_ini_path.c_str();
            logf("overlay: imgui layout file = %s", s_ini_path.c_str());
        } else {
            io.IniFilename = nullptr;
        }
    }
    ImGui::StyleColorsDark();

    // Pre-build the font atlas (lazy build inside NewFrame stack-
    // overflows the HashLink host thread).
    if (!io.Fonts->Build()) {
        logf("overlay: io.Fonts->Build() failed");
        return false;
    }

    auto srv_cpu = g_overlay.srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto srv_gpu = g_overlay.srv_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(g_overlay.device,
                             static_cast<int>(g_overlay.back_buffer_count),
                             g_overlay.rt_format, g_overlay.srv_heap,
                             srv_cpu, srv_gpu)) {
        logf("overlay: ImGui_ImplDX12_Init failed");
        return false;
    }
    if (!ImGui_ImplWin32_Init(g_overlay.hwnd)) {
        logf("overlay: ImGui_ImplWin32_Init failed");
        return false;
    }
    // v0.4.17 Option B: in standalone-window mode we deliberately do
    // NOT install the wndproc subclass. The overlay window forwards
    // every mouse event to the game window itself (see
    // overlay_window.cpp's wndproc), so we must not let overlay's
    // wndproc intercept them first - it would eat clicks before our
    // base wndproc forwards them. ImGui input handling is also off,
    // which means overlay UI is render-only for v0.4.17.
    if (!g_overlay_standalone_window.load()) {
        g_overlay.orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_overlay.hwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(overlay_wndproc)));
        if (!g_overlay.orig_wndproc) {
            logf("overlay: SetWindowLongPtrW(GWLP_WNDPROC) failed");
            return false;
        }
    } else {
        logf("overlay: standalone-window mode, wndproc subclass skipped");
    }

    // Minimap assets - optional. Failure logs but doesn't abort init
    // (the DPS meter still works without the compass background).
    std::wstring mosaic_p = data_path(L"maps\\W1_Siagarta.preview.png");
    {
        // Use the background-decoded mosaic if it's ready (started at DLL
        // load, overlapping the loading screen), so init only uploads the
        // pixels instead of doing the multi-second WIC decode here. Falls
        // back to a synchronous decode if the preload didn't run / failed.
        std::vector<std::uint8_t> mpx;
        UINT mw = 0, mh = 0;
        bool ok = false;
        if (textures_take_mosaic(&mpx, &mw, &mh)) {
            ok = load_texture_from_pixels(g_overlay.device, g_overlay.srv_heap,
                                          1, mpx, mw, mh, &g_overlay.mosaic);
        }
        if (!ok && !load_texture_from_file(
                g_overlay.device, g_overlay.srv_heap, 1,
                mosaic_p.c_str(), &g_overlay.mosaic)) {
            logf("overlay: mosaic load failed; minimap will use solid bg");
        }
    }
    std::wstring atlas_p = data_path(L"icons\\activities.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 2,
            atlas_p.c_str(), &g_overlay.poi_atlas)) {
        logf("overlay: POI atlas load failed; falling back to shapes");
    }
    std::wstring arrow_p = data_path(L"icons\\PlayerMapArrow.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 3,
            arrow_p.c_str(), &g_overlay.player_arrow)) {
        logf("overlay: player arrow load failed; falling back to dot");
    }
    std::wstring pois_p = data_path(L"pois_W1_Siagarta.json");
    pois_load(pois_p.c_str());
    // #62: POI progress is now loaded per character by poi_progress_set_profile
    // (called from the hl_pump worker once the local hero locks), not at boot.
    waypoints_load();
    ui_lock_load();

    logf("overlay: DX12+ImGui init OK (hwnd=%p, buffers=%u, fmt=%d)",
         g_overlay.hwnd, g_overlay.back_buffer_count,
         static_cast<int>(g_overlay.rt_format));

    plugins_start();
    return true;
}

void render_imgui_window() {
    // The aggregator runs unconditionally - pulls drain damage events
    // every frame so totals stay correct even with the window hidden.
    aggregator_tick();
    if (!g_dps_visible.load()) return;
    // Hold the UI back until the local Hero is locked (= we know
    // which character is the user). No point showing 0-damage rows
    // before the world is even visible.
    if (!hero_state_read().locked) return;
    const AggSnapshot& snap = aggregator_snapshot();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,    window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_Border,      kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,     kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,        kColText);
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(48, 36, 16, 240));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,    IM_COL32( 0,  0,  0, 60));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, IM_COL32( 0,  0,  0, 110));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, kColBezel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    // No NoCollapse flag: ImGui's title-bar collapse arrow lets the
    // user shrink the window to just the title bar by clicking the
    // triangle next to "DPS Meter".
    ImGuiWindowFlags dps_flags = ImGuiWindowFlags_NoScrollbar;
    // Independent DPS lock (issue #15): the minimap lock no longer
    // pins the DPS window. The padlock inside the DPS window's status
    // line toggles g_dps_locked instead.
    if (g_dps_locked.load())
        dps_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    ImGui::Begin("DPS Meter", nullptr, dps_flags);
    // Off-screen rescue (issue #1).
    {
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        // #47: skip on the appearing frame / degenerate size. These
        // windows use AlwaysAutoResize, so GetWindowSize() is ~0 before
        // content is measured, which made the pos+size<slack edge test
        // false-trip for any window near the top/left and reset it.
        if (!ImGui::IsWindowAppearing() && ws.x > 32.0f && ws.y > 32.0f &&
            window_is_offscreen(wp, ws, vp)) {
            logf("overlay: DPS meter was off-screen, snapping back");
            ImGui::SetWindowPos(ImVec2(40, 40));
        }
    }

    // Pick a layout tier from current content width - drives column
    // visibility, header/footer verbosity, icon scale. Thresholds tuned
    // by eyeballing default font metrics; ImGui's WindowAutoResize is
    // off here, so the user is in charge of the size and we adapt.
    float content_w = ImGui::GetContentRegionAvail().x;
    int tier = 3;
    if (content_w < 520.0f) tier = 2;
    if (content_w < 360.0f) tier = 1;
    if (content_w < 220.0f) tier = 0;

    const float kIconPx = (tier <= 1) ? 20.0f : (tier == 2 ? 22.0f : 24.0f);

    // Lock toggle (tiny custom-drawn padlock) at the start of the
    // status line. Toggles the DPS-window-only lock (issue #15) -
    // the minimap has its own padlock on the bezel.
    {
        bool locked = g_dps_locked.load();
        const float fh = ImGui::GetFontSize();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##dps_lock_toggle", ImVec2(fh, fh));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            g_dps_locked.store(!locked);
            ui_lock_save();
            locked = !locked;
        }
        if (hovered) {
            ImGui::SetTooltip(locked ? "DPS window locked (click to unlock)"
                                     : "DPS window unlocked (click to lock)");
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + fh * 0.5f, p.y + fh * 0.5f);
        ImU32 col = locked ? IM_COL32(255, 220, 100, 240)
                           : IM_COL32(190, 190, 190, 240);
        if (hovered) col = IM_COL32(255, 255, 255, 255);
        float s = fh * 0.30f;
        // Body
        dl->AddRect({c.x - s, c.y - s * 0.1f},
                    {c.x + s, c.y + s * 1.05f},
                    col, s * 0.15f, 0, 1.4f);
        // Keyhole
        dl->AddCircleFilled({c.x, c.y + s * 0.5f}, s * 0.20f, col, 10);
        // Shackle
        float sh = s * 0.65f;
        dl->AddLine({c.x - sh, c.y - s * 0.1f},
                    {c.x - sh, c.y - s * 0.9f}, col, 1.4f);
        dl->AddLine({c.x - sh, c.y - s * 0.9f},
                    {c.x + sh, c.y - s * 0.9f}, col, 1.4f);
        if (locked)
            dl->AddLine({c.x + sh, c.y - s * 0.9f},
                        {c.x + sh, c.y - s * 0.1f}, col, 1.4f);
        ImGui::SameLine(0.0f, 6.0f);
    }

    // Combat-state badge, leading the status line. Red filled circle
    // while in combat, dim grey hollow circle once we've idled past
    // the timeout. v0.5: drawn via ImDrawList because the unicode
    // glyphs ● (U+25CF) / ○ (U+25CB) we used pre-v0.5 are not in
    // the Karla font we ship, so they rendered as ? for users.
    {
        const float fh   = ImGui::GetFontSize();
        ImVec2 p         = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##combat_badge", ImVec2(fh, fh));
        bool hovered     = ImGui::IsItemHovered();
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + fh * 0.5f, p.y + fh * 0.5f);
        float r          = fh * 0.30f;
        ImU32 col_active = IM_COL32(255, 80, 80, 255);
        ImU32 col_idle   = IM_COL32(140, 140, 140, 240);
        if (snap.in_combat) {
            dl->AddCircleFilled(c, r, col_active, 16);
        } else {
            dl->AddCircle      (c, r, col_idle,   16, 1.5f);
        }
        if (hovered) {
            ImGui::SetTooltip(snap.in_combat ? "in combat" : "idle");
        }
        ImGui::SameLine(0.0f, 8.0f);
    }

    if (snap.have_fight) {
        if (tier >= 2) {
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.52f, 1.0f),
                               "elapsed %5.1fs", snap.elapsed_sec);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::Text("total %.0f", snap.total_damage);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "DPS %.1f", snap.dps);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                               "HPS %.1f", snap.hps);
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::TextColored(ImVec4(0.45f, 0.72f, 0.98f, 1.0f),
                               "SPS %.1f", snap.sps);
        } else if (tier == 1) {
            ImGui::Text("total %.0f", snap.total_damage);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "DPS %.1f", snap.dps);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                               "HPS %.1f", snap.hps);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(ImVec4(0.45f, 0.72f, 0.98f, 1.0f),
                               "SPS %.1f", snap.sps);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "%.0f DPS", snap.dps);
        }
    } else {
        // No fight yet - just keep the line height occupied by the
        // combat dot; no status text.
        ImGui::NewLine();
    }

    // #57/#70: DMG/HEAL/SHIELD toggle - picks which breakdown the table shows.
    // Only meaningful from tier 1 up (tier 0 is the tiny icon-only mode).
    int view = g_meter_view.load();
    if (tier >= 1) {
        if (ImGui::SmallButton(view == MV_DMG ? "[DMG]" : "DMG")) {
            g_meter_view.store(MV_DMG); ui_lock_save();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton(view == MV_HEAL ? "[HEAL]" : "HEAL")) {
            g_meter_view.store(MV_HEAL); ui_lock_save();
        }
        ImGui::SameLine(0.0f, 6.0f);
        if (ImGui::SmallButton(view == MV_SHIELD ? "[SHIELD]" : "SHIELD")) {
            g_meter_view.store(MV_SHIELD); ui_lock_save();
        }
        view = g_meter_view.load();
    }

    // Table data source per view.
    const SkillRow* view_rows  = view == MV_SHIELD ? snap.shield_rows
                               : view == MV_HEAL   ? snap.heal_rows : snap.rows;
    std::size_t     view_count = view == MV_SHIELD ? snap.shield_row_count
                               : view == MV_HEAL   ? snap.heal_row_count
                                                   : snap.row_count;
    double          view_total = view == MV_SHIELD ? snap.total_shield
                               : view == MV_HEAL   ? snap.total_heal
                                                   : snap.total_damage;
    const char*     rate_hdr   = view == MV_SHIELD ? "SPS"
                               : view == MV_HEAL   ? "HPS" : "DPS";

    ImGui::Spacing();

    // Column set per tier. Each entry is (header, stretch_weight,
    // bitmask of what to render in the cell).
    enum Col { COL_SKILL = 0, COL_HITS, COL_TOTAL, COL_MAX,
               COL_CRIT, COL_DPS, COL_PCT };
    int  col_ids[7];
    int  col_count   = 0;
    auto add_col     = [&](int id) { col_ids[col_count++] = id; };

    add_col(COL_SKILL);
    if (tier >= 2) add_col(COL_HITS);
    if (tier >= 1) add_col(COL_TOTAL);
    if (tier >= 3) add_col(COL_MAX);
    if (tier >= 3) add_col(COL_CRIT);
    add_col(COL_DPS);
    if (tier >= 1) add_col(COL_PCT);

    ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    // Reserve bottom space only for the (optional) history block.
    static bool g_history_open = true;
    const float kLineH        = ImGui::GetTextLineHeightWithSpacing();
    const bool  show_hist     = (tier >= 2 && snap.history_count > 0);
    const float history_reserve =
        show_hist ? (g_history_open ? 130.0f : kLineH) : 0.0f;
    const float footer_h =
        (history_reserve > 0.0f) ? -history_reserve : 0.0f;

    if (ImGui::BeginTable("##skills", col_count, table_flags,
                          ImVec2(0.0f, footer_h))) {
        ImGui::TableSetupScrollFreeze(0, tier >= 2 ? 1 : 0);
        for (int c = 0; c < col_count; ++c) {
            const char* hdr  = "";
            float       wgt  = 1.0f;
            switch (col_ids[c]) {
                case COL_SKILL: hdr = "Skill"; wgt = (tier == 0) ? 0.8f : 1.4f; break;
                case COL_HITS:  hdr = "Hits";  wgt = 0.55f; break;
                case COL_TOTAL: hdr = "Total"; wgt = 1.0f;  break;
                case COL_MAX:   hdr = "Max";   wgt = 0.85f; break;
                case COL_CRIT:  hdr = "Crit%"; wgt = 0.65f; break;
                case COL_DPS:   hdr = rate_hdr; wgt = 0.85f; break;
                case COL_PCT:   hdr = "%";     wgt = 0.55f; break;
            }
            ImGui::TableSetupColumn(
                hdr, ImGuiTableColumnFlags_WidthStretch, wgt);
        }
        if (tier >= 2) ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < view_count; ++i) {
            const SkillRow& r = view_rows[i];
            ImGui::TableNextRow();

            float crit_pct = r.hit_count > 0
                ? 100.0f * (float)r.crit_count / (float)r.hit_count : 0.0f;
            double row_dps = (snap.elapsed_sec > 0.001)
                ? r.total / snap.elapsed_sec : 0.0;
            float pct_total = view_total > 0.001
                ? 100.0f * (float)(r.total / view_total) : 0.0f;

            for (int c = 0; c < col_count; ++c) {
                ImGui::TableSetColumnIndex(c);
                switch (col_ids[c]) {
                    case COL_SKILL: {
                        SkillGfx sgfx{};
                        LoadedTexture* atlas_tex = nullptr;
                        if (skill_resolve_lookup(r.skill, &sgfx)) {
                            atlas_tex = get_or_load_atlas(sgfx.atlas_filename);
                        }
                        if (atlas_tex && atlas_tex->resource &&
                            sgfx.size > 0 &&
                            atlas_tex->width > 0 && atlas_tex->height > 0) {
                            float aw  = (float)atlas_tex->width;
                            float ah  = (float)atlas_tex->height;
                            float px0 = (float)(sgfx.x * sgfx.size);
                            float py0 = (float)(sgfx.y * sgfx.size);
                            float pw  = (float)(sgfx.width  * sgfx.size);
                            float ph  = (float)(sgfx.height * sgfx.size);
                            ImVec2 uv0(px0 / aw, py0 / ah);
                            ImVec2 uv1((px0 + pw) / aw, (py0 + ph) / ah);
                            ImGui::Image(
                                (ImTextureID)atlas_tex->srv_gpu.ptr,
                                ImVec2(kIconPx, kIconPx), uv0, uv1);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", r.skill);
                            }
                        } else {
                            ImGui::TextUnformatted(r.skill);
                        }
                        break;
                    }
                    case COL_HITS:  ImGui::Text("%d",  r.hit_count); break;
                    case COL_TOTAL: ImGui::Text("%.0f", r.total);     break;
                    case COL_MAX:   ImGui::Text("%.0f", r.max_hit);   break;
                    case COL_CRIT:
                        if (crit_pct > 0.0f) {
                            ImGui::TextColored(
                                ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                "%.0f%%", crit_pct);
                        } else {
                            ImGui::TextUnformatted("-");
                        }
                        break;
                    case COL_DPS:   ImGui::Text("%.1f",  row_dps);   break;
                    case COL_PCT:   ImGui::Text("%.0f%%", pct_total); break;
                }
            }
        }
        ImGui::EndTable();
    }

    // Fight-history block: visible from tier 2 onward, collapsible.
    if (show_hist) {
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "Fight history (%zu)###hist_hdr",
                      snap.history_count);
        ImGuiTreeNodeFlags hdr_flags =
            g_history_open ? ImGuiTreeNodeFlags_DefaultOpen : 0;
        bool was_open = g_history_open;
        g_history_open = ImGui::CollapsingHeader(hdr, hdr_flags);
        if (was_open != g_history_open) {
            // Force one extra frame so the table re-lays out with the
            // new reserve. ImGui auto-handles it next frame anyway.
        }
        if (g_history_open) {
            constexpr ImGuiTableFlags hist_flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("##hist", 6, hist_flags,
                                  ImVec2(0.0f, history_reserve - kLineH))) {
                ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthStretch, 0.70f);
                ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("Total",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("DPS",     ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("HPS",     ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableHeadersRow();

                for (std::size_t i = 0; i < snap.history_count; ++i) {
                    const FightLogEntry& f = snap.history[i];

                    // FILETIME -> local hh:mm:ss
                    std::int64_t ft100ns =
                        f.ended_unix_ms * 10000LL + 116444736000000000LL;
                    FILETIME ft;
                    ft.dwLowDateTime  = (DWORD)(ft100ns & 0xffffffff);
                    ft.dwHighDateTime = (DWORD)(ft100ns >> 32);
                    SYSTEMTIME utc, lt;
                    FileTimeToSystemTime(&ft, &utc);
                    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &lt);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);

                    // Whole-row selectable: click opens the detail view
                    // for that fight (per-skill breakdown).
                    char sel[32];
                    std::snprintf(sel, sizeof(sel), "#%d##fight_%d",
                                  f.id, f.id);
                    bool is_sel = (g_selected_fight_id == f.id);
                    if (ImGui::Selectable(sel, is_sel,
                            ImGuiSelectableFlags_SpanAllColumns)) {
                        g_selected_fight_id =
                            is_sel ? 0 : f.id;   // toggle
                    }
                    if (f.top_skill[0] && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "top: %s   |   %d hits   |   click for details",
                            f.top_skill, f.hit_count);
                    }
                    ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%02u:%02u:%02u",
                                    lt.wHour, lt.wMinute, lt.wSecond);
                    ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%.1fs", f.duration_sec);
                    ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%.0f", f.total_damage);
                    ImGui::TableSetColumnIndex(4);
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                            "%.1f", f.dps);
                    ImGui::TableSetColumnIndex(5);
                        ImGui::TextColored(
                            ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                            "%.1f", f.hps);
                }
                ImGui::EndTable();
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(10);
}

// v0.4.14: forward-declared because the definitions live near the
// kill-switch globals further down. Called from overlay_render before
// any hero-lock-gated window.
void render_diagnostic_box();
extern std::atomic<bool> g_diag_no_overlay;
extern std::atomic<bool> g_diag_no_hl_tick;
extern std::atomic<bool> g_diag_anticrash;

// v0.5.6 cursor-park workaround. Farever toggles the system cursor
// invisible whenever the player is in camera mode (ALT taps flip it).
// When invisible the cursor still moves physically and can hover over
// overlay widgets without the user knowing. Park it at the game
// window center while invisible so the overlay stays untouched.
//
// We don't track ALT ourselves - the cursor's actual show/hide state
// is the ground truth via GetCursorInfo(). That way we work regardless
// of how the game decides to toggle cursor visibility (ALT, ESC menu,
// inventory, etc.).
//
// Idempotent each frame; the ClipCursor call is cheap when the clip
// rect hasn't changed. Conditions to clip:
//   - game window is the foreground window
//   - overlay is visible + hero locked (no point clipping otherwise)
//   - clickthrough not on (user wants overlay click-through, cursor
//     should be the game's responsibility)
//   - system cursor is currently HIDDEN (game is in camera mode)
static void overlay_park_cursor_tick() {
    HWND game_hwnd = g_overlay.hwnd;
    if (!game_hwnd) return;

    static bool s_clip_active = false;

    // Opt-in via data/cursor_park.flag. Defaults OFF because ClipCursor
    // and SetCursorPos can interact with the game's wndproc in
    // unpredictable ways. User enables it explicitly.
    static int  s_flag_check_ticks = 0;
    static bool s_enabled          = false;
    if ((s_flag_check_ticks++ % 120) == 0) {
        static std::wstring flag_path;
        if (flag_path.empty()) {
            flag_path = data_path(L"cursor_park.flag");
        }
        DWORD attr = GetFileAttributesW(flag_path.c_str());
        s_enabled = (attr != INVALID_FILE_ATTRIBUTES);
    }
    if (!s_enabled) {
        if (s_clip_active) {
            ClipCursor(nullptr);
            s_clip_active = false;
        }
        return;
    }

    bool fg_ok       = (GetForegroundWindow() == game_hwnd) &&
                       !IsIconic(game_hwnd);
    bool hero_locked = (hero_state_locked_ptr() != 0);
    // Any of the overlay's primary windows visible counts as "user
    // might hover an overlay widget"; cursor park only matters then.
    bool any_overlay = g_minimap_visible.load(std::memory_order_acquire) ||
                       g_dps_visible.load(std::memory_order_acquire)     ||
                       plugins_manager_visible();
    bool ct          = g_clickthrough.load(std::memory_order_acquire);

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    bool cursor_visible = true;
    if (GetCursorInfo(&ci)) {
        cursor_visible = (ci.flags & CURSOR_SHOWING) != 0;
    }

    bool want_clip = fg_ok && hero_locked && any_overlay && !ct &&
                     !cursor_visible;

    if (want_clip && !s_clip_active) {
        RECT rc;
        if (GetClientRect(game_hwnd, &rc)) {
            POINT center{ (rc.right  - rc.left) / 2,
                          (rc.bottom - rc.top)  / 2 };
            ClientToScreen(game_hwnd, &center);
            // One-shot snap so the cursor leaves wherever it last was
            // (potentially over an overlay widget).
            SetCursorPos(center.x, center.y);
            RECT clip{ center.x, center.y, center.x + 1, center.y + 1 };
            if (ClipCursor(&clip)) s_clip_active = true;
        }
    } else if (!want_clip && s_clip_active) {
        ClipCursor(nullptr);
        s_clip_active = false;
    }
}

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* /*unused_v0_4_16*/) {
    // Issue #54: an AC appeared mid-session -> go passive, draw nothing.
    // Hooks stay installed (issue #8); this path just no-ops.
    if (anticheat_is_disabled()) return;
    keybinds_maybe_reload();
    // #65: the locked character changed -> reload that character's UI settings
    // (runs on the render thread so the settings vars stay single-threaded).
    if (g_profile_reload.exchange(false, std::memory_order_acquire))
        ui_lock_load();
    overlay_park_cursor_tick();

    // Crash-diagnosis heartbeat. Logs every 600 frames so a post-crash
    // log shows the last frame the overlay submission completed for,
    // and -- combined with the damage heartbeat -- lets us tell
    // whether a freeze died inside our overlay submission or in the
    // game's own Present that followed.
    //
    // 0.4.9: heartbeat now reports per-guard skip counts since the
    // previous heartbeat. If a long-AFK alt-tab crash hits, the last
    // heartbeat tells us whether our guards were doing their job
    // (skip_fg should == 600 if alt-tabbed for the full window) or
    // whether we slipped through and submitted into background-state.
    static std::uint64_t s_overlay_ticks    = 0;
    static int s_submitted   = 0;
    static int s_skip_no_hero = 0;
    static int s_skip_pause   = 0;
    static int s_skip_iconic  = 0;
    static int s_skip_hidden  = 0;
    static int s_skip_fg      = 0;
    static int s_skip_fence   = 0;
    if (++s_overlay_ticks % 600 == 0) {
        logf("overlay: alive @ tick %llu submitted=%d "
             "skip(no_hero=%d pause=%d iconic=%d hidden=%d fg=%d "
             "fence=%d) auto-disabled=%d",
             (unsigned long long)s_overlay_ticks,
             s_submitted,
             s_skip_no_hero, s_skip_pause, s_skip_iconic, s_skip_hidden,
             s_skip_fg, s_skip_fence,
             (int)!g_overlay_enabled.load());
        s_submitted = s_skip_no_hero = s_skip_pause = 0;
        s_skip_iconic = s_skip_hidden = s_skip_fg = s_skip_fence = 0;
    }

    // Loading-screen guard: when we have no Hero lock, the game is
    // very likely between zones (loading / streaming / hxbit resync)
    // and is sometimes mid-recreation of its DX12 resources. Doing a
    // full render-and-execute through our overlay during that window
    // can crash the host's Present (observed AVs in DX12Driver.present
    // on dungeon entry). Skip the entire overlay path until we're
    // back on a known-good Hero.
    HeroSnapshot h = hero_state_read();
    // v0.4.14: when a kill switch is engaged we still want to render
    // the diagnostic status box even without a Hero lock, so the user
    // can visually confirm the mod is alive. Skip the no_hero early-
    // exit in that case and fall through; the actual minimap + DPS
    // windows still gate on h.locked internally so they stay hidden.
    const bool diag_force = g_diag_no_hl_tick.load() ||
                            g_diag_no_overlay.load() ||
                            g_diag_anticrash.load();
    if (!h.locked && !diag_force) {
        ++s_skip_no_hero;
        return;
    }

    // Post-transition pause (issues #11 + #12). The DX12Driver.present
    // AVs reported by several users happened at moments when the
    // game's render pipeline was reconfiguring resources --- first
    // hero spawn after the title screen (issue #12: crash ~5s after
    // LOCKED) and cross-zone teleports (issue #11: crash ~31s after
    // a Mayda->Azuram teleport). Two heuristics catch both:
    //
    //   1. Hero pointer changed from null (or to a fresh value),
    //      meaning the player just locked into the world or
    //      reconnected after a Hero replace.
    //   2. Hero position jumped more than 500 game units between
    //      frames, meaning a teleport just streamed in a new chunk.
    //
    // When either fires, we skip overlay submission for the next
    // kPauseFrames frames (~5 s @ 60 fps) so the game's own DX12
    // streaming finishes before we add another command list on top.
    // 0.4.10 bumped from 120 (2 s) to 300 (5 s) after issue #12's
    // v0.4.9 log showed our submission resumed cleanly 2 s post-LOCK,
    // ran 1441 frames without any guard firing, and then crashed at
    // the 5 s mark.
    //
    // 0.4.12 adds a follow-on "skeleton" phase: after the full pause
    // expires we don't immediately resume the full minimap render
    // (mosaic + 1000+ POI markers + player arrow). Instead we go
    // through kSkeletonFrames where the minimap renders only the
    // bezel ring + player arrow. That keeps the draw-call count tiny
    // during the dangerous post-transition window where issue #12's
    // user's setup was crashing ~9 s after we started submitting.
    // Hypothesis: the volume of our ImGui geometry (mosaic AddImage
    // + 1k AddImage/AddCircle for POIs) was the trigger on that
    // hardware. If skeleton mode prevents the crash, we know the
    // POI / mosaic render is the heavy step.
    {
        constexpr int   kPauseFrames    = 300;
        constexpr int   kSkeletonFrames = 600;   // ~10 s @ 60 fps
        constexpr double kTeleportDist2 = 500.0 * 500.0;
        static std::uintptr_t s_prev_hero_ptr = 0;
        static double s_prev_x = 0.0, s_prev_y = 0.0, s_prev_z = 0.0;
        static int    s_pause_left    = 0;
        static int    s_skeleton_left = 0;   // follow-on phase, see 0.4.12 note

        std::uintptr_t cur_hero = hero_state_locked_ptr();
        double dx = h.x - s_prev_x;
        double dy = h.y - s_prev_y;
        double dz = h.z - s_prev_z;
        double dist2 = dx * dx + dy * dy + dz * dz;

        if (cur_hero != s_prev_hero_ptr) {
            // First lock or hero-pointer changed (zone transition,
            // recovery from a re-validation failure, etc.).
            s_pause_left    = kPauseFrames;
            s_skeleton_left = kSkeletonFrames;
            logf("overlay: hero pointer changed (0x%llx -> 0x%llx); "
                 "pausing overlay submission for %d frames + skeleton "
                 "minimap for %d frames after that",
                 (unsigned long long)s_prev_hero_ptr,
                 (unsigned long long)cur_hero,
                 kPauseFrames, kSkeletonFrames);
        } else if (s_prev_hero_ptr != 0 && dist2 > kTeleportDist2) {
            // Same Hero, sudden large position jump = in-place
            // teleport (some teleporters don't allocate a new Hero,
            // they just update pos).
            s_pause_left    = kPauseFrames;
            s_skeleton_left = kSkeletonFrames;
            logf("overlay: position jumped %.0fm from (%.0f,%.0f,%.0f) "
                 "to (%.0f,%.0f,%.0f); pausing for %d + skeleton %d frames",
                 std::sqrt(dist2),
                 s_prev_x, s_prev_y, s_prev_z, h.x, h.y, h.z,
                 kPauseFrames, kSkeletonFrames);
        }

        s_prev_hero_ptr = cur_hero;
        s_prev_x = h.x; s_prev_y = h.y; s_prev_z = h.z;

        if (s_pause_left > 0) {
            --s_pause_left;
            ++s_skip_pause;
            return;
        }
        // Pause expired -- if we're still in the skeleton window,
        // flip the file-scope flag the minimap reads to skip
        // mosaic + POIs. Decremented every frame after the full
        // pause is done.
        if (s_skeleton_left > 0) {
            --s_skeleton_left;
            g_skeleton_minimap.store(true, std::memory_order_release);
        } else {
            g_skeleton_minimap.store(false, std::memory_order_release);
        }
    }

    // Alt-tab / minimize guard (issues #9, #10). When the game window
    // is iconic or not the foreground window, DXGI's Present can return
    // DXGI_STATUS_OCCLUDED and our back buffer state isn't guaranteed
    // valid. Submitting our overlay in that state is the most likely
    // cause of the alt-tab access violations a few users reported.
    // Cheap to check via the Win32 API; skips the whole render pass
    // (including our ExecuteCommandLists) when the window isn't on
    // screen.
    if (g_overlay.hwnd) {
        if (IsIconic(g_overlay.hwnd))         { ++s_skip_iconic; return; }
        if (!IsWindowVisible(g_overlay.hwnd)) { ++s_skip_hidden; return; }
        // The big one for issue #11: alt-tab to Discord / a browser
        // doesn't minimize or hide the game window, it just loses
        // foreground focus. Game's DXGI swap chain is in occluded /
        // background state at that point and our overlay submission
        // is implicated in the AV some users hit in that scenario.
        // GetForegroundWindow is cheap and the right check.
        HWND fg = GetForegroundWindow();
        if (fg && fg != g_overlay.hwnd)       { ++s_skip_fg;     return; }
    }

    UINT idx = swap_chain->GetCurrentBackBufferIndex();
    if (idx >= g_overlay.frames.size()) return;
    auto& frame = g_overlay.frames[idx];
    if (!frame.allocator || !frame.back_buffer) return;

    if (!wait_for_frame(frame, kFenceTimeoutMs)) {
        int n = ++g_consecutive_slow_frames;
        if (n == 1 || (n % 30) == 0) {
            logf("overlay: fence wait timed out (%d consecutive)", n);
        }
        if (n >= kAutoDisableSlowFrames) {
            g_overlay_enabled.store(false);
            logf("overlay: %d slow frames -> auto-disabled", n);
        }
        ++s_skip_fence;
        return;
    }
    g_consecutive_slow_frames = 0;

    // v0.5.2.2 issue #23: clear the "minimap hovered" cache; if the
    // minimap window does not render this frame (F8 hidden, hero
    // unlocked) the flag stays false and the wndproc keeps the v0.5.2
    // RMB auto-clickthrough behaviour unchanged.
    g_overlay_minimap_hovered.store(false, std::memory_order_release);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // v0.5.3 issue #22: apply UI scale before NewFrame so the slider's
    // most recent value takes effect for the upcoming layout pass.
    // FontGlobalScale stacks on top of the per-font scale ImGui already
    // applies; clamp here so a corrupted ui_state.json can't divide-by-
    // zero the layout.
    {
        float s = g_ui_scale;
        if (s < 0.50f) s = 0.50f;
        if (s > 2.50f) s = 2.50f;
        ImGui::GetIO().FontGlobalScale = s;
    }

    // v0.5.6 mouse-park (safe variant of the cursor-park workaround).
    // When the OS cursor is hidden (game in camera mode after an ALT
    // toggle), force ImGui's notion of the mouse to off-screen so no
    // overlay widget receives invisible hovers. Doesn't touch the OS
    // cursor at all -- when the game re-shows it, ImGui_ImplWin32 sees
    // the next WM_MOUSEMOVE and the position updates normally.
    //
    // Modern ImGui queues input events and applies them inside
    // NewFrame(), so we both queue an "off-screen" mouse-pos event
    // (which the event pump will pick up next frame) AND overwrite
    // io.MousePos directly after NewFrame so this frame's widget
    // hover tests also see the off-screen position.
    bool s_mouse_park_hide_this_frame = false;
    if (!g_clickthrough.load(std::memory_order_acquire)) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0) {
            s_mouse_park_hide_this_frame = true;
            ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
    }

    ImGui::NewFrame();

    if (s_mouse_park_hide_this_frame) {
        // NewFrame applied any queued events; clobber the resolved
        // position too so widgets that check IsMouseHoveringRect or
        // IsItemHovered this frame see "no mouse".
        ImGui::GetIO().MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    }

    // v0.5.2 issue #11: reset-positions hotkey. SetWindowPos by name
    // works on the stored ImGui window state regardless of whether
    // the window is currently rendering, so a closed Hotkeys panel
    // (or a Fight Detail with no fight selected) still gets its
    // position fixed for next open. Use the ###id suffix for the
    // fight-detail window since its visible title changes per fight.
    if (g_reset_positions_request.exchange(false,
                                           std::memory_order_acq_rel)) {
        ImGui::SetWindowPos("minimap",         ImVec2(600,  20));
        ImGui::SetWindowPos("DPS Meter",       ImVec2( 40,  40));
        ImGui::SetWindowPos("Hotkeys",         ImVec2(620, 540));
        ImGui::SetWindowPos("###fight_detail", ImVec2(240, 240));
        ImGui::SetWindowPos("Loot",            ImVec2( 40, 240));
        ImGui::SetWindowPos("Options",         ImVec2(360,  40));
    }

    render_diagnostic_box();
    render_imgui_window();
    render_minimap_window();
    render_keys_window();
    render_fight_detail_window();
    render_compass_strip_window();
    render_boss_timer_window();
    render_mob_alert_banner();
    render_loot_counter_window();
    plugins_tick();
    plugins_render_manager();

    // v0.5.1 smart hover (issue #7): cache whether the cursor is over
    // an actually interactive widget (button, selectable, title bar
    // drag, etc) versus just over a decorative window area (the
    // compass mosaic image, an empty DPS table cell, padding). The
    // wndproc then uses this in place of io.WantCaptureMouse so
    // clicks on decorative pixels pass through to the game. Read
    // BEFORE Render() while item state is still valid for the frame.
    g_overlay_wants_real_input.store(
        ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive(),
        std::memory_order_release);

    ImGui::Render();

    frame.allocator->Reset();
    frame.command_list->Reset(frame.allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = frame.back_buffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->OMSetRenderTargets(1, &frame.rtv_handle, FALSE, nullptr);

    // DCOMP: clear the back buffer to fully transparent. The composition
    // swap chain uses DXGI_ALPHA_MODE_PREMULTIPLIED, so RGB=0 + A=0
    // contributes nothing to the desktop composition - the game shows
    // through wherever ImGui hasn't drawn anything.
    // GAME_SWAPCHAIN: do NOT clear - we draw on top of the game's
    // already-rendered frame in its own back buffer; clearing would
    // erase the game.
    if (overlay_backend() == RenderBackend::Dcomp) {
        constexpr float kClearTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        frame.command_list->ClearRenderTargetView(frame.rtv_handle,
                                                  kClearTransparent,
                                                  0, nullptr);
    }

    ID3D12DescriptorHeap* heaps[] = {g_overlay.srv_heap};
    frame.command_list->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), frame.command_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    frame.command_list->ResourceBarrier(1, &barrier);

    frame.command_list->Close();
    ID3D12CommandList* lists[] = {frame.command_list};
    // v0.4.16: submit on our own DIRECT queue (was the game's captured
    // queue in 0.4.15 and earlier). Decouples our render from the
    // game's command-queue lifecycle entirely.
    g_overlay.queue->ExecuteCommandLists(1, lists);

    g_overlay.next_fence_value++;
    g_overlay.queue->Signal(g_overlay.fence, g_overlay.next_fence_value);
    frame.fence_value = g_overlay.next_fence_value;
    ++s_submitted;
}

// v0.4.13 kill switch (issues #12 / #16 bisection). Set once at
// module init from FAREVER_NO_OVERLAY=1 by dllmain. When engaged
// the overlay never initialises ImGui or submits any command list
// to the game's queue, but damage/hero_state HL reads still run
// in d3d12_hook's Present detour.
std::atomic<bool> g_overlay_killed{false};

// v0.4.14: cached state of both kill switches so the diagnostic
// status box can show which mode the user is in. Set once by
// dllmain via overlay_set_kill_switch_state().
std::atomic<bool> g_diag_no_overlay{false};
std::atomic<bool> g_diag_no_hl_tick{false};
// v0.4.15: cached anticrash state for the same diag box.
std::atomic<bool> g_diag_anticrash{false};

// Diagnostic-mode status box. Bypasses the hero-lock gate so it
// appears even when no_hl_tick.flag is set (hero never locks ->
// minimap + DPS both early-exit -> overlay would otherwise look
// dead). Tiny window pinned top-left. Only rendered when at least
// one kill switch is active.
void render_diagnostic_box() {
    if (!g_diag_no_hl_tick.load() &&
        !g_diag_no_overlay.load() &&
        !g_diag_anticrash.load()) return;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 16, 8, 220));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(180, 140, 60, 240));
    ImGui::PushStyleVar  (ImGuiStyleVar_WindowBorderSize, 1.5f);
    if (ImGui::Begin("##farever_diag", nullptr, flags)) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.40f, 1.0f),
                           "farever-mod v1.2.8");
        ImGui::Separator();
        if (g_diag_no_overlay.load()) {
            ImGui::Text("no_overlay.flag  ACTIVE");
        }
        if (g_diag_no_hl_tick.load()) {
            ImGui::Text("no_hl_tick.flag  ACTIVE");
            ImGui::TextDisabled("minimap + DPS suppressed by design");
        }
        if (g_diag_anticrash.load()) {
            if (hero_state_anticrash_disarmed()) {
                ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f),
                                   "anticrash.flag  DISARMED");
                ImGui::TextDisabled("alloc-hook removed, DPS off");
                ImGui::TextDisabled("minimap polls Player.hero");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                                   "anticrash.flag  ARMED");
                ImGui::TextDisabled("waiting 5 s lock stable, then");
                ImGui::TextDisabled("alloc-hook will be removed");
            }
        }
        ImGui::TextDisabled("diagnostic mode -- delete the flag");
        ImGui::TextDisabled("from data/ + restart for normal use");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

}  // namespace

// Community-plugin theming. Push the overlay's own palette + shape so plugin
// windows and all their widgets render in our gold/brown look, then pop it.
// A plugin's run_render_hooks brackets each plugin's Begin/End with these, so
// a plugin needs no code changes to match. A plugin's own explicit colours
// (imgui.text_colored / draw_*) still win; this only sets the baseline. The
// push and pop counts MUST stay in sync (kPluginThemeColors / kPluginThemeVars).
constexpr int kPluginThemeColors = 28;
constexpr int kPluginThemeVars   = 3;

void overlay_push_plugin_theme() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg,              window_bg_color());
    ImGui::PushStyleColor(ImGuiCol_ChildBg,              IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,              IM_COL32(28, 20, 10, 250));
    ImGui::PushStyleColor(ImGuiCol_Border,               kColBezel);
    ImGui::PushStyleColor(ImGuiCol_TitleBg,              kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,     kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_Text,                 kColText);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,         IM_COL32(150, 130, 95, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              IM_COL32(22, 16, 8, 235));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       kColBtnHover);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,        kColBtnActive);
    ImGui::PushStyleColor(ImGuiCol_Button,               kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,        kColBtnHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,         kColBtnActive);
    ImGui::PushStyleColor(ImGuiCol_Header,               kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,        kColBtnHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,         kColBtnActive);
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            kColIcon);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           kColBezel);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     kColIcon);
    ImGui::PushStyleColor(ImGuiCol_Separator,            kColBezelShadow);
    ImGui::PushStyleColor(ImGuiCol_Tab,                  kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_TabHovered,           kColBtnHover);
    ImGui::PushStyleColor(ImGuiCol_TabSelected,          kColBtnActive);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        kColBtnFill);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, kColBtnHover);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  kColBtnActive);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,     4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  2.0f);
}

void overlay_pop_plugin_theme() {
    ImGui::PopStyleVar(kPluginThemeVars);
    ImGui::PopStyleColor(kPluginThemeColors);
}

bool overlay_plugin_theme_enabled() { return g_plugin_theme.load(); }

// #99: the same atlas-cell blit the DPS skill table uses, exposed for the
// plugin host. Runs on the render thread inside the overlay frame, so it
// can touch the atlas cache and ImGui directly.
bool overlay_draw_atlas_cell(const char* atlas, int cell_x, int cell_y,
                             int cell_size, int cell_w, int cell_h,
                             float draw_px) {
    if (!atlas || !*atlas || cell_size <= 0 || draw_px <= 0.0f) return false;
    if (cell_w <= 0) cell_w = 1;
    if (cell_h <= 0) cell_h = 1;
    LoadedTexture* tex = get_or_load_atlas(atlas);
    if (!tex || !tex->resource || tex->width <= 0 || tex->height <= 0)
        return false;
    float aw  = static_cast<float>(tex->width);
    float ah  = static_cast<float>(tex->height);
    float px0 = static_cast<float>(cell_x * cell_size);
    float py0 = static_cast<float>(cell_y * cell_size);
    float pw  = static_cast<float>(cell_w * cell_size);
    float ph  = static_cast<float>(cell_h * cell_size);
    if (px0 < 0.0f || py0 < 0.0f || px0 + pw > aw || py0 + ph > ah) return false;
    ImGui::Image(static_cast<ImTextureID>(tex->srv_gpu.ptr),
                 ImVec2(draw_px, draw_px),
                 ImVec2(px0 / aw, py0 / ah),
                 ImVec2((px0 + pw) / aw, (py0 + ph) / ah));
    return true;
}

void overlay_kill()    { g_overlay_killed.store(true); }
bool overlay_killed()  { return g_overlay_killed.load(); }
void overlay_set_kill_switch_state(bool no_overlay, bool no_hl_tick) {
    g_diag_no_overlay.store(no_overlay);
    g_diag_no_hl_tick.store(no_hl_tick);
}
void overlay_set_anticrash_state(bool anticrash) {
    g_diag_anticrash.store(anticrash);
}
void overlay_set_standalone_window(bool on) {
    g_overlay_standalone_window.store(on);
}
RenderBackend overlay_backend() {
    return g_render_backend.load(std::memory_order_acquire);
}
void overlay_set_backend(RenderBackend b) {
    g_render_backend.store(b, std::memory_order_release);
}
void overlay_set_window_hwnd(void* hwnd) {
    g_overlay_hwnd_override.store(reinterpret_cast<HWND>(hwnd));
}

// #65: switch the per-character UI-settings profile (see overlay.h).
void overlay_set_profile(const char* key) {
    if (!key || !*key) return;
    std::wstring w;                          // key is ASCII (sanitized)
    for (const char* p = key; *p; ++p) w.push_back((wchar_t)(unsigned char)*p);
    {
        std::lock_guard<std::mutex> lk(g_profile_mu);
        if (g_profile_key == w) return;      // no-op while unchanged
        g_profile_key = w;
    }
    // Seed a new character's file from the shared settings so it starts from
    // the user's current setup. CopyFile with fail-if-exists keeps an existing
    // per-character file untouched; failure (no shared file yet) is harmless,
    // ui_lock_load then just keeps the in-memory state.
    std::wstring pcp = user_data_path((L"ui_state__" + w + L".json").c_str());
    if (GetFileAttributesW(pcp.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring shared = user_data_path(L"ui_state.json");
        CopyFileW(shared.c_str(), pcp.c_str(), TRUE);
    }
    g_profile_reload.store(true, std::memory_order_release);   // render reloads
    logf("overlay: per-character UI profile '%s'", key);
}
void overlay_set_wants_real_input(bool on) {
    g_overlay_wants_real_input.store(on);
}
unsigned overlay_get_toggle_overlay_key() {
    return g_keybinds.toggle_overlay;
}

// Issue #30: walks the most recent ImDrawData and unions the clip
// rects of every non-empty draw command. Returns the bounding box of
// all UI content the user actually sees, in swap-chain pixel coords.
// The caller (overlay_window) passes this as a DXGI dirty rect to
// Present1 so DWM does not have to re-composite the empty regions of
// our full-screen-sized overlay buffer every frame.
bool overlay_get_dirty_rect(int* x, int* y, int* w, int* h) {
    if (!x || !y || !w || !h) return false;
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return false;

    bool any = false;
    float L = 0.0f, T = 0.0f, R = 0.0f, B = 0.0f;
    for (int i = 0; i < dd->CmdListsCount; ++i) {
        const ImDrawList* cl = dd->CmdLists[i];
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd& cmd = cl->CmdBuffer[c];
            if (cmd.ElemCount == 0) continue;
            const float l = cmd.ClipRect.x;
            const float t = cmd.ClipRect.y;
            const float r = cmd.ClipRect.z;
            const float b = cmd.ClipRect.w;
            if (l >= r || t >= b) continue;
            if (!any) {
                L = l; T = t; R = r; B = b;
                any = true;
            } else {
                if (l < L) L = l;
                if (t < T) T = t;
                if (r > R) R = r;
                if (b > B) B = b;
            }
        }
    }
    if (!any) return false;

    // A few-pixel pad guards against antialiased edges and shadows
    // bleeding outside their clip rect (drop shadows, dropdowns).
    constexpr float kPad = 2.0f;
    L -= kPad; T -= kPad; R += kPad; B += kPad;
    if (L < 0.0f) L = 0.0f;
    if (T < 0.0f) T = 0.0f;

    *x = static_cast<int>(L);
    *y = static_cast<int>(T);
    *w = static_cast<int>(R - L);
    *h = static_cast<int>(B - T);
    return *w > 0 && *h > 0;
}

void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* caller_queue) {
    if (g_overlay_killed.load()) {
        // Still emit a minimal heartbeat so #12/#16 retest logs
        // confirm the Present hook is firing while the overlay is
        // suppressed. Otherwise a silent log looks identical to "DLL
        // didn't load".
        static std::uint64_t s_killed_tick = 0;
        if (++s_killed_tick % 600 == 0) {
            logf("overlay: killed @ tick %llu (FAREVER_NO_OVERLAY=1, "
                 "no submit, no ImGui)",
                 static_cast<unsigned long long>(s_killed_tick));
        }
        return;
    }
    if (g_overlay.init_failed) return;
    // v0.4.16: no longer gated on captured-queue presence (we own ours).
    if (!g_overlay_enabled.load()) return;

    bool expected = false;
    if (!g_in_render.compare_exchange_strong(expected, true)) return;
    struct Scope { ~Scope() { g_in_render.store(false); } } scope;

    if (!g_overlay.initialized) {
        if (!overlay_init(swap_chain, caller_queue)) {
            g_overlay.init_failed = true;
            release_all();
            return;
        }
        g_overlay.initialized = true;
    }
    if (swap_chain != g_overlay.owned_swap_chain) return;
    overlay_render(swap_chain, nullptr);
}

void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT /*width*/, UINT /*height*/) {
    if (g_overlay_killed.load()) return;
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    for (auto& f : g_overlay.frames) {
        if (!wait_for_frame(f, 1000)) {
            logf("overlay: resize drain timed out");
        }
        f.fence_value = 0;
    }
    release_frame_targets();
    if (buffer_count != 0 && buffer_count != g_overlay.back_buffer_count) {
        g_overlay.back_buffer_count = buffer_count;
        g_overlay.frames.resize(buffer_count);
    }
}

void overlay_after_resize(IDXGISwapChain3* swap_chain) {
    if (g_overlay_killed.load()) return;
    if (!g_overlay.initialized) return;
    if (swap_chain != g_overlay.owned_swap_chain) return;
    if (!create_back_buffer_targets(swap_chain)) {
        logf("overlay: re-creating RTVs after resize failed");
        g_overlay.init_failed = true;
    }
}

void overlay_shutdown() {
    if (!g_overlay.initialized && !g_overlay.init_failed) return;

    // Release any cursor clip we set as part of the cursor-park
    // workaround. Cheap when no clip was active.
    ClipCursor(nullptr);

    if (g_overlay.orig_wndproc && g_overlay.hwnd) {
        SetWindowLongPtrW(g_overlay.hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_overlay.orig_wndproc));
        g_overlay.orig_wndproc = nullptr;
    }
    if (g_overlay.initialized) {
        plugins_stop();
        for (auto& f : g_overlay.frames) {
            if (!wait_for_frame(f, 1000)) {
                logf("overlay: shutdown drain timed out");
            }
        }
        release_texture(&g_overlay.mosaic);
        release_texture(&g_overlay.poi_atlas);
        release_texture(&g_overlay.player_arrow);
        for (auto& kv : g_atlas_cache) release_texture(&kv.second);
        g_atlas_cache.clear();
        g_next_atlas_slot = kSkillAtlasSlotBase;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    g_overlay.initialized = false;
    g_overlay.init_failed = false;
}

bool overlay_is_dps_tracking_paused() {
    return g_dps_tracking_paused.load(std::memory_order_relaxed);
}

}  // namespace farever
