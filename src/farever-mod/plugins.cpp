// Lua-based plugin system. See plugins.h for the user-facing contract.
//
// Each plugin owns its own lua_State so a crash in one plugin doesn't
// taint the others. The runtime is fully sandboxed — io / os.execute /
// require / dofile / loadfile / debug are all nil'd out before any
// plugin code runs. Plugins read game state through the bound
// `farever` table (read-only) and draw through the bound `imgui` table.
//
// Threading: every Lua call happens on the render thread. The event
// emit_* helpers may be called from other threads — they just push
// into a mutex-guarded queue, drained on the next plugins_tick.

#include "plugins.h"
#include "log.h"
#include "hero_state.h"
#include "aggregator.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "imgui.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace farever {
namespace {

// Plugin search path is relative to the directory the DLL lives in.
// Mirrors overlay.cpp's dll_dir(); kept local so plugins.cpp does
// not depend on overlay internals.
std::wstring plugins_dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&plugins_dll_dir),
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

struct Plugin {
    std::string  name;            // filename without extension
    std::wstring path;            // full path to the .lua file
    lua_State*   L          = nullptr;
    FILETIME     mtime{};
    bool         has_render = false;
    bool         has_event  = false;
    bool         init_ok    = false;
    std::string  last_error;
};

struct PluginEvent {
    enum class Kind { HeroLocked, DamageDealt, FightStart, FightEnd };
    Kind        k = Kind::HeroLocked;
    std::string skill;
    std::string top_skill;
    double      amount   = 0.0;
    double      duration = 0.0;
    double      dps      = 0.0;
    int         fight_id = 0;
    bool        is_crit  = false;
    bool        is_kill  = false;
};

std::vector<Plugin>      g_plugins;
std::mutex               g_event_mtx;
std::vector<PluginEvent> g_event_queue;
std::atomic<bool>        g_manager_visible{false};
std::chrono::steady_clock::time_point g_last_mtime_check{};
constexpr auto           kMtimeInterval = std::chrono::seconds(1);

// === Toast queue ===
//
// Plugins call farever.toast(msg, [duration]) to flash a center-screen
// message. Drawn from plugins_render_toasts (called from overlay
// render after all plugin windows). Independent per-mod, not per-
// plugin, because a centered toast naturally serializes.
struct Toast {
    std::string msg;
    double      remaining = 2.0;   // seconds
    double      total     = 2.0;
};
std::mutex         g_toast_mtx;
std::vector<Toast> g_toasts;
std::chrono::steady_clock::time_point g_last_toast_tick{};

// === Store file path helper ===
std::wstring store_path_for(const std::string& plugin_name) {
    std::wstring dir = plugins_dll_dir() + L"\\data\\plugins";
    std::wstring path = dir + L"\\";
    // utf8 -> wide
    int wn = MultiByteToWideChar(CP_UTF8, 0, plugin_name.c_str(), -1,
                                 nullptr, 0);
    std::wstring wname(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, plugin_name.c_str(), -1,
                        wname.data(), wn);
    if (!wname.empty() && wname.back() == L'\0') wname.pop_back();
    path += wname;
    path += L".store.lua";
    return path;
}

// === Sandbox ===
//
// luaL_openlibs gives us base + string + table + math + io + os + ...
// io is removed entirely. os keeps date/time/clock/difftime (useful
// for plugin timing) and drops execute/remove/rename/exit/getenv. The
// loader functions (require/dofile/loadfile/load) and the debug
// library go away too so a plugin cannot read arbitrary files or
// rewrite globals at runtime.
void apply_sandbox(lua_State* L) {
    lua_pushnil(L); lua_setglobal(L, "io");

    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        static const char* const kill[] = {
            "execute", "remove", "rename", "setlocale",
            "tmpname", "exit", "getenv", nullptr
        };
        for (int i = 0; kill[i]; ++i) {
            lua_pushnil(L);
            lua_setfield(L, -2, kill[i]);
        }
    }
    lua_pop(L, 1);

    lua_pushnil(L); lua_setglobal(L, "package");
    lua_pushnil(L); lua_setglobal(L, "require");
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");
    lua_pushnil(L); lua_setglobal(L, "debug");
}

// === farever.* API ===

int api_player_x(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushnumber(L, h.x);
    return 1;
}
int api_player_y(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushnumber(L, h.y);
    return 1;
}
int api_player_z(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushnumber(L, h.z);
    return 1;
}
int api_player_rot(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushnumber(L, h.rot_z);
    return 1;
}
int api_player_locked(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushboolean(L, h.locked ? 1 : 0);
    return 1;
}
int api_player_in_combat(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushboolean(L, h.in_combat ? 1 : 0);
    return 1;
}

int api_dps_current(lua_State* L) {
    AggSnapshot s = aggregator_snapshot();
    lua_pushnumber(L, s.dps);
    return 1;
}
int api_dps_total(lua_State* L) {
    AggSnapshot s = aggregator_snapshot();
    lua_pushnumber(L, s.total_damage);
    return 1;
}
int api_dps_elapsed(lua_State* L) {
    AggSnapshot s = aggregator_snapshot();
    lua_pushnumber(L, s.elapsed_sec);
    return 1;
}
int api_dps_in_combat(lua_State* L) {
    AggSnapshot s = aggregator_snapshot();
    lua_pushboolean(L, s.in_combat ? 1 : 0);
    return 1;
}

int api_log_info(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    // Pull the plugin name out of the registry so each log line is
    // attributable. plugins_tick stashed it before calling into Lua.
    lua_getfield(L, LUA_REGISTRYINDEX, "fv_plugin_name");
    const char* name = lua_tostring(L, -1);
    logf("[plugin %s] %s", name ? name : "?", msg);
    lua_pop(L, 1);
    return 0;
}

// === farever.store ===
//
// Each plugin gets a private table at REGISTRY["fv_store"], persisted
// to data/plugins/<name>.store.lua as a `return { ... }` chunk. The
// chunk is loaded with luaL_loadbuffer (no filesystem access from
// inside the sandbox) so the format is just Lua. Top-level keys are
// strings; values are string / number / boolean / nil. Nested tables
// are not supported in v1.1 — plugins should compose via
// string.format if they need richer data.

std::string store_escape(const char* s) {
    std::string out;
    out.reserve(std::strlen(s) + 4);
    for (const char* p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%03d", (int)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

void store_save(lua_State* L, const std::string& plugin_name) {
    // store table on top
    lua_getfield(L, LUA_REGISTRYINDEX, "fv_store");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    std::string out = "return {\n";
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        // -2 = key, -1 = value
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }
        const char* key = lua_tostring(L, -2);
        out += "  [\"";
        out += store_escape(key);
        out += "\"] = ";
        switch (lua_type(L, -1)) {
            case LUA_TBOOLEAN:
                out += lua_toboolean(L, -1) ? "true" : "false";
                break;
            case LUA_TNUMBER: {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.15g",
                              lua_tonumber(L, -1));
                out += buf;
                break;
            }
            case LUA_TSTRING:
                out += "\"";
                out += store_escape(lua_tostring(L, -1));
                out += "\"";
                break;
            default:
                out += "nil";
                break;
        }
        out += ",\n";
        lua_pop(L, 1);  // pop value, keep key for next iter
    }
    lua_pop(L, 1);  // pop store table
    out += "}\n";

    std::wstring path = store_path_for(plugin_name);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logf("plugins: '%s' store save failed (CreateFile)",
             plugin_name.c_str());
        return;
    }
    DWORD written = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
}

void store_load(lua_State* L, const std::string& plugin_name) {
    std::wstring path = store_path_for(plugin_name);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // No file yet — start with empty store table.
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "fv_store");
        return;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string buf((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);

    if (luaL_loadbuffer(L, buf.data(), buf.size(), "store") != LUA_OK) {
        logf("plugins: '%s' store parse error: %s",
             plugin_name.c_str(),
             lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "fv_store");
        return;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        logf("plugins: '%s' store eval error: %s",
             plugin_name.c_str(),
             lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "fv_store");
        return;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "fv_store");
}

int api_store_get(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    // Default value is whatever's at index 2; if nothing, push nil.
    if (lua_gettop(L) < 2) lua_pushnil(L);
    lua_getfield(L, LUA_REGISTRYINDEX, "fv_store");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // default already on stack
        return 1;
    }
    lua_getfield(L, -1, key);
    if (lua_isnil(L, -1)) {
        // return default (index 2)
        lua_pop(L, 2);  // nil, store
        lua_pushvalue(L, 2);
        return 1;
    }
    // Found value: leave it on top, drop the store table beneath.
    lua_remove(L, -2);
    return 1;
}

int api_store_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    // value at index 2 (may be nil, string, number, boolean)
    int vt = lua_type(L, 2);
    if (vt != LUA_TNIL && vt != LUA_TBOOLEAN &&
        vt != LUA_TNUMBER && vt != LUA_TSTRING) {
        return luaL_error(L, "farever.store.set: value must be nil, "
                             "boolean, number, or string (got %s)",
                          lua_typename(L, vt));
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "fv_store");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "fv_store");
    }
    lua_pushvalue(L, 2);     // value
    lua_setfield(L, -2, key);
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "fv_plugin_name");
    const char* name = lua_tostring(L, -1);
    std::string nm = name ? name : "anon";
    lua_pop(L, 1);
    store_save(L, nm);
    return 0;
}

// === farever.toast ===
int api_toast(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    double dur = luaL_optnumber(L, 2, 2.0);
    if (dur < 0.1) dur = 0.1;
    if (dur > 30.0) dur = 30.0;
    Toast t;
    t.msg = msg;
    t.remaining = dur;
    t.total     = dur;
    std::lock_guard<std::mutex> lk(g_toast_mtx);
    g_toasts.push_back(std::move(t));
    return 0;
}

// === imgui.* API (curated subset) ===

int api_imgui_text(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    ImGui::TextUnformatted(s);
    return 0;
}
int api_imgui_text_colored(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float g = (float)luaL_checknumber(L, 2);
    float b = (float)luaL_checknumber(L, 3);
    float a = (float)luaL_optnumber(L, 4, 1.0);
    const char* s = luaL_checkstring(L, 5);
    ImGui::TextColored(ImVec4(r, g, b, a), "%s", s);
    return 0;
}
int api_imgui_button(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    lua_pushboolean(L, ImGui::Button(s) ? 1 : 0);
    return 1;
}
int api_imgui_checkbox(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    bool val = lua_toboolean(L, 2) != 0;
    bool changed = ImGui::Checkbox(label, &val);
    lua_pushboolean(L, val ? 1 : 0);
    lua_pushboolean(L, changed ? 1 : 0);
    return 2;
}
int api_imgui_slider_float(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float val = (float)luaL_checknumber(L, 2);
    float min = (float)luaL_checknumber(L, 3);
    float max = (float)luaL_checknumber(L, 4);
    bool changed = ImGui::SliderFloat(label, &val, min, max);
    lua_pushnumber(L, val);
    lua_pushboolean(L, changed ? 1 : 0);
    return 2;
}
int api_imgui_separator(lua_State* /*L*/) {
    ImGui::Separator();
    return 0;
}
int api_imgui_same_line(lua_State* /*L*/) {
    ImGui::SameLine();
    return 0;
}
int api_imgui_spacing(lua_State* /*L*/) {
    ImGui::Spacing();
    return 0;
}
int api_imgui_progress(lua_State* L) {
    float v = (float)luaL_checknumber(L, 1);
    const char* overlay = lua_tostring(L, 2);
    ImGui::ProgressBar(v, ImVec2(-1, 0), overlay);
    return 0;
}

int api_imgui_input_text(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    const char* cur   = luaL_optstring(L, 2, "");
    char buf[256] = {0};
    std::strncpy(buf, cur, sizeof(buf) - 1);
    bool changed = ImGui::InputText(label, buf, sizeof(buf));
    lua_pushstring(L, buf);
    lua_pushboolean(L, changed ? 1 : 0);
    return 2;
}

int api_imgui_combo(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    int  cur_lua      = (int)luaL_checkinteger(L, 2);    // 1-based for Lua
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    int n = (int)lua_rawlen(L, 3);
    items.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        items.emplace_back(lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    int cur = cur_lua - 1;
    if (cur < 0) cur = 0;
    if (cur >= (int)items.size()) cur = (int)items.size() - 1;
    std::vector<const char*> raw;
    raw.reserve(items.size());
    for (auto& s : items) raw.push_back(s.c_str());
    bool changed = ImGui::Combo(label, &cur, raw.data(), (int)raw.size());
    lua_pushinteger(L, cur + 1);                    // 1-based back to Lua
    lua_pushboolean(L, changed ? 1 : 0);
    return 2;
}

int api_imgui_color_edit(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float col[3] = {
        (float)luaL_checknumber(L, 2),
        (float)luaL_checknumber(L, 3),
        (float)luaL_checknumber(L, 4),
    };
    bool changed = ImGui::ColorEdit3(label, col);
    lua_pushnumber(L, col[0]);
    lua_pushnumber(L, col[1]);
    lua_pushnumber(L, col[2]);
    lua_pushboolean(L, changed ? 1 : 0);
    return 4;
}

int api_imgui_drag_float(lua_State* L) {
    const char* label = luaL_checkstring(L, 1);
    float v     = (float)luaL_checknumber(L, 2);
    float speed = (float)luaL_optnumber(L, 3, 1.0);
    float lo    = (float)luaL_optnumber(L, 4, 0.0);
    float hi    = (float)luaL_optnumber(L, 5, 0.0);
    bool changed = ImGui::DragFloat(label, &v, speed, lo, hi);
    lua_pushnumber(L, v);
    lua_pushboolean(L, changed ? 1 : 0);
    return 2;
}

void install_api(lua_State* L) {
    lua_newtable(L);  // farever

    lua_newtable(L);  // farever.player
    lua_pushcfunction(L, api_player_x);         lua_setfield(L, -2, "x");
    lua_pushcfunction(L, api_player_y);         lua_setfield(L, -2, "y");
    lua_pushcfunction(L, api_player_z);         lua_setfield(L, -2, "z");
    lua_pushcfunction(L, api_player_rot);       lua_setfield(L, -2, "rot_z");
    lua_pushcfunction(L, api_player_locked);    lua_setfield(L, -2, "locked");
    lua_pushcfunction(L, api_player_in_combat); lua_setfield(L, -2, "in_combat");
    lua_setfield(L, -2, "player");

    lua_newtable(L);  // farever.dps
    lua_pushcfunction(L, api_dps_current);   lua_setfield(L, -2, "current");
    lua_pushcfunction(L, api_dps_total);     lua_setfield(L, -2, "total");
    lua_pushcfunction(L, api_dps_elapsed);   lua_setfield(L, -2, "elapsed");
    lua_pushcfunction(L, api_dps_in_combat); lua_setfield(L, -2, "in_combat");
    lua_setfield(L, -2, "dps");

    lua_newtable(L);  // farever.log
    lua_pushcfunction(L, api_log_info); lua_setfield(L, -2, "info");
    lua_pushcfunction(L, api_log_info); lua_setfield(L, -2, "warn");
    lua_setfield(L, -2, "log");

    lua_newtable(L);  // farever.store
    lua_pushcfunction(L, api_store_get); lua_setfield(L, -2, "get");
    lua_pushcfunction(L, api_store_set); lua_setfield(L, -2, "set");
    lua_setfield(L, -2, "store");

    lua_pushcfunction(L, api_toast); lua_setfield(L, -2, "toast");

    lua_setglobal(L, "farever");

    lua_newtable(L);  // imgui
    lua_pushcfunction(L, api_imgui_text);         lua_setfield(L, -2, "text");
    lua_pushcfunction(L, api_imgui_text_colored); lua_setfield(L, -2, "text_colored");
    lua_pushcfunction(L, api_imgui_button);       lua_setfield(L, -2, "button");
    lua_pushcfunction(L, api_imgui_checkbox);     lua_setfield(L, -2, "checkbox");
    lua_pushcfunction(L, api_imgui_slider_float); lua_setfield(L, -2, "slider_float");
    lua_pushcfunction(L, api_imgui_separator);    lua_setfield(L, -2, "separator");
    lua_pushcfunction(L, api_imgui_same_line);    lua_setfield(L, -2, "same_line");
    lua_pushcfunction(L, api_imgui_spacing);      lua_setfield(L, -2, "spacing");
    lua_pushcfunction(L, api_imgui_progress);     lua_setfield(L, -2, "progress");
    lua_pushcfunction(L, api_imgui_input_text);   lua_setfield(L, -2, "input_text");
    lua_pushcfunction(L, api_imgui_combo);        lua_setfield(L, -2, "combo");
    lua_pushcfunction(L, api_imgui_color_edit);   lua_setfield(L, -2, "color_edit");
    lua_pushcfunction(L, api_imgui_drag_float);   lua_setfield(L, -2, "drag_float");
    lua_setglobal(L, "imgui");
}

bool get_mtime(const std::wstring& path, FILETIME* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = GetFileTime(h, nullptr, nullptr, out) != 0;
    CloseHandle(h);
    return ok;
}

bool mtime_changed(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime != b.dwLowDateTime ||
           a.dwHighDateTime != b.dwHighDateTime;
}

bool load_plugin(Plugin& p) {
    if (p.L) { lua_close(p.L); p.L = nullptr; }
    p.L = luaL_newstate();
    if (!p.L) { p.last_error = "luaL_newstate failed"; return false; }
    luaL_openlibs(p.L);
    apply_sandbox(p.L);
    install_api(p.L);

    // Stash the plugin name in the registry so the bound log function
    // can prefix log lines with it.
    lua_pushstring(p.L, p.name.c_str());
    lua_setfield(p.L, LUA_REGISTRYINDEX, "fv_plugin_name");

    // Persistent store loads from data/plugins/<name>.store.lua if it
    // exists, otherwise starts empty. Populates REGISTRY["fv_store"].
    store_load(p.L, p.name);

    char path_utf8[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, p.path.c_str(), -1,
                        path_utf8, sizeof(path_utf8), nullptr, nullptr);
    if (luaL_dofile(p.L, path_utf8) != LUA_OK) {
        p.last_error = lua_tostring(p.L, -1);
        logf("plugins: '%s' load error: %s",
             p.name.c_str(), p.last_error.c_str());
        return false;
    }

    lua_getglobal(p.L, "on_render");
    p.has_render = lua_isfunction(p.L, -1); lua_pop(p.L, 1);
    lua_getglobal(p.L, "on_event");
    p.has_event = lua_isfunction(p.L, -1); lua_pop(p.L, 1);

    lua_getglobal(p.L, "on_init");
    if (lua_isfunction(p.L, -1)) {
        if (lua_pcall(p.L, 0, 0, 0) != LUA_OK) {
            p.last_error = lua_tostring(p.L, -1);
            logf("plugins: '%s' on_init error: %s",
                 p.name.c_str(), p.last_error.c_str());
            lua_pop(p.L, 1);
            return false;
        }
    } else {
        lua_pop(p.L, 1);
    }

    p.last_error.clear();
    p.init_ok = true;
    logf("plugins: '%s' loaded (render=%d event=%d)",
         p.name.c_str(), p.has_render ? 1 : 0, p.has_event ? 1 : 0);
    return true;
}

void scan_plugins_dir() {
    std::wstring dir = plugins_dll_dir() + L"\\data\\plugins";
    std::wstring pattern = dir + L"\\*.lua";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf("plugins: no plugins folder or no .lua files at %ls", dir.c_str());
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        Plugin p;
        p.path = dir + L"\\" + fd.cFileName;

        // Filename without .lua, utf8
        std::wstring stem = fd.cFileName;
        auto dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
        char name8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), -1,
                            name8, sizeof(name8) - 1, nullptr, nullptr);
        p.name = name8;

        get_mtime(p.path, &p.mtime);
        load_plugin(p);
        g_plugins.push_back(std::move(p));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void check_hot_reload() {
    auto now = std::chrono::steady_clock::now();
    if (now - g_last_mtime_check < kMtimeInterval) return;
    g_last_mtime_check = now;

    for (auto& p : g_plugins) {
        FILETIME current{};
        if (!get_mtime(p.path, &current)) continue;
        if (!mtime_changed(p.mtime, current)) continue;
        p.mtime = current;
        logf("plugins: '%s' file changed, reloading", p.name.c_str());
        load_plugin(p);
    }
}

void push_event_table(lua_State* L, const PluginEvent& ev) {
    lua_newtable(L);
    switch (ev.k) {
        case PluginEvent::Kind::HeroLocked:
            // empty table; presence of the event itself is the signal
            break;
        case PluginEvent::Kind::DamageDealt:
            lua_pushstring(L, ev.skill.c_str()); lua_setfield(L, -2, "skill");
            lua_pushnumber(L, ev.amount);        lua_setfield(L, -2, "amount");
            lua_pushboolean(L, ev.is_crit);      lua_setfield(L, -2, "is_crit");
            lua_pushboolean(L, ev.is_kill);      lua_setfield(L, -2, "is_kill");
            break;
        case PluginEvent::Kind::FightStart:
            lua_pushinteger(L, ev.fight_id); lua_setfield(L, -2, "fight_id");
            break;
        case PluginEvent::Kind::FightEnd:
            lua_pushinteger(L, ev.fight_id);         lua_setfield(L, -2, "fight_id");
            lua_pushnumber(L, ev.duration);          lua_setfield(L, -2, "duration");
            lua_pushnumber(L, ev.amount);            lua_setfield(L, -2, "total_damage");
            lua_pushnumber(L, ev.dps);               lua_setfield(L, -2, "dps");
            lua_pushstring(L, ev.top_skill.c_str()); lua_setfield(L, -2, "top_skill");
            break;
    }
}

const char* event_name(PluginEvent::Kind k) {
    switch (k) {
        case PluginEvent::Kind::HeroLocked:  return "hero_locked";
        case PluginEvent::Kind::DamageDealt: return "damage_dealt";
        case PluginEvent::Kind::FightStart:  return "fight_start";
        case PluginEvent::Kind::FightEnd:    return "fight_end";
    }
    return "?";
}

void dispatch_events() {
    std::vector<PluginEvent> drained;
    {
        std::lock_guard<std::mutex> lk(g_event_mtx);
        drained.swap(g_event_queue);
    }
    if (drained.empty()) return;

    for (auto& p : g_plugins) {
        if (!p.init_ok || !p.has_event || !p.L) continue;
        for (const auto& ev : drained) {
            lua_getglobal(p.L, "on_event");
            if (!lua_isfunction(p.L, -1)) { lua_pop(p.L, 1); continue; }
            lua_pushstring(p.L, event_name(ev.k));
            push_event_table(p.L, ev);
            if (lua_pcall(p.L, 2, 0, 0) != LUA_OK) {
                p.last_error = lua_tostring(p.L, -1);
                logf("plugins: '%s' on_event error: %s",
                     p.name.c_str(), p.last_error.c_str());
                lua_pop(p.L, 1);
            }
        }
    }
}

// Tick toast timers and draw any active ones centered on screen.
// Each toast fades out over its last second of life. Drawn as a
// standalone ImGui window (NoBackground, NoTitle, NoInputs) positioned
// at the top-center of the display. Stacks multiple toasts vertically.
void render_toasts(double dt) {
    std::vector<Toast> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_toast_mtx);
        if (g_toasts.empty()) return;
        for (auto& t : g_toasts) t.remaining -= dt;
        g_toasts.erase(
            std::remove_if(g_toasts.begin(), g_toasts.end(),
                           [](const Toast& t) { return t.remaining <= 0.0; }),
            g_toasts.end());
        snapshot = g_toasts;
    }
    if (snapshot.empty()) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return;
    float cx = vp->WorkPos.x + vp->WorkSize.x * 0.5f;
    float top = vp->WorkPos.y + vp->WorkSize.y * 0.12f;

    ImGui::PushFont(ImGui::GetIO().FontDefault);
    for (const auto& t : snapshot) {
        float fade = (float)(t.remaining / (t.total > 0.0 ? t.total : 1.0));
        if (fade > 1.0f) fade = 1.0f;
        // Hold full opacity for first ~70%, fade over the rest.
        float a = fade > 0.30f ? 1.0f : (fade / 0.30f);
        ImU32 col_text   = IM_COL32(255, 240, 200, (int)(255 * a));
        ImU32 col_shadow = IM_COL32(0,   0,   0,   (int)(180 * a));

        ImVec2 sz = ImGui::CalcTextSize(t.msg.c_str());
        // 2x scaled via DrawList::AddText: re-measure at scaled size.
        const float scale = 1.6f;
        ImVec2 pos(cx - (sz.x * scale) * 0.5f, top);

        ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
        ImFont* fnt = ImGui::GetFont();
        float fsz   = ImGui::GetFontSize() * scale;
        dl->AddText(fnt, fsz, ImVec2(pos.x + 2, pos.y + 2),
                    col_shadow, t.msg.c_str());
        dl->AddText(fnt, fsz, pos, col_text, t.msg.c_str());

        top += sz.y * scale + 6.0f;
    }
    ImGui::PopFont();
}

void run_render_hooks() {
    for (auto& p : g_plugins) {
        if (!p.init_ok || !p.has_render || !p.L) continue;
        // Open one ImGui window per plugin. Plugin author cannot
        // begin / end windows themselves, keeping the surface area
        // small. Plugin name doubles as the ImGui window ID, so two
        // plugins with the same filename would collide — but a path
        // scan would have to land on two same-named files for that to
        // happen, which the .lua extension + FindFirstFile pattern
        // prevents.
        bool open = true;
        ImGuiWindowFlags wf = ImGuiWindowFlags_AlwaysAutoResize;
        if (!ImGui::Begin(p.name.c_str(), &open, wf)) {
            ImGui::End();
            continue;
        }
        lua_getglobal(p.L, "on_render");
        if (lua_isfunction(p.L, -1)) {
            if (lua_pcall(p.L, 0, 0, 0) != LUA_OK) {
                p.last_error = lua_tostring(p.L, -1);
                logf("plugins: '%s' on_render error: %s",
                     p.name.c_str(), p.last_error.c_str());
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                   "error: %s", p.last_error.c_str());
                lua_pop(p.L, 1);
            }
        } else {
            lua_pop(p.L, 1);
        }
        ImGui::End();
    }
}

}  // namespace

bool plugins_start() {
    // Create the plugins folder if it does not exist so users have a
    // clear place to drop files.
    std::wstring dir = plugins_dll_dir() + L"\\data\\plugins";
    CreateDirectoryW(dir.c_str(), nullptr);
    scan_plugins_dir();
    logf("plugins: started, %zu plugin(s) loaded", g_plugins.size());
    return true;
}

void plugins_tick() {
    auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (g_last_toast_tick.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<double>(now - g_last_toast_tick).count();
    }
    g_last_toast_tick = now;

    check_hot_reload();
    dispatch_events();
    run_render_hooks();
    render_toasts(dt);
}

void plugins_stop() {
    for (auto& p : g_plugins) {
        if (p.L) { lua_close(p.L); p.L = nullptr; }
    }
    g_plugins.clear();
}

void plugins_emit_hero_locked() {
    if (g_plugins.empty()) return;
    PluginEvent ev;
    ev.k = PluginEvent::Kind::HeroLocked;
    std::lock_guard<std::mutex> lk(g_event_mtx);
    g_event_queue.push_back(std::move(ev));
}

void plugins_emit_damage_dealt(const char* skill_name, double amount,
                               bool is_crit, bool is_kill) {
    if (g_plugins.empty()) return;
    PluginEvent ev;
    ev.k       = PluginEvent::Kind::DamageDealt;
    ev.skill   = skill_name ? skill_name : "";
    ev.amount  = amount;
    ev.is_crit = is_crit;
    ev.is_kill = is_kill;
    std::lock_guard<std::mutex> lk(g_event_mtx);
    g_event_queue.push_back(std::move(ev));
}

void plugins_emit_fight_start(int fight_id) {
    if (g_plugins.empty()) return;
    PluginEvent ev;
    ev.k        = PluginEvent::Kind::FightStart;
    ev.fight_id = fight_id;
    std::lock_guard<std::mutex> lk(g_event_mtx);
    g_event_queue.push_back(std::move(ev));
}

void plugins_emit_fight_end(int fight_id, double duration_s,
                            double total_damage, double dps,
                            const char* top_skill) {
    if (g_plugins.empty()) return;
    PluginEvent ev;
    ev.k         = PluginEvent::Kind::FightEnd;
    ev.fight_id  = fight_id;
    ev.duration  = duration_s;
    ev.amount    = total_damage;
    ev.dps       = dps;
    ev.top_skill = top_skill ? top_skill : "";
    std::lock_guard<std::mutex> lk(g_event_mtx);
    g_event_queue.push_back(std::move(ev));
}

bool plugins_manager_visible() { return g_manager_visible.load(); }
void plugins_manager_toggle()  { g_manager_visible.store(!g_manager_visible.load()); }

void plugins_render_manager() {
    if (!g_manager_visible.load()) return;
    bool open = true;
    if (!ImGui::Begin("Plugins", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) g_manager_visible.store(false);
        return;
    }
    if (g_plugins.empty()) {
        ImGui::TextDisabled("No plugins loaded.");
        ImGui::TextDisabled("Drop .lua files into data/plugins/.");
    } else {
        for (auto& p : g_plugins) {
            ImU32 col = p.init_ok ? IM_COL32(180, 220, 130, 255)
                                  : IM_COL32(255, 140, 100, 255);
            ImGui::TextColored(
                ImVec4(((col >> 0) & 0xff) / 255.0f,
                       ((col >> 8) & 0xff) / 255.0f,
                       ((col >> 16) & 0xff) / 255.0f, 1.0f),
                "%s%s", p.name.c_str(), p.init_ok ? "" : "  (error)");
            if (!p.last_error.empty()) {
                ImGui::Indent();
                ImGui::TextWrapped("%s", p.last_error.c_str());
                ImGui::Unindent();
            }
            ImGui::SameLine();
            ImGui::PushID(&p);
            if (ImGui::SmallButton("reload")) {
                load_plugin(p);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Auto-reload on file change is on.");
    }
    ImGui::End();
    if (!open) g_manager_visible.store(false);
}

}  // namespace farever
