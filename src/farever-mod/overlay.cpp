// ImGui-DX12 overlay for the unified mod. The DX12 init / fence /
// WndProc machinery follows the same pattern as dpsmeter-dll's
// overlay.cpp — see those comments for the deep rationale. The
// farever-mod-specific bits are at the bottom (render_imgui_window).

#include "overlay.h"
#include "log.h"
#include "aggregator.h"
#include "damage.h"
#include "hero_state.h"
#include "textures.h"
#include "pois.h"
#include "poi_progress.h"
#include "skill_resolve.h"
#include "entity_state.h"

#include <unordered_map>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
// g_overlay_enabled — if a wedged queue forces us off, the user can't
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
constexpr float kZoomMin  = 10.0f;
constexpr float kZoomMax  = 20.0f;
constexpr float kZoomStep = 1.0f;

constexpr float kCompassSizes[3] = { 256.0f, 384.0f, 512.0f };
int g_compass_size_idx = 2;   // start largest

struct PoiFilter {
    bool obelisks   = true;
    bool respawns   = true;
    bool merchants  = true;
    bool dungeons   = true;
    bool activities = true;
    // Collectibles + farming nodes. Off by default — the chest button
    // on the bezel toggles all four at once for quick on/off, and
    // each one can be turned on individually in the filter panel.
    bool chests     = false;
    bool red_orbs   = false;
    bool plants     = false;
    bool ores       = false;
};
PoiFilter g_filter;
bool g_filter_open       = false;
bool g_keys_open         = false;
bool g_compass_collapsed = false;
int  g_selected_fight_id = 0;   // 0 = no fight detail open
std::atomic<bool> g_ui_locked{false};   // minimap lock: pin + size
std::atomic<bool> g_dps_locked{false};  // DPS/fight-history window lock (issue #15: independent)

// Bezel button layout — angle (rad) around the compass center, one
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
};
BezelLayout g_bezel;
int         g_bezel_drag = -1;   // 0..8 while a right-drag is in progress

void ui_lock_load();
void ui_lock_save();

float compass_size_px() { return kCompassSizes[g_compass_size_idx]; }

bool poi_passes_filter(const PoiRow& p) {
    if (std::strcmp(p.kind, "obelisk")   == 0) return g_filter.obelisks;
    if (std::strcmp(p.kind, "respawn")   == 0) return g_filter.respawns;
    if (std::strcmp(p.kind, "merchant")  == 0) return g_filter.merchants;
    if (std::strcmp(p.kind, "dungeon")   == 0) return g_filter.dungeons;
    if (std::strcmp(p.kind, "activity")  == 0) return g_filter.activities;
    if (std::strcmp(p.kind, "chest")     == 0) return g_filter.chests;
    if (std::strcmp(p.kind, "red_orb")   == 0) return g_filter.red_orbs;
    if (std::strcmp(p.kind, "plant")     == 0) return g_filter.plants;
    if (std::strcmp(p.kind, "ore")       == 0) return g_filter.ores;
    return true;
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
    if (calib_extract_double(text, "zoom",     v)) c.zoom                   = (float)v;
    calib_extract_bool(text, "flip_y", c.flip_y);
    if (c.zoom < 1.0f)  c.zoom = 1.0f;
    if (c.zoom > 64.0f) c.zoom = 64.0f;
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
    UINT toggle_dps         = VK_F10;
    UINT toggle_minimap     = VK_F8;
    UINT reset_dps          = VK_F9;
    UINT toggle_clickthru   = VK_F11;   // issues #4 + #7
    UINT toggle_dps_track   = VK_F12;   // issue #13
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

// 0.4.12 (issue #12): when true, the minimap renders only the bezel
// ring + player arrow + buttons -- no mosaic image, no POI markers.
// Set by overlay_render for the kSkeletonFrames following the post-
// transition pause, then cleared. Lets us coast past the danger
// window with a near-zero ImGui draw count.
std::atomic<bool> g_skeleton_minimap{false};

// In-game rebind: the user clicks a slot in the keys panel, we set
// this to the slot id, and the next non-modifier key press in
// overlay_wndproc gets written into that slot (ESC cancels).
enum class RebindSlot : int { None = 0, Dps = 1, Map = 2, Reset = 3,
                              ClickThru = 4, DpsTrack = 5 };
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
    static const std::wstring p = data_path(L"keybinds.json");
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
    g_keybinds = kb;
    logf("overlay: keybinds reloaded (dps=%s map=%s reset=%s "
         "clickthru=%s dps_track=%s)",
         key_to_name(kb.toggle_dps).c_str(),
         key_to_name(kb.toggle_minimap).c_str(),
         key_to_name(kb.reset_dps).c_str(),
         key_to_name(kb.toggle_clickthru).c_str(),
         key_to_name(kb.toggle_dps_track).c_str());
}

const std::wstring& kUiStatePath() {
    static const std::wstring p = data_path(L"ui_state.json");
    return p;
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

void ui_lock_load() {
    std::ifstream f(kUiStatePath());
    if (!f) return;
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    g_ui_locked.store(s.find("\"locked\": true") != std::string::npos ||
                      s.find("\"locked\":true")  != std::string::npos);
    g_dps_locked.store(s.find("\"dps_locked\": true") != std::string::npos ||
                       s.find("\"dps_locked\":true")  != std::string::npos);

    // Bezel angles — each one optional; defaults stay if absent.
    ui_state_extract_float(s, "pin",      g_bezel.pin);
    ui_state_extract_float(s, "size",     g_bezel.size);
    ui_state_extract_float(s, "collapse", g_bezel.collapse);
    ui_state_extract_float(s, "filter",   g_bezel.filter);
    ui_state_extract_float(s, "lock",     g_bezel.lock);
    ui_state_extract_float(s, "keys",     g_bezel.keys);
    ui_state_extract_float(s, "chest",    g_bezel.chest);
    ui_state_extract_float(s, "plus",     g_bezel.plus);
    ui_state_extract_float(s, "minus",    g_bezel.minus);

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
}

void ui_lock_save() {
    std::wstring dir_path = dll_dir() + L"\\data";
    CreateDirectoryW(dir_path.c_str(), nullptr);
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
      << "  \"compass_size\": " << g_compass_size_idx << ",\n"
      << "  \"clickthrough\": " << (g_clickthrough.load() ? "true" : "false") << ",\n"
      << "  \"dps_tracking_paused\": " << (g_dps_tracking_paused.load() ? "true" : "false") << ",\n"
      << "  \"minimap_alpha\": " << g_minimap_alpha << "\n"
      << "}\n";
}

void keybinds_save() {
    std::wstring dir_path = dll_dir() + L"\\data";
    CreateDirectoryW(dir_path.c_str(), nullptr);
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
      << "  \"toggle_dps_track\":   \"" << key_to_name(g_keybinds.toggle_dps_track) << "\"\n"
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

// --- bezel buttons --------------------------------------------------

struct BezelButton {
    ImVec2 center;
    float  radius;
    bool   clicked;
    bool   hovered;
    bool   active;
};

BezelButton bezel_hit(const char* id, ImVec2 center, float bezel_r,
                      float angle_rad, float btn_r) {
    BezelButton b{};
    b.center = ImVec2(center.x + cosf(angle_rad) * bezel_r,
                      center.y + sinf(angle_rad) * bezel_r);
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
    float w = 5.0f, h = 6.0f;
    ImVec2 tl(b.center.x - w, b.center.y - h);
    ImVec2 tr(b.center.x + w, b.center.y - h);
    ImVec2 nl(b.center.x - 1.5f, b.center.y + 1.0f);
    ImVec2 nr(b.center.x + 1.5f, b.center.y + 1.0f);
    ImVec2 sb(b.center.x, b.center.y + h);
    dl->AddLine(tl, tr, c, 1.8f);
    dl->AddLine(tl, nl, c, 1.8f);
    dl->AddLine(tr, nr, c, 1.8f);
    dl->AddLine({nl.x, nl.y}, {nl.x + 1.5f, sb.y}, c, 1.8f);
    dl->AddLine({nr.x, nr.y}, {nr.x - 1.5f, sb.y}, c, 1.8f);
}
void bezel_draw_key(ImDrawList* dl, const BezelButton& b, bool active) {
    bezel_draw_base(dl, b);
    ImU32 c = active ? IM_COL32(255, 255, 200, 255) : kColIcon;
    // bow (round head of the key) on the left
    ImVec2 bow(b.center.x - 3.0f, b.center.y - 1.0f);
    dl->AddCircle(bow, 3.0f, c, 14, 1.6f);
    // shaft to the right
    dl->AddLine({bow.x + 3.0f, bow.y},
                {b.center.x + 5.0f, bow.y}, c, 1.8f);
    // two teeth at the tip
    dl->AddLine({b.center.x + 3.0f, bow.y},
                {b.center.x + 3.0f, bow.y + 3.0f}, c, 1.8f);
    dl->AddLine({b.center.x + 5.0f, bow.y},
                {b.center.x + 5.0f, bow.y + 3.0f}, c, 1.8f);
}
void bezel_draw_lock(ImDrawList* dl, const BezelButton& b, bool locked) {
    bezel_draw_base(dl, b);
    ImU32 c = locked ? IM_COL32(255, 220, 100, 255) : kColIcon;
    // Body of the padlock
    ImVec2 tl(b.center.x - 4.0f, b.center.y);
    ImVec2 br(b.center.x + 4.0f, b.center.y + 4.5f);
    dl->AddRect(tl, br, c, 0.8f, 0, 1.5f);
    // Keyhole
    dl->AddCircleFilled({b.center.x, b.center.y + 2.0f}, 0.9f, c, 8);
    // Shackle — closed loop when locked, open hook when not
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
    // body
    float w = 5.5f, hUp = 4.0f, hDn = 2.5f;
    ImVec2 tl(b.center.x - w, b.center.y - hUp);
    ImVec2 tr(b.center.x + w, b.center.y - hUp);
    ImVec2 bl(b.center.x - w, b.center.y + hDn);
    ImVec2 br(b.center.x + w, b.center.y + hDn);
    dl->AddRect(tl, br, c, 0.5f, 0, 1.6f);
    // lid seam
    dl->AddLine({tl.x, b.center.y - 1.0f},
                {tr.x, b.center.y - 1.0f}, c, 1.4f);
    // lock plate
    dl->AddRectFilled(
        {b.center.x - 1.0f, b.center.y - 1.5f},
        {b.center.x + 1.0f, b.center.y + 1.0f}, c);
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

    BezelButton pin      = bezel_hit("##pin",        center, r, g_bezel.pin,      btn_r);
    BezelButton sizeb    = bezel_hit("##size_cycle", center, r, g_bezel.size,     btn_r);
    BezelButton collapse = bezel_hit("##collapse",   center, r, g_bezel.collapse, btn_r);
    BezelButton filter   = bezel_hit("##filter",     center, r, g_bezel.filter,   btn_r);
    BezelButton lockb    = bezel_hit("##lock",       center, r, g_bezel.lock,     btn_r);
    BezelButton keys     = bezel_hit("##keys",       center, r, g_bezel.keys,     btn_r);
    BezelButton chest    = bezel_hit("##chest",      center, r, g_bezel.chest,    btn_r);
    BezelButton plus     = bezel_hit("##zoom_plus",  center, r, g_bezel.plus,     btn_r);
    BezelButton minus    = bezel_hit("##zoom_minus", center, r, g_bezel.minus,    btn_r);

    auto* dl = ImGui::GetWindowDrawList();
    ViewUV view = compute_view_uv(h.x, h.y, h.locked);

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
    if (!skeleton && g_overlay.mosaic.resource) {
        dl->AddImageRounded((ImTextureID)g_overlay.mosaic.srv_gpu.ptr,
                            p_min, p_max, view.uv0, view.uv1,
                            tint_mosaic, r);
    } else {
        dl->AddCircleFilled(center, r,
                            scale_alpha(IM_COL32(20, 22, 30, 255)), 64);
    }
    dl->AddCircle(center, r,        scale_alpha(kColBezel),       96, 3.0f);
    dl->AddCircle(center, r - 2.0f, scale_alpha(kColBezelShadow), 96, 1.0f);
    dl->AddLine({center.x, p_min.y}, {center.x, p_min.y + 10.0f},
                kColNorth, 2.5f);

    // POIs (clipped to the bezel disc). Skipped entirely in skeleton
    // mode (issue #12) so the post-transition draw call count stays
    // tiny while the game's DX12 driver settles.
    if (!skeleton) {
        const auto& pois = pois_get();
        const float clip_r  = r - 4.0f;
        const float clip_r2 = clip_r * clip_r;
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

        for (const auto& poi : pois) {
            if (!poi_passes_filter(poi)) continue;
            ImVec2 sp_local = player_to_screen(poi.x, poi.y, view, size);
            ImVec2 sp(p_min.x + sp_local.x, p_min.y + sp_local.y);
            float ddx = sp.x - center.x, ddy = sp.y - center.y;
            if (ddx * ddx + ddy * ddy > clip_r2) continue;

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
                // done — collectible glyph drawn
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

            // Right-click target: only collectibles can be toggled,
            // not story POIs (dungeon, obelisk, merchant ...).
            if (std::strcmp(poi.kind, "chest")   == 0 ||
                std::strcmp(poi.kind, "red_orb") == 0) {
                click_cands.push_back({sp, &poi});
            }
        }

        // Right-click handling: priority 1 is bezel-button drag (so
        // the user can rearrange the rim layout), priority 2 is the
        // collectible-toggle. Drag is only enabled while UI is
        // unlocked — locking pins the layout in place.
        bool right_consumed = false;
        if (!g_ui_locked.load()) {
            // Pointer to each angle in g_bezel + button positions we
            // already computed via bezel_hit, ordered to match.
            float* angles[] = { &g_bezel.pin, &g_bezel.size,
                                &g_bezel.collapse, &g_bezel.filter,
                                &g_bezel.lock,    &g_bezel.keys,
                                &g_bezel.chest,   &g_bezel.plus,
                                &g_bezel.minus };
            ImVec2 btn_pos[] = { pin.center, sizeb.center, collapse.center,
                                 filter.center, lockb.center, keys.center,
                                 chest.center, plus.center, minus.center };
            constexpr int kNumBezel = 9;

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
        if (!right_consumed &&
            ImGui::IsMouseHoveringRect(p_min, p_max) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImVec2 m = ImGui::GetMousePos();
            float best = 14.0f * 14.0f;
            const PoiRow* hit = nullptr;
            for (const auto& c : click_cands) {
                float dx = c.sp.x - m.x, dy = c.sp.y - m.y;
                float d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; hit = c.p; }
            }
            if (hit && hit->id[0]) {
                poi_progress_toggle(hit->id);
                logf("poi_progress: toggled '%s' (%s) -> %s",
                     hit->id, hit->kind,
                     poi_progress_is_done(hit->id) ? "done" : "not done");
            }
        }
    }

    if (!h.locked) {
        dl->AddText({center.x - 60.0f, center.y - 6.0f}, kColText,
                    "Waiting for Hero alloc...");
    } else {
        ImVec2 dot_local = player_to_screen(h.x, h.y, view, size);
        ImVec2 dot(p_min.x + dot_local.x, p_min.y + dot_local.y);
        float c = cosf((float)h.rot_z), s = sinf((float)h.rot_z);
        if (g_overlay.player_arrow.resource) {
            float arr =
                (size <= 256.0f) ? 9.0f : (size <= 384.0f) ? 11.0f : 13.0f;
            auto rot = [&](float lx, float ly) {
                return ImVec2(dot.x + lx * c - ly * s,
                              dot.y + lx * s + ly * c);
            };
            ImVec2 p0 = rot(-arr, -arr), p1 = rot(arr, -arr);
            ImVec2 p2 = rot( arr,  arr), p3 = rot(-arr, arr);
            dl->AddImageQuad((ImTextureID)g_overlay.player_arrow.srv_gpu.ptr,
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

    bezel_draw_pin     (dl, pin);
    bezel_draw_size    (dl, sizeb, g_compass_size_idx);
    bezel_draw_collapse(dl, collapse);
    bezel_draw_filter  (dl, filter, g_filter_open);
    bezel_draw_lock    (dl, lockb,  g_ui_locked.load());
    bezel_draw_key     (dl, keys,   g_keys_open);
    bezel_draw_chest   (dl, chest,  any_collectible_on);
    bezel_draw_plus    (dl, plus);
    bezel_draw_minus   (dl, minus);

    if (plus.clicked)  { g_calib.zoom += kZoomStep; if (g_calib.zoom > kZoomMax) g_calib.zoom = kZoomMax; }
    if (minus.clicked) { g_calib.zoom -= kZoomStep; if (g_calib.zoom < kZoomMin) g_calib.zoom = kZoomMin; }
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
    // Off-screen rescue (issue #1): if the saved window position is
    // outside the current viewport, snap it back to a safe spot. Runs
    // every frame but only acts when the check trips.
    {
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        if (window_is_offscreen(wp, ws, vp)) {
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
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg,        kColBtnFill);
        ImGui::PushStyleColor(ImGuiCol_Border,         kColBezel);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(22, 16, 8, 235));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,      kColIcon);
        ImGui::PushStyleColor(ImGuiCol_Text,           kColText);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   4.0f);
        ImGui::BeginChild("##filter_tablet", ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeX |
                              ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::Checkbox("Obelisks",   &g_filter.obelisks);
        ImGui::Checkbox("Respawns",   &g_filter.respawns);
        ImGui::Checkbox("Dungeons",   &g_filter.dungeons);
        ImGui::Checkbox("Merchants",  &g_filter.merchants);
        ImGui::Checkbox("Activities", &g_filter.activities);
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
        ImGui::Checkbox(lbl, &g_filter.chests);
        labelled("Red Orbs", "red_orb", lbl, sizeof(lbl));
        ImGui::Checkbox(lbl, &g_filter.red_orbs);
        // Plants / Ores respawn, so a "done X of Y" counter would be
        // misleading. Plain labels.
        ImGui::Checkbox("Plants", &g_filter.plants);
        ImGui::Checkbox("Ores",   &g_filter.ores);

        // Opacity slider (issue #2). Affects mosaic + bezel ring;
        // the player arrow, POI markers and bezel buttons stay full
        // alpha so they don't fade away. Persists on release.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Opacity");
        ImGui::SetNextItemWidth(140);
        if (ImGui::SliderFloat("##minimap_alpha", &g_minimap_alpha,
                               0.30f, 1.00f, "%.2f")) {
            // dragging -- live preview, save below
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui_lock_save();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void render_fight_detail_window() {
    if (g_selected_fight_id == 0) return;
    if (!hero_state_read().locked) return;

    AggSnapshot snap = aggregator_snapshot();
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

    ImGui::PushStyleColor(ImGuiCol_WindowBg,         kColBtnFill);
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
        // Off-screen rescue (issue #1) — runs inside the Begin block
        // so we only touch ImGui state when the window actually exists.
        ImVec2 vp = ImGui::GetIO().DisplaySize;
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        if (window_is_offscreen(wp, ws, vp)) {
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
        ImGui::TextDisabled("%d hits", f->hit_count);

        ImGui::Spacing();

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
            ImGui::TableSetupColumn("DPS",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn("%",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
            ImGui::TableHeadersRow();

            const float kIconPx = 22.0f;
            for (std::size_t i = 0; i < f->row_count; ++i) {
                const SkillRow& r = f->rows[i];
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
                double row_dps = r.total / f->duration_sec;
                ImGui::Text("%.1f", row_dps);

                ImGui::TableSetColumnIndex(6);
                float pct = f->total_damage > 0.001
                    ? 100.0f * (float)(r.total / f->total_damage) : 0.0f;
                ImGui::Text("%.0f%%", pct);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open) g_selected_fight_id = 0;

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(10);
}

void render_keys_window() {
    if (!g_keys_open) return;
    if (!hero_state_read().locked) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      kColBtnFill);
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
        if (window_is_offscreen(wp, ws, vp)) {
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
            // that dies at the end of the full expression — Button
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
        row("Toggle DPS",       RebindSlot::Dps,       g_keybinds.toggle_dps);
        row("Toggle Minimap",   RebindSlot::Map,       g_keybinds.toggle_minimap);
        row("Reset DPS",        RebindSlot::Reset,     g_keybinds.reset_dps);
        row("Click-through",    RebindSlot::ClickThru, g_keybinds.toggle_clickthru);
        row("Pause DPS track",  RebindSlot::DpsTrack,  g_keybinds.toggle_dps_track);

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
            // Skip pure modifier keys — wait for the real key.
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
            logf("overlay: %s DPS -> %s",
                 key_to_name(vk).c_str(), now ? "VISIBLE" : "HIDDEN");
            return 0;
        }
        if (vk == g_keybinds.toggle_minimap) {
            bool now = !g_minimap_visible.load();
            g_minimap_visible.store(now);
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
        const bool ct = g_clickthrough.load();
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

        if (ct && is_mouse_button) {
            // Hide click / wheel events from ImGui entirely. Mouse
            // moves still flow through so hover/POI scaling work.
            return CallWindowProcW(g_overlay.orig_wndproc, hwnd, msg, wp, lp);
        }

        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (!ct) {
            // v0.5.1 smart hover (issue #7): only consume the click
            // when the cursor is over an ACTUAL interactive widget
            // (button, selectable, title bar drag) — not just over
            // an ImGui window's decorative pixels (the compass
            // bezel + mosaic image, empty DPS table area, etc).
            // io.WantCaptureMouse was the old check but it's too
            // coarse: it returned true for any cursor pos over any
            // ImGui window, eating clicks meant for the game world
            // behind the overlay. g_overlay_wants_real_input is set
            // each frame from IsAnyItemHovered + IsAnyItemActive.
            if (g_overlay_wants_real_input.load(std::memory_order_acquire)) {
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
    // — required because the swap chain is bound to one queue at
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
    // UTF-8 copy. Path is <dll dir>\data\farever_layout.ini — a
    // dedicated file so we never clash with the game's own imgui.ini.
    {
        std::wstring dir = dll_dir() + L"\\data";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring wpath = dir + L"\\farever_layout.ini";
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
    // wndproc intercept them first — it would eat clicks before our
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

    // Minimap assets — optional. Failure logs but doesn't abort init
    // (the DPS meter still works without the compass background).
    std::wstring mosaic_p = data_path(L"maps\\W1_Siagarta.preview.png");
    if (!load_texture_from_file(
            g_overlay.device, g_overlay.srv_heap, 1,
            mosaic_p.c_str(), &g_overlay.mosaic)) {
        logf("overlay: mosaic load failed; minimap will use solid bg");
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
    poi_progress_load();
    ui_lock_load();

    logf("overlay: DX12+ImGui init OK (hwnd=%p, buffers=%u, fmt=%d)",
         g_overlay.hwnd, g_overlay.back_buffer_count,
         static_cast<int>(g_overlay.rt_format));
    return true;
}

void render_imgui_window() {
    // The aggregator runs unconditionally — pulls drain damage events
    // every frame so totals stay correct even with the window hidden.
    aggregator_tick();
    if (!g_dps_visible.load()) return;
    // Hold the UI back until the local Hero is locked (= we know
    // which character is the user). No point showing 0-damage rows
    // before the world is even visible.
    if (!hero_state_read().locked) return;
    AggSnapshot snap = aggregator_snapshot();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 360), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,    kColBtnFill);
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
        if (window_is_offscreen(wp, ws, vp)) {
            logf("overlay: DPS meter was off-screen, snapping back");
            ImGui::SetWindowPos(ImVec2(40, 40));
        }
    }

    // Pick a layout tier from current content width — drives column
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
    // status line. Toggles the DPS-window-only lock (issue #15) —
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
        } else if (tier == 1) {
            ImGui::Text("total %.0f", snap.total_damage);
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "DPS %.1f", snap.dps);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "%.0f DPS", snap.dps);
        }
    } else {
        // No fight yet — just keep the line height occupied by the
        // combat dot; no status text.
        ImGui::NewLine();
    }

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
                case COL_DPS:   hdr = "DPS";   wgt = 0.85f; break;
                case COL_PCT:   hdr = "%";     wgt = 0.55f; break;
            }
            ImGui::TableSetupColumn(
                hdr, ImGuiTableColumnFlags_WidthStretch, wgt);
        }
        if (tier >= 2) ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < snap.row_count; ++i) {
            const SkillRow& r = snap.rows[i];
            ImGui::TableNextRow();

            float crit_pct = r.hit_count > 0
                ? 100.0f * (float)r.crit_count / (float)r.hit_count : 0.0f;
            double row_dps = (snap.elapsed_sec > 0.001)
                ? r.total / snap.elapsed_sec : 0.0;
            float pct_total = snap.total_damage > 0.001
                ? 100.0f * (float)(r.total / snap.total_damage) : 0.0f;

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
            if (ImGui::BeginTable("##hist", 5, hist_flags,
                                  ImVec2(0.0f, history_reserve - kLineH))) {
                ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthStretch, 0.70f);
                ImGui::TableSetupColumn("Dur",     ImGuiTableColumnFlags_WidthStretch, 0.55f);
                ImGui::TableSetupColumn("Total",   ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("DPS",     ImGuiTableColumnFlags_WidthStretch, 0.85f);
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

void overlay_render(IDXGISwapChain3* swap_chain, ID3D12CommandQueue* /*unused_v0_4_16*/) {
    keybinds_maybe_reload();

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

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    render_diagnostic_box();
    render_imgui_window();
    render_minimap_window();
    render_keys_window();
    render_fight_detail_window();

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

    // v0.4.17 Option B: clear back buffer to fully transparent.
    // The DCOMP composition swap chain uses
    // DXGI_ALPHA_MODE_PREMULTIPLIED, so RGB=0 + A=0 contributes
    // nothing to the desktop composition — the game shows through
    // wherever ImGui hasn't drawn anything.
    constexpr float kClearTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    frame.command_list->ClearRenderTargetView(frame.rtv_handle,
                                              kClearTransparent,
                                              0, nullptr);

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
                           "farever-mod v0.5");
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
void overlay_set_window_hwnd(void* hwnd) {
    g_overlay_hwnd_override.store(reinterpret_cast<HWND>(hwnd));
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

    if (g_overlay.orig_wndproc && g_overlay.hwnd) {
        SetWindowLongPtrW(g_overlay.hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_overlay.orig_wndproc));
        g_overlay.orig_wndproc = nullptr;
    }
    if (g_overlay.initialized) {
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
