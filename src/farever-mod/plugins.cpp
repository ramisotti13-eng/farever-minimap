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
#include "target_state.h"
#include "aggregator.h"
#include "pois.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "imgui.h"

#include <windows.h>
#include <mmsystem.h>
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
    enum class Kind { HeroLocked, DamageDealt, FightStart, FightEnd,
                      TargetChanged, CastStart, CastEnd, WeaponChanged };
    Kind        k = Kind::HeroLocked;
    std::string skill;
    std::string top_skill;
    std::string target_kind;
    std::string weapon_kind;
    std::string prev_weapon_kind;
    double      amount   = 0.0;
    double      duration = 0.0;
    double      dps      = 0.0;
    double      total_sec = 0.0;
    int         fight_id = 0;
    int         weapon_level   = 0;
    int         weapon_upgrade = 0;
    bool        is_crit  = false;
    bool        is_kill  = false;
};

std::vector<Plugin>      g_plugins;
// v0.5.3.2 (#6): mirror of g_plugins.size() for the lock-free fast
// path in plugins_emit_*. Render thread calls emit_* every Present and
// has historically called g_plugins.empty() without a lock — fine when
// the vector is stable, but a hot reload from the overlay thread can
// reallocate the storage and torn the size read. The atomic is
// maintained on every scan / reload / stop.
std::atomic<int>         g_plugin_count{0};
// v0.5.3.3 diagnostic kill switch. When true plugins_start is skipped
// and plugins_tick / plugins_emit_* short-circuit so we can bisect
// whether the plugin path is involved in a post-lock crash.
std::atomic<bool>        g_disabled{false};
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

// v0.5.3.2 (#5): bound the queues. If the overlay thread stalls while
// the render thread keeps flooding events / toasts these would grow
// without limit. Drop-oldest is fine — these are advisory UI events.
constexpr std::size_t kMaxEventQueue = 256;
constexpr std::size_t kMaxToasts     = 16;

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

// Macro-generated getters that read one f64 (or i32 / bool) field
// from the HeroSnapshot and push it onto the Lua stack. Two flavours:
// UA_GETTER for fields gated by attr_ok (UnitAttributes layer),
// HA_GETTER for hero-only fields gated by hero_attr_ok.
#define UA_GETTER(name, field) \
    int api_player_##name(lua_State* L) { \
        HeroSnapshot h = hero_state_read(); \
        lua_pushnumber(L, h.attr_ok ? h.field : 0.0); \
        return 1; \
    }
#define HA_GETTER(name, field) \
    int api_player_##name(lua_State* L) { \
        HeroSnapshot h = hero_state_read(); \
        lua_pushnumber(L, h.hero_attr_ok ? h.field : 0.0); \
        return 1; \
    }

// Health and energy
UA_GETTER(health,       health)
UA_GETTER(max_health,   max_health)
UA_GETTER(health_regen, health_regen)
UA_GETTER(shield,       shield)
UA_GETTER(energy,       energy)
UA_GETTER(energy_regen, energy_regen)

int api_player_health_pct(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    double pct = 0.0;
    if (h.attr_ok && h.max_health > 0.0) pct = h.health / h.max_health;
    lua_pushnumber(L, pct);
    return 1;
}

// Primary stats
UA_GETTER(vitality,  vitality)
UA_GETTER(strength,  strength)
UA_GETTER(dexterity, dexterity)
UA_GETTER(faith,     faith)
UA_GETTER(intellect, intellect)

// Combat numbers
UA_GETTER(crit_chance,                crit_chance)
UA_GETTER(crit_damage,                crit_damage)
UA_GETTER(armor_penetration,          armor_penetration)
UA_GETTER(spell_penetration,          spell_penetration)
UA_GETTER(fervor,                     fervor)
UA_GETTER(block_mitigation,           block_mitigation)
UA_GETTER(dodge_chance,               dodge_chance)
UA_GETTER(magic_mastery,              magic_mastery)
UA_GETTER(physical_mastery,           physical_mastery)
UA_GETTER(spell_cast_time_reduction,  spell_cast_time_reduction)
UA_GETTER(knock_resistance,           knock_resistance)
UA_GETTER(cooldown_reduction,         cooldown_reduction)

// Defense
UA_GETTER(armor,            armor)
UA_GETTER(magic_armor,      magic_armor)
UA_GETTER(magic_reduction,  magic_reduction)

// Misc
UA_GETTER(move_speed_factor, move_speed_factor)
UA_GETTER(damage,            damage)
UA_GETTER(heal,              heal)

// Hero-only class resources
HA_GETTER(poise,                   poise)
HA_GETTER(poise_regen,             poise_regen)
HA_GETTER(oxygen,                  oxygen)
HA_GETTER(rage,                    rage)
HA_GETTER(rage_regen,              rage_regen)
HA_GETTER(spark,                   spark)
HA_GETTER(spark_regen,             spark_regen)
HA_GETTER(combo_point,             combo_point)
HA_GETTER(focus,                   focus)
HA_GETTER(damage_modifier,         damage_modifier)
HA_GETTER(damage_taken_modifier,   damage_taken_modifier)
HA_GETTER(heal_given_multiplier,   heal_given_multiplier)
HA_GETTER(shield_power_multiplier, shield_power_multiplier)
HA_GETTER(glide_speed,             glide_speed)

#undef UA_GETTER
#undef HA_GETTER

int api_player_level(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushinteger(L, h.level);
    return 1;
}
int api_player_combat_start(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushnumber(L, h.combat_start);
    return 1;
}
int api_player_has_target(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushboolean(L, h.has_target ? 1 : 0);
    return 1;
}

// Equipped weapon getters. Empty string / 0 when no weapon is equipped
// (e.g. mid-swap) or the read chain failed this frame.
int api_player_weapon_kind(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    if (h.weapon_ok) {
        lua_pushlstring(L, h.weapon_kind.data(), h.weapon_kind.size());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}
int api_player_weapon_level(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushinteger(L, h.weapon_ok ? h.weapon_level : 0);
    return 1;
}
int api_player_weapon_upgrade(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushinteger(L, h.weapon_ok ? h.weapon_upgrade : 0);
    return 1;
}

// Full equipped loadout as a Lua array of tables:
//   {{ kind="Helmet_X", level=1, upgrade=0 }, ...}
// Order matches the game's content array; empty when not yet read.
int api_player_equipment(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.equipment.size(), 0);
    for (std::size_t i = 0; i < h.equipment.size(); ++i) {
        const auto& it = h.equipment[i];
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, it.kind.data(), it.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, it.level);
        lua_setfield(L, -2, "level");
        lua_pushinteger(L, it.upgrade);
        lua_setfield(L, -2, "upgrade");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// Active statuses (buffs / debuffs) as a Lua array of tables:
//   {{ kind="Bleed", duration=12.0, stacks=3 }, ...}
// Plugins compute remaining time client-side if they want a countdown.
int api_player_statuses(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.statuses.size(), 0);
    for (std::size_t i = 0; i < h.statuses.size(); ++i) {
        const auto& s = h.statuses[i];
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, s.kind.data(), s.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushnumber(L, s.duration);
        lua_setfield(L, -2, "duration");
        lua_pushinteger(L, s.stacks);
        lua_setfield(L, -2, "stacks");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

// v0.5.3.1's farever.foes API (Phase C) was removed in v0.5.3.2 after
// it was identified as the root cause of a post-lock crash. Foe
// tracking will return in a later release once the read path is
// rebuilt in isolation. Plugin authors who started using the API will
// see it as nil and should branch accordingly.

// farever.target.* — Slice 1: existence + kind id only.
// Hero.target chased once per ~4 frames on the render thread by
// target_state_tick; this just reads the published snapshot.
int api_target_exists(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushboolean(L, t.exists ? 1 : 0);
    return 1;
}
int api_target_name(lua_State* L) {
    TargetSnapshot t = target_state_read();
    if (t.exists) lua_pushlstring(L, t.kind.data(), t.kind.size());
    else          lua_pushstring(L, "");
    return 1;
}
int api_target_x(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, t.exists ? t.x : 0.0);
    return 1;
}
int api_target_y(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, t.exists ? t.y : 0.0);
    return 1;
}
int api_target_z(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, t.exists ? t.z : 0.0);
    return 1;
}
int api_target_level(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushinteger(L, t.exists ? t.level : 0);
    return 1;
}
int api_target_hp(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.attr_ok) ? t.health : 0.0);
    return 1;
}
int api_target_max_hp(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.attr_ok) ? t.max_health : 0.0);
    return 1;
}
int api_target_hp_pct(lua_State* L) {
    TargetSnapshot t = target_state_read();
    double pct = 0.0;
    if (t.exists && t.attr_ok && t.max_health > 0.0) {
        pct = t.health / t.max_health;
    }
    lua_pushnumber(L, pct);
    return 1;
}

// Slice 4 (damage planner): target defense surface. Returns 0 when no
// target or attr chase hasn't completed; plugins should gate on exists()
// + attr_ok via hp() != 0 just like the other attribute getters.
int api_target_armor(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.attr_ok) ? t.armor : 0.0);
    return 1;
}
int api_target_magic_armor(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.attr_ok) ? t.magic_armor : 0.0);
    return 1;
}
int api_target_magic_reduction(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.attr_ok) ? t.magic_reduction : 0.0);
    return 1;
}

// Slice 3: cast bar.
int api_target_is_casting(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushboolean(L, (t.exists && t.is_casting) ? 1 : 0);
    return 1;
}
int api_target_cast_skill(lua_State* L) {
    TargetSnapshot t = target_state_read();
    if (t.exists && t.is_casting) {
        lua_pushlstring(L, t.cast_skill.data(), t.cast_skill.size());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}
int api_target_cast_progress(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.is_casting) ? t.cast_progress : 0.0);
    return 1;
}
int api_target_cast_total_sec(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.is_casting) ? t.cast_total_sec : 0.0);
    return 1;
}
int api_target_cast_remaining_sec(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.is_casting) ? t.cast_remaining_sec : 0.0);
    return 1;
}
int api_target_cast_elapsed_sec(lua_State* L) {
    TargetSnapshot t = target_state_read();
    lua_pushnumber(L, (t.exists && t.is_casting) ? t.cast_elapsed_sec : 0.0);
    return 1;
}

// farever.sound(name) — play a short system sound for plugin alerts
// (boss-cast warnings etc.). Built-in names map to the Windows
// system event sounds so we don't ship audio files. Names accepted:
//   "alert"   -> SystemAsterisk    (sharp ping)
//   "warning" -> SystemExclamation (lower ping)
//   "info"    -> SystemNotification
//   "beep"    -> simple beep
int api_sound(lua_State* L) {
    const char* name = luaL_optstring(L, 1, "alert");
    LPCWSTR alias = L"SystemAsterisk";
    if      (std::strcmp(name, "warning") == 0) alias = L"SystemExclamation";
    else if (std::strcmp(name, "info")    == 0) alias = L"SystemNotification";
    else if (std::strcmp(name, "beep")    == 0) {
        MessageBeep(MB_OK);
        return 0;
    }
    PlaySoundW(alias, nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    return 0;
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

    // v0.5.3.2 (#4): atomic write — write to a .tmp file first, then
    // MoveFileExW with MOVEFILE_REPLACE_EXISTING. CreateFile-with-
    // CREATE_ALWAYS truncates the destination before the new bytes
    // land, so a game crash mid-write previously left users with an
    // empty store file and a wiped personal_best.
    std::wstring final_path = store_path_for(plugin_name);
    std::wstring tmp_path   = final_path + L".tmp";
    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logf("plugins: '%s' store save failed (CreateFile tmp)",
             plugin_name.c_str());
        return;
    }
    DWORD written = 0;
    BOOL  wrote_ok = WriteFile(h, out.data(), (DWORD)out.size(),
                               &written, nullptr);
    // Force the bytes to disk before the rename so a power loss
    // between rename and flush can't leave the rename naming an empty
    // file. FlushFileBuffers is cheap on SSDs.
    if (wrote_ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!wrote_ok || written != out.size()) {
        logf("plugins: '%s' store save failed (Write %lu of %zu)",
             plugin_name.c_str(),
             (unsigned long)written, out.size());
        DeleteFileW(tmp_path.c_str());
        return;
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logf("plugins: '%s' store save failed (rename, GLE=%lu)",
             plugin_name.c_str(),
             (unsigned long)GetLastError());
        DeleteFileW(tmp_path.c_str());
    }
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
    // v0.5.3.2 (#5): drop-oldest if a runaway plugin spams toast().
    if (g_toasts.size() > kMaxToasts) {
        g_toasts.erase(g_toasts.begin(),
                       g_toasts.begin() +
                       (g_toasts.size() - kMaxToasts));
    }
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
    // v0.5.3.2 (#7): 1 KB buffer covers anything a sane plugin would
    // ever type into a single field. Longer inputs still get cleanly
    // truncated; no plugin should be storing essays in here.
    char buf[1024] = {0};
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

// ---------- v0.5.6 animation surface ----------------------------------
//
// Time helper: monotonic seconds since plugin runtime startup. Use it
// to drive sin/cos pulses or remaining-time count-downs. Same source
// across all plugins so animations stay in phase.
std::chrono::steady_clock::time_point g_plugin_start_time =
    std::chrono::steady_clock::now();

int api_farever_now(lua_State* L) {
    auto now = std::chrono::steady_clock::now();
    auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                   now - g_plugin_start_time).count();
    lua_pushnumber(L, (double)us / 1e6);
    return 1;
}

// farever.pois() returns the full POI list the mod loaded at boot
// (from data/pois_<world>.json) as a Lua array of tables:
//   {{ x, y, z, kind, subkind, name, id }, ...}
// Order matches pois_get(). Snapshot of the in-memory data, cheap to
// build (one table per POI, plus 7 fields each). Plugins can filter
// by category/kind themselves and don't have to hardcode positions.
int api_farever_pois(lua_State* L) {
    const auto& list = pois_get();
    lua_createtable(L, (int)list.size(), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& p = list[i];
        lua_createtable(L, 0, 7);
        lua_pushnumber (L, p.x);       lua_setfield(L, -2, "x");
        lua_pushnumber (L, p.y);       lua_setfield(L, -2, "y");
        lua_pushnumber (L, p.z);       lua_setfield(L, -2, "z");
        lua_pushstring (L, p.kind);    lua_setfield(L, -2, "kind");
        lua_pushstring (L, p.subkind); lua_setfield(L, -2, "subkind");
        lua_pushstring (L, p.name);    lua_setfield(L, -2, "name");
        lua_pushstring (L, p.id);      lua_setfield(L, -2, "id");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// Font scale in the CURRENT window. Resets to 1.0 each frame? No: it
// persists until SetWindowFontScale is called again. Plugin authors
// should reset to 1.0 after their scaled draws to avoid leaking the
// scale into widgets that come after.
int api_imgui_font_scale(lua_State* L) {
    float s = (float)luaL_checknumber(L, 1);
    if (s < 0.1f) s = 0.1f;
    if (s > 10.0f) s = 10.0f;
    ImGui::SetWindowFontScale(s);
    return 0;
}

// Returns the absolute screen-space cursor position (where the next
// widget would draw). Use as anchor for the draw_* primitives.
int api_imgui_cursor_pos(lua_State* L) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
}

// Reserves a w x h block in the current window's flow without drawing
// anything. Needed when a plugin draws custom shapes via draw_* and
// wants ImGui to leave that vertical space for subsequent widgets.
int api_imgui_dummy(lua_State* L) {
    float w = (float)luaL_checknumber(L, 1);
    float h = (float)luaL_checknumber(L, 2);
    ImGui::Dummy(ImVec2(w, h));
    return 0;
}

// Pack 4 floats [0..1] into an ImGui ABGR color (the format DrawList
// wants). Clamps to [0..1] then maps to [0..255].
static ImU32 lua_to_col4(lua_State* L, int r_arg) {
    auto clamp01 = [](double v) {
        if (v < 0.0) return 0.0;
        if (v > 1.0) return 1.0;
        return v;
    };
    double r = clamp01(luaL_checknumber(L, r_arg));
    double g = clamp01(luaL_checknumber(L, r_arg + 1));
    double b = clamp01(luaL_checknumber(L, r_arg + 2));
    double a = clamp01(luaL_optnumber(L, r_arg + 3, 1.0));
    return IM_COL32(
        (int)(r * 255.0 + 0.5),
        (int)(g * 255.0 + 0.5),
        (int)(b * 255.0 + 0.5),
        (int)(a * 255.0 + 0.5));
}

int api_imgui_draw_rect_filled(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    ImU32 col = lua_to_col4(L, 5);
    ImGui::GetWindowDrawList()->AddRectFilled({x1, y1}, {x2, y2}, col);
    return 0;
}

int api_imgui_draw_rect(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    ImU32 col = lua_to_col4(L, 5);
    float thickness = (float)luaL_optnumber(L, 9, 1.0);
    ImGui::GetWindowDrawList()->AddRect({x1, y1}, {x2, y2}, col,
                                        0.0f, 0, thickness);
    return 0;
}

int api_imgui_draw_circle_filled(lua_State* L) {
    float x      = (float)luaL_checknumber(L, 1);
    float y      = (float)luaL_checknumber(L, 2);
    float radius = (float)luaL_checknumber(L, 3);
    ImU32 col    = lua_to_col4(L, 4);
    int segments = (int)luaL_optinteger(L, 8, 32);
    ImGui::GetWindowDrawList()->AddCircleFilled({x, y}, radius, col,
                                                segments);
    return 0;
}

int api_imgui_draw_circle(lua_State* L) {
    float x      = (float)luaL_checknumber(L, 1);
    float y      = (float)luaL_checknumber(L, 2);
    float radius = (float)luaL_checknumber(L, 3);
    ImU32 col    = lua_to_col4(L, 4);
    float thickness = (float)luaL_optnumber(L, 8, 1.0);
    int segments    = (int)luaL_optinteger(L, 9, 32);
    ImGui::GetWindowDrawList()->AddCircle({x, y}, radius, col,
                                          segments, thickness);
    return 0;
}

int api_imgui_draw_line(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    ImU32 col = lua_to_col4(L, 5);
    float thickness = (float)luaL_optnumber(L, 9, 1.0);
    ImGui::GetWindowDrawList()->AddLine({x1, y1}, {x2, y2}, col, thickness);
    return 0;
}

int api_imgui_draw_text(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    ImU32 col = lua_to_col4(L, 3);
    const char* s = luaL_checkstring(L, 7);
    ImGui::GetWindowDrawList()->AddText({x, y}, col, s);
    return 0;
}

int api_imgui_draw_triangle_filled(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    float x3 = (float)luaL_checknumber(L, 5);
    float y3 = (float)luaL_checknumber(L, 6);
    ImU32 col = lua_to_col4(L, 7);
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        {x1, y1}, {x2, y2}, {x3, y3}, col);
    return 0;
}

int api_imgui_draw_triangle(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    float x3 = (float)luaL_checknumber(L, 5);
    float y3 = (float)luaL_checknumber(L, 6);
    ImU32 col = lua_to_col4(L, 7);
    float thickness = (float)luaL_optnumber(L, 11, 1.0);
    ImGui::GetWindowDrawList()->AddTriangle(
        {x1, y1}, {x2, y2}, {x3, y3}, col, thickness);
    return 0;
}

void install_api(lua_State* L) {
    lua_newtable(L);  // farever

    lua_newtable(L);  // farever.player
    #define BIND(name) do { \
        lua_pushcfunction(L, api_player_##name); \
        lua_setfield(L, -2, #name); \
    } while (0)

    // Position / orientation / lock state
    BIND(x); BIND(y); BIND(z);
    lua_pushcfunction(L, api_player_rot); lua_setfield(L, -2, "rot_z");
    BIND(locked); BIND(in_combat); BIND(has_target);
    BIND(level); BIND(combat_start);

    // Equipped weapon (Hero.weaponInHand chase)
    BIND(weapon_kind); BIND(weapon_level); BIND(weapon_upgrade);

    // Full loadout + active statuses (Lua array returns)
    BIND(equipment); BIND(statuses);

    // Health and energy
    BIND(health); BIND(max_health); BIND(health_pct); BIND(health_regen);
    BIND(shield); BIND(energy); BIND(energy_regen);

    // Primary stats
    BIND(vitality); BIND(strength); BIND(dexterity);
    BIND(faith);    BIND(intellect);

    // Combat numbers
    BIND(crit_chance);  BIND(crit_damage);
    BIND(armor_penetration); BIND(spell_penetration);
    BIND(fervor); BIND(block_mitigation); BIND(dodge_chance);
    BIND(magic_mastery); BIND(physical_mastery);
    BIND(spell_cast_time_reduction);
    BIND(knock_resistance); BIND(cooldown_reduction);

    // Defense
    BIND(armor); BIND(magic_armor); BIND(magic_reduction);

    // Misc modifiers
    BIND(move_speed_factor); BIND(damage); BIND(heal);

    // Hero-only class resources
    BIND(poise); BIND(poise_regen);
    BIND(oxygen);
    BIND(rage); BIND(rage_regen);
    BIND(spark); BIND(spark_regen);
    BIND(combo_point); BIND(focus);
    BIND(damage_modifier); BIND(damage_taken_modifier);
    BIND(heal_given_multiplier); BIND(shield_power_multiplier);
    BIND(glide_speed);

    #undef BIND
    lua_setfield(L, -2, "player");

    lua_newtable(L);  // farever.dps
    lua_pushcfunction(L, api_dps_current);   lua_setfield(L, -2, "current");
    lua_pushcfunction(L, api_dps_total);     lua_setfield(L, -2, "total");
    lua_pushcfunction(L, api_dps_elapsed);   lua_setfield(L, -2, "elapsed");
    lua_pushcfunction(L, api_dps_in_combat); lua_setfield(L, -2, "in_combat");
    lua_setfield(L, -2, "dps");

    // farever.foes table was here in v0.5.3.1. Removed in v0.5.3.2;
    // see the comment by the (deleted) bindings above.

    lua_newtable(L);  // farever.target  (Slice 3: + cast bar)
    lua_pushcfunction(L, api_target_exists);  lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, api_target_name);    lua_setfield(L, -2, "name");
    lua_pushcfunction(L, api_target_x);       lua_setfield(L, -2, "x");
    lua_pushcfunction(L, api_target_y);       lua_setfield(L, -2, "y");
    lua_pushcfunction(L, api_target_z);       lua_setfield(L, -2, "z");
    lua_pushcfunction(L, api_target_level);   lua_setfield(L, -2, "level");
    lua_pushcfunction(L, api_target_hp);      lua_setfield(L, -2, "hp");
    lua_pushcfunction(L, api_target_max_hp);  lua_setfield(L, -2, "max_hp");
    lua_pushcfunction(L, api_target_hp_pct);  lua_setfield(L, -2, "hp_pct");
    lua_pushcfunction(L, api_target_armor);           lua_setfield(L, -2, "armor");
    lua_pushcfunction(L, api_target_magic_armor);     lua_setfield(L, -2, "magic_armor");
    lua_pushcfunction(L, api_target_magic_reduction); lua_setfield(L, -2, "magic_reduction");
    lua_pushcfunction(L, api_target_is_casting);
    lua_setfield(L, -2, "is_casting");
    lua_pushcfunction(L, api_target_cast_skill);
    lua_setfield(L, -2, "cast_skill");
    lua_pushcfunction(L, api_target_cast_progress);
    lua_setfield(L, -2, "cast_progress");
    lua_pushcfunction(L, api_target_cast_total_sec);
    lua_setfield(L, -2, "cast_total_sec");
    lua_pushcfunction(L, api_target_cast_remaining_sec);
    lua_setfield(L, -2, "cast_remaining_sec");
    lua_pushcfunction(L, api_target_cast_elapsed_sec);
    lua_setfield(L, -2, "cast_elapsed_sec");
    lua_setfield(L, -2, "target");

    lua_newtable(L);  // farever.log
    lua_pushcfunction(L, api_log_info); lua_setfield(L, -2, "info");
    lua_pushcfunction(L, api_log_info); lua_setfield(L, -2, "warn");
    lua_setfield(L, -2, "log");

    lua_newtable(L);  // farever.store
    lua_pushcfunction(L, api_store_get); lua_setfield(L, -2, "get");
    lua_pushcfunction(L, api_store_set); lua_setfield(L, -2, "set");
    lua_setfield(L, -2, "store");

    lua_pushcfunction(L, api_toast); lua_setfield(L, -2, "toast");
    lua_pushcfunction(L, api_sound); lua_setfield(L, -2, "sound");
    lua_pushcfunction(L, api_farever_now);  lua_setfield(L, -2, "now");
    lua_pushcfunction(L, api_farever_pois); lua_setfield(L, -2, "pois");

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
    // v0.5.6 animation surface
    lua_pushcfunction(L, api_imgui_font_scale);         lua_setfield(L, -2, "font_scale");
    lua_pushcfunction(L, api_imgui_cursor_pos);         lua_setfield(L, -2, "cursor_pos");
    lua_pushcfunction(L, api_imgui_dummy);              lua_setfield(L, -2, "dummy");
    lua_pushcfunction(L, api_imgui_draw_rect_filled);   lua_setfield(L, -2, "draw_rect_filled");
    lua_pushcfunction(L, api_imgui_draw_rect);          lua_setfield(L, -2, "draw_rect");
    lua_pushcfunction(L, api_imgui_draw_circle_filled); lua_setfield(L, -2, "draw_circle_filled");
    lua_pushcfunction(L, api_imgui_draw_circle);        lua_setfield(L, -2, "draw_circle");
    lua_pushcfunction(L, api_imgui_draw_line);             lua_setfield(L, -2, "draw_line");
    lua_pushcfunction(L, api_imgui_draw_text);             lua_setfield(L, -2, "draw_text");
    lua_pushcfunction(L, api_imgui_draw_triangle);         lua_setfield(L, -2, "draw_triangle");
    lua_pushcfunction(L, api_imgui_draw_triangle_filled);  lua_setfield(L, -2, "draw_triangle_filled");
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
        g_plugin_count.store((int)g_plugins.size(),
                             std::memory_order_release);
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
        case PluginEvent::Kind::TargetChanged:
            lua_pushstring(L, ev.target_kind.c_str());
            lua_setfield(L, -2, "kind");
            break;
        case PluginEvent::Kind::CastStart:
            lua_pushstring(L, ev.skill.c_str());
            lua_setfield(L, -2, "skill");
            lua_pushnumber(L, ev.total_sec);
            lua_setfield(L, -2, "total_sec");
            break;
        case PluginEvent::Kind::CastEnd:
            lua_pushstring(L, ev.skill.c_str());
            lua_setfield(L, -2, "skill");
            lua_pushnumber(L, ev.duration);
            lua_setfield(L, -2, "duration");
            break;
        case PluginEvent::Kind::WeaponChanged:
            lua_pushstring(L, ev.weapon_kind.c_str());
            lua_setfield(L, -2, "kind");
            lua_pushstring(L, ev.prev_weapon_kind.c_str());
            lua_setfield(L, -2, "prev_kind");
            lua_pushinteger(L, ev.weapon_level);
            lua_setfield(L, -2, "level");
            lua_pushinteger(L, ev.weapon_upgrade);
            lua_setfield(L, -2, "upgrade");
            break;
    }
}

const char* event_name(PluginEvent::Kind k) {
    switch (k) {
        case PluginEvent::Kind::HeroLocked:    return "hero_locked";
        case PluginEvent::Kind::DamageDealt:   return "damage_dealt";
        case PluginEvent::Kind::FightStart:    return "fight_start";
        case PluginEvent::Kind::FightEnd:      return "fight_end";
        case PluginEvent::Kind::TargetChanged: return "target_changed";
        case PluginEvent::Kind::CastStart:     return "cast_start";
        case PluginEvent::Kind::CastEnd:       return "cast_end";
        case PluginEvent::Kind::WeaponChanged: return "weapon_changed";
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

void plugins_set_disabled(bool disabled) {
    g_disabled.store(disabled);
}

bool plugins_start() {
    if (g_disabled.load()) {
        logf("plugins: disabled by data/no_plugins.flag — skipping scan");
        return true;
    }
    // Create the plugins folder if it does not exist so users have a
    // clear place to drop files.
    std::wstring dir = plugins_dll_dir() + L"\\data\\plugins";
    CreateDirectoryW(dir.c_str(), nullptr);
    scan_plugins_dir();
    logf("plugins: started, %zu plugin(s) loaded", g_plugins.size());
    return true;
}

void plugins_tick() {
    if (g_disabled.load()) return;
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
    g_plugin_count.store(0, std::memory_order_release);
}

// v0.5.3.2 (#5 + #6): queue helper consolidates the cap-and-drop logic
// and uses the atomic plugin count for the fast-path skip so the
// render-thread emit doesn't race the overlay-thread vector mutation
// during hot reload.
static void enqueue_event(PluginEvent&& ev) {
    if (g_plugin_count.load(std::memory_order_acquire) <= 0) return;
    std::lock_guard<std::mutex> lk(g_event_mtx);
    g_event_queue.push_back(std::move(ev));
    if (g_event_queue.size() > kMaxEventQueue) {
        g_event_queue.erase(
            g_event_queue.begin(),
            g_event_queue.begin() +
            (g_event_queue.size() - kMaxEventQueue));
    }
}

void plugins_emit_hero_locked() {
    PluginEvent ev;
    ev.k = PluginEvent::Kind::HeroLocked;
    enqueue_event(std::move(ev));
}

void plugins_emit_damage_dealt(const char* skill_name, double amount,
                               bool is_crit, bool is_kill) {
    PluginEvent ev;
    ev.k       = PluginEvent::Kind::DamageDealt;
    ev.skill   = skill_name ? skill_name : "";
    ev.amount  = amount;
    ev.is_crit = is_crit;
    ev.is_kill = is_kill;
    enqueue_event(std::move(ev));
}

void plugins_emit_fight_start(int fight_id) {
    PluginEvent ev;
    ev.k        = PluginEvent::Kind::FightStart;
    ev.fight_id = fight_id;
    enqueue_event(std::move(ev));
}

void plugins_emit_fight_end(int fight_id, double duration_s,
                            double total_damage, double dps,
                            const char* top_skill) {
    PluginEvent ev;
    ev.k         = PluginEvent::Kind::FightEnd;
    ev.fight_id  = fight_id;
    ev.duration  = duration_s;
    ev.amount    = total_damage;
    ev.dps       = dps;
    ev.top_skill = top_skill ? top_skill : "";
    enqueue_event(std::move(ev));
}

void plugins_emit_target_changed(const char* kind) {
    PluginEvent ev;
    ev.k           = PluginEvent::Kind::TargetChanged;
    ev.target_kind = kind ? kind : "";
    enqueue_event(std::move(ev));
}

void plugins_emit_cast_start(const char* skill, double total_sec) {
    PluginEvent ev;
    ev.k         = PluginEvent::Kind::CastStart;
    ev.skill     = skill ? skill : "";
    ev.total_sec = total_sec;
    enqueue_event(std::move(ev));
}

void plugins_emit_cast_end(const char* skill, double duration_sec) {
    PluginEvent ev;
    ev.k        = PluginEvent::Kind::CastEnd;
    ev.skill    = skill ? skill : "";
    ev.duration = duration_sec;
    enqueue_event(std::move(ev));
}

void plugins_emit_weapon_changed(const char* kind, const char* prev_kind,
                                 int level, int upgrade) {
    PluginEvent ev;
    ev.k                = PluginEvent::Kind::WeaponChanged;
    ev.weapon_kind      = kind      ? kind      : "";
    ev.prev_weapon_kind = prev_kind ? prev_kind : "";
    ev.weapon_level     = level;
    ev.weapon_upgrade   = upgrade;
    enqueue_event(std::move(ev));
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
