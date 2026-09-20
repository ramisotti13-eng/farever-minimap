// Lua-based plugin system. See plugins.h for the user-facing contract.
//
// Each plugin owns its own lua_State so a crash in one plugin doesn't
// taint the others. The runtime is fully sandboxed - io / os.execute /
// require / dofile / loadfile / debug are all nil'd out before any
// plugin code runs. Plugins read game state through the bound
// `farever` table (read-only) and draw through the bound `imgui` table.
//
// Threading: every Lua call happens on the render thread. The event
// emit_* helpers may be called from other threads - they just push
// into a mutex-guarded queue, drained on the next plugins_tick.

#include "plugins.h"
#include "log.h"
#include "hero_state.h"
#include "skill_resolve.h"   // #96: SkillCooldown, skill_cooldown_snapshot()
#include "target_state.h"
#include "aggregator.h"
#include "codex_state.h"
#include "pois.h"
#include "waypoints.h"
#include "overlay.h"
#include "party_state.h"
#include "camera_state.h"    // #105: camera yaw for plugins
#include "user_data.h"

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
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
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
    bool         theme_opt_out = false;  // plugin set `plugin_theme = false`
    // #105: user switched this one off in the plugin manager. A disabled
    // plugin stays loaded (so the manager can still show it and turning it
    // back on is instant) but draws nothing and receives no events. The set
    // of disabled names is persisted, see load/save_disabled_plugins.
    bool         enabled    = true;
    bool         wants_settings = false;  // plugin defines on_settings()
    bool         settings_open  = false;  // its settings window is showing
    std::string  last_error;
};

struct PluginEvent {
    enum class Kind { HeroLocked, DamageDealt, HealDealt, ShieldApplied,
                      FightStart, FightEnd, TargetChanged, CastStart, CastEnd,
                      WeaponChanged, CharacterLogin };
    Kind        k = Kind::HeroLocked;
    std::string skill;
    std::string top_skill;
    std::string target_kind;
    std::string weapon_kind;
    std::string prev_weapon_kind;
    std::string character_key;    // #109 CharacterLogin
    std::string character_name;   // #109 CharacterLogin
    double      amount   = 0.0;
    double      blocked  = 0.0;    // #87: DamageDealt blocked amount (_block)
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
// has historically called g_plugins.empty() without a lock - fine when
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
// #109: the character the mod is currently running as. Written from the
// hl_pump worker (which owns the debounced profile switch), read from the
// render thread by the store + player APIs, hence the mutex rather than a
// bare string. Empty until a character has been observed for ~3 s.
std::mutex               g_character_mtx;
std::string              g_character_key;
std::string              g_character_name;
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
// without limit. Drop-oldest is fine - these are advisory UI events.
constexpr std::size_t kMaxEventQueue = 256;
constexpr std::size_t kMaxToasts     = 16;

// === Store file path helper ===
constexpr const wchar_t* kStoreSuffix = L".store.lua";

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
    path += kStoreSuffix;
    return path;
}

// #108: true for the store files store_path_for() writes. The plugin scan
// globs *.lua, which matches them too; they are not plugins.
bool is_store_file(const wchar_t* filename) {
    if (!filename) return false;
    std::size_t n = std::wcslen(filename);
    std::size_t s = std::wcslen(kStoreSuffix);
    if (n <= s) return false;   // "<name>" must be non-empty
    return _wcsicmp(filename + (n - s), kStoreSuffix) == 0;
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
// #91: the cap that belongs with each of the above. 0 when the class has no
// such resource (or the HUD has not drawn its bar yet), so a plugin can use
// rage_max() > 0 as "this character has rage".
HA_GETTER(poise_max,               poise_max)
HA_GETTER(oxygen_max,              oxygen_max)
HA_GETTER(rage_max,                rage_max)
HA_GETTER(spark_max,               spark_max)
HA_GETTER(combo_point_max,         combo_point_max)
HA_GETTER(focus_max,               focus_max)
HA_GETTER(energy_max,              energy_max)
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

// #71: current character name (ent.Hero.name@1240). Empty string until a
// hero is locked. The same source the per-character POI profiles already use.
int api_player_name(lua_State* L) {
    std::string name;
    if (hero_state_local_name(name)) lua_pushlstring(L, name.data(), name.size());
    else                             lua_pushstring(L, "");
    return 1;
}

// #90: the player's class as a string, one of "Rogue" / "Mage" / "Priest" /
// "Warrior". Empty string until it resolves (pre-spawn). Derived from the
// class-specific skills the hero has learned, not inferred from gear.
int api_player_class(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_pushlstring(L, h.player_class.data(), h.player_class.size());
    return 1;
}

// #109: the key identifying the character currently played. Derived from the
// character name (sanitised + hashed), the same key the mod's own per-character
// files use, so a plugin's per-character state lines up with the mod's. Empty
// while no character is settled - see the debounce in hl_pump.
int api_player_character_key(lua_State* L) {
    std::lock_guard<std::mutex> lk(g_character_mtx);
    lua_pushlstring(L, g_character_key.data(), g_character_key.size());
    return 1;
}

// #87: the local player's stable account uid (st.Player.uid). Constant across
// sessions (unlike Hero.__uid), so FareverLogs can correlate one player's logs.
// Empty string until a player is locked / the uid populates.
int api_player_uid(lua_State* L) {
    std::string uid;
    if (hero_state_local_uid(uid)) lua_pushlstring(L, uid.data(), uid.size());
    else                           lua_pushstring(L, "");
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

// Full equipped loadout as a Lua array of tables (#87 adds slot / slot_name):
//   {{ kind="Helmet_X", level=1, upgrade=0, slot=3, slot_name="Head" }, ...}
// `slot` is the fixed EquipmentSlot index and `slot_name` its canonical name
// ("Head", "Chest", "FingerLeft"/"FingerRight" for the two rings, "Weapon1"
// etc.); slot_name is "" for an unmapped index. Order matches the game's
// content array; empty when not yet read.
int api_player_equipment(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.equipment.size(), 0);
    for (std::size_t i = 0; i < h.equipment.size(); ++i) {
        const auto& it = h.equipment[i];
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, it.kind.data(), it.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, it.level);
        lua_setfield(L, -2, "level");
        lua_pushinteger(L, it.upgrade);
        lua_setfield(L, -2, "upgrade");
        lua_pushinteger(L, it.slot);
        lua_setfield(L, -2, "slot");
        lua_pushlstring(L, it.slot_name.data(), it.slot_name.size());
        lua_setfield(L, -2, "slot_name");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// #94: the arsenal / selected weapon skills, as a Lua array of tables in slot
// order:
//   {{ slot=1, kind="Mage_RayOfSpark" }, { slot=2, kind="DS_Bladeleaf_Skill2" }}
// `slot` is the 1-based arsenal position; an empty / unresolved slot yields
// kind="". Lets a proc tracker tell which weapon skills are actually slotted
// (a two-hander in the arsenal still lists its skill here). Empty until the
// hero has drawn a weapon.
// #99: push { atlas, x, y, size, width, height, px, py, w, h } for a skill
// kind, or nil when the icon has not been resolved yet. `x`/`y` are cell
// indices in the atlas, the px/py/w/h mirror is the same rectangle in
// pixels so a plugin can slice the shipped PNG without doing the maths.
// The icon resolves once the skill has been seen (hotbar / cast / combat
// log), so a fresh session can return nil for a skill until then.
void push_skill_icon(lua_State* L, const char* kind) {
    SkillGfx g{};
    if (!kind || !*kind || !skill_resolve_lookup(kind, &g) ||
        !g.atlas_filename[0] || g.size <= 0) {
        lua_pushnil(L);
        return;
    }
    int w = g.width  > 0 ? g.width  : 1;
    int h = g.height > 0 ? g.height : 1;
    lua_createtable(L, 0, 10);
    lua_pushstring(L, g.atlas_filename);       lua_setfield(L, -2, "atlas");
    lua_pushinteger(L, g.x);                   lua_setfield(L, -2, "x");
    lua_pushinteger(L, g.y);                   lua_setfield(L, -2, "y");
    lua_pushinteger(L, g.size);                lua_setfield(L, -2, "size");
    lua_pushinteger(L, w);                     lua_setfield(L, -2, "width");
    lua_pushinteger(L, h);                     lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)g.x * g.size); lua_setfield(L, -2, "px");
    lua_pushinteger(L, (lua_Integer)g.y * g.size); lua_setfield(L, -2, "py");
    lua_pushinteger(L, (lua_Integer)w * g.size);   lua_setfield(L, -2, "w");
    lua_pushinteger(L, (lua_Integer)h * g.size);   lua_setfield(L, -2, "h");
}

// #105: farever.camera.rotation_z() -> the camera's yaw in radians, same
// value the compass uses, or nil when no camera is anchored yet (fall back to
// farever.player.rot_z() then). Deliberately yaw only: pitch, position and the
// look-at target are not exposed, since those are what a movement bot would
// want and the heading is all a HUD needs.
int api_camera_rotation_z(lua_State* L) {
    double yaw = 0.0;
    if (!camera_state_yaw(yaw)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, yaw);
    return 1;
}

// #105: true while a camera is anchored and the yaw above is meaningful.
int api_camera_available(lua_State* L) {
    double yaw = 0.0;
    lua_pushboolean(L, camera_state_yaw(yaw) ? 1 : 0);
    return 1;
}

// farever.icons.skill(kind) -> icon table or nil (see push_skill_icon).
int api_icons_skill(lua_State* L) {
    push_skill_icon(L, luaL_checkstring(L, 1));
    return 1;
}

// #104: farever.icons.cached() -> array of every skill kind that currently
// has an icon, sorted. These are the game's internal BaseSkill.kind ids, so
// this is the way to find the exact string icons.skill() wants rather than
// guessing at it. The list grows as skills resolve, so it is empty right
// after load and fills in once the hero's skills have been seen.
int api_icons_cached(lua_State* L) {
    std::vector<std::string> kinds = skill_resolve_cached_kinds();
    lua_createtable(L, (int)kinds.size(), 0);
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        lua_pushlstring(L, kinds[i].data(), kinds[i].size());
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

int api_player_weapon_skills(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.weapon_skills.size(), 0);
    for (std::size_t i = 0; i < h.weapon_skills.size(); ++i) {
        const auto& kind = h.weapon_skills[i];
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, (lua_Integer)(i + 1));
        lua_setfield(L, -2, "slot");
        lua_pushlstring(L, kind.data(), kind.size());
        lua_setfield(L, -2, "kind");
        push_skill_icon(L, kind.c_str());       // #99
        lua_setfield(L, -2, "icon");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// #93: the player's bag inventory as a Lua array of tables:
//   {{ kind="LavendulaPetal", level=0, upgrade=0, stack=10 }, ...}
// `stack` is the slot's item count (1 for gear / non-stacking). This bag holds
// gear AND stackable consumables / crafting materials, so a drop / loot tracker
// reads material counts straight from here. Currencies (gold) are separate, see
// player.currencies(). Order matches the game's array; empty until read.
int api_player_inventory(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.inventory.size(), 0);
    for (std::size_t i = 0; i < h.inventory.size(); ++i) {
        const auto& it = h.inventory[i];
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, it.kind.data(), it.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, it.level);
        lua_setfield(L, -2, "level");
        lua_pushinteger(L, it.upgrade);
        lua_setfield(L, -2, "upgrade");
        lua_pushinteger(L, it.stack);
        lua_setfield(L, -2, "stack");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// #93: the player's currencies (gold + other counted currencies) as a Lua array
// of tables:
//   {{ kind="Gold", amount=10433 }, ...}
// These live in Loadout.currencies, separate from the item bag. `kind` is the
// ItemKind id, `amount` the current count. Crafting materials are NOT here -
// they are stackable items in player.inventory(). Empty until read (pre-spawn).
int api_player_currencies(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.currencies.size(), 0);
    for (std::size_t i = 0; i < h.currencies.size(); ++i) {
        const auto& c = h.currencies[i];
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, c.kind.data(), c.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, c.amount);
        lua_setfield(L, -2, "amount");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// #96: the player's skills with their cooldowns, as a Lua array of tables:
//   {{ kind="Mage_Blink", cooldown=14.4, base_cooldown=15.0, charges=2 }, ...}
// `cooldown` is the effective (reduced) cooldown the game uses, `base_cooldown`
// the unmodified CDB value; 1 - cooldown/base_cooldown is that skill's total
// cooldown reduction. Populated as skills resolve (played a little); empty
// before that. See also player.cooldown_reduction() for the global estimate.
int api_player_skills(lua_State* L) {
    std::vector<SkillCooldown> skills = skill_cooldown_snapshot();
    lua_createtable(L, (int)skills.size(), 0);
    for (std::size_t i = 0; i < skills.size(); ++i) {
        const auto& sc = skills[i];
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, sc.kind.data(), sc.kind.size());
        lua_setfield(L, -2, "kind");
        push_skill_icon(L, sc.kind.c_str());
        lua_setfield(L, -2, "icon");
        lua_pushnumber(L, sc.effective);
        lua_setfield(L, -2, "cooldown");
        lua_pushnumber(L, sc.base);
        lua_setfield(L, -2, "base_cooldown");
        lua_pushnumber(L, sc.charges);
        lua_setfield(L, -2, "charges");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// #71: the full character sheet as the player sees it, as a Lua array of
// tables:
//   {{ name="Vitalität", value=124, text="124" },
//    { name="Krit. Trefferchance", value=11.1, text="11.1%" }, ...}
// `name` is the localized attribute label, `value` the parsed number, `text`
// the raw display string. Covers primaries AND secondaries (crit, block,
// penetration, armor, ...). Empty until the character menu has been opened
// once this session; the typed getters (player.vitality() etc.) cover the
// primaries directly.
int api_player_stats(lua_State* L) {
    std::vector<UiStat> stats = hero_state_ui_stats();
    lua_createtable(L, (int)stats.size(), 0);
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const auto& s = stats[i];
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, s.label.data(), s.label.size());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, s.value);
        lua_setfield(L, -2, "value");
        lua_pushlstring(L, s.text.data(), s.text.size());
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, (int)(i + 1));    // 1-based Lua array
    }
    return 1;
}

// Active statuses (buffs / debuffs) as a Lua array of tables:
//   {{ kind="Bleed", duration=12.0, stacks=3, shield_amount=0.0 }, ...}
// shield_amount (#70) is the live absorb the status grants (0 for non-shield
// statuses). Plugins compute remaining time client-side if they want a countdown.
int api_player_statuses(lua_State* L) {
    HeroSnapshot h = hero_state_read();
    lua_createtable(L, (int)h.statuses.size(), 0);
    for (std::size_t i = 0; i < h.statuses.size(); ++i) {
        const auto& s = h.statuses[i];
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, s.kind.data(), s.kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushnumber(L, s.duration);
        lua_setfield(L, -2, "duration");
        lua_pushinteger(L, s.stacks);
        lua_setfield(L, -2, "stacks");
        lua_pushnumber(L, s.shield_amount);
        lua_setfield(L, -2, "shield_amount");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

// farever.player.codex(kind) - issue #44. Takes a Unit.kind id (exactly
// what farever.target.name() returns) and returns the player's codex
// entry for that monster, or nil if the codex isn't loaded yet or the
// id isn't a codex monster:
//   { state="unknown"|"partial"|"complete", completed=bool,
//     progress=int, max=int, name=<localized>, path=<codex tree path> }
int api_player_codex(lua_State* L) {
    const char* kind = luaL_optstring(L, 1, "");
    CodexEntry e;
    if (!kind || !*kind || !codex_state_lookup(kind, &e)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    lua_pushstring (L, e.state);      lua_setfield(L, -2, "state");
    lua_pushboolean(L, e.completed);  lua_setfield(L, -2, "completed");
    lua_pushinteger(L, e.completion); lua_setfield(L, -2, "progress");
    lua_pushinteger(L, e.max);        lua_setfield(L, -2, "max");
    lua_pushstring (L, e.name);       lua_setfield(L, -2, "name");
    lua_pushstring (L, e.path);       lua_setfield(L, -2, "path");
    return 1;
}

// #105: farever.player.codex_list() -> the whole bestiary as an array, sorted
// by `kind`, so a codex tracker can page through it by index instead of having
// to know the monster ids up front. Each entry carries the same fields as
// codex(kind) plus `kind` itself:
//   { kind, name, path, state, completed, progress, max }
// Empty until the codex has been located (a second or two after the hero
// loads). This walks the game's whole bestiary map, so call it on an interval
// or on a kill event, not every frame.
int api_player_codex_list(lua_State* L) {
    std::vector<CodexEntry> all = codex_state_all();
    lua_createtable(L, (int)all.size(), 0);
    for (std::size_t i = 0; i < all.size(); ++i) {
        const CodexEntry& e = all[i];
        lua_createtable(L, 0, 7);
        lua_pushstring (L, e.kind);       lua_setfield(L, -2, "kind");
        lua_pushstring (L, e.name);       lua_setfield(L, -2, "name");
        lua_pushstring (L, e.path);       lua_setfield(L, -2, "path");
        lua_pushstring (L, e.state);      lua_setfield(L, -2, "state");
        lua_pushboolean(L, e.completed);  lua_setfield(L, -2, "completed");
        lua_pushinteger(L, e.completion); lua_setfield(L, -2, "progress");
        lua_pushinteger(L, e.max);        lua_setfield(L, -2, "max");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

// v0.5.3.1's farever.foes API (Phase C) was removed in v0.5.3.2 after
// it was identified as the root cause of a post-lock crash. Foe
// tracking will return in a later release once the read path is
// rebuilt in isolation. Plugin authors who started using the API will
// see it as nil and should branch accordingly.

// farever.target.* - Slice 1: existence + kind id only.
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

// farever.sound(name) - play a short system sound for plugin alerts
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
    lua_pushnumber(L, aggregator_dps());
    return 1;
}
int api_dps_total(lua_State* L) {
    lua_pushnumber(L, aggregator_total_damage());
    return 1;
}
int api_dps_elapsed(lua_State* L) {
    lua_pushnumber(L, aggregator_elapsed_sec());
    return 1;
}
int api_dps_in_combat(lua_State* L) {
    lua_pushboolean(L, aggregator_in_combat() ? 1 : 0);
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
// are not supported in v1.1 - plugins should compose via
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

    // v0.5.3.2 (#4): atomic write - write to a .tmp file first, then
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
        // No file yet - start with empty store table.
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

// #109: character-scoped keys. Plugin authors were building their own key out
// of name()..class()..uid() and pasting it in front of every store key, which
// only works once all three have resolved - before that they silently share one
// global slot across characters. These keep the value in the same per-plugin
// store file under a reserved prefix, so there is no second file to manage and
// no ordering trap. Returns false while no character is known yet.
bool store_char_key(const char* key, std::string* out) {
    std::string ck;
    {
        std::lock_guard<std::mutex> lk(g_character_mtx);
        ck = g_character_key;
    }
    if (ck.empty()) return false;
    out->assign("char:").append(ck).append(":").append(key);
    return true;
}

// Shared bodies. Stack convention matches the Lua entry points: the default
// (get) / value (set) sits at index 2; `key` is the effective store key, which
// is the plain argument for the global variants and the prefixed one for the
// per-character variants.
int store_get_with_key(lua_State* L, const char* key) {
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

void store_set_with_key(lua_State* L, const char* key) {
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
}

// Reject anything the store file cannot round-trip, before we touch the table.
// luaL_error longjmps out of the calling C function, so this either returns
// having validated or does not return at all.
void store_check_value(lua_State* L, const char* fn) {
    int vt = lua_type(L, 2);
    if (vt != LUA_TNIL && vt != LUA_TBOOLEAN &&
        vt != LUA_TNUMBER && vt != LUA_TSTRING) {
        luaL_error(L, "%s: value must be nil, boolean, number, "
                      "or string (got %s)",
                   fn, lua_typename(L, vt));
    }
}

int api_store_get(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    // Default value is whatever's at index 2; if nothing, push nil.
    if (lua_gettop(L) < 2) lua_pushnil(L);
    return store_get_with_key(L, key);
}

int api_store_set(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    store_check_value(L, "farever.store.set");
    store_set_with_key(L, key);
    return 0;
}

// #109: same pair, scoped to the character currently played. get_char falls
// back to the default while no character is known (loading screen, character
// select); set_char returns false in that case rather than writing the value
// into a slot that would later belong to whoever logs in.
int api_store_get_char(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (lua_gettop(L) < 2) lua_pushnil(L);
    std::string full;
    if (!store_char_key(key, &full)) {
        lua_pushvalue(L, 2);
        return 1;
    }
    return store_get_with_key(L, full.c_str());
}

int api_store_set_char(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    store_check_value(L, "farever.store.set_char");
    std::string full;
    if (!store_char_key(key, &full)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    store_set_with_key(L, full.c_str());
    lua_pushboolean(L, 1);
    return 1;
}

// === farever.write_combatlog ===
//
// #87 (FareverLogs.com): the plugin sandbox nils out `io`, so a plugin cannot
// write files itself. This lets a plugin emit a combat-log file (e.g. the
// FareverCompanion v1 schema) into one predictable folder an uploader /
// analytics site can ingest:
//     %LOCALAPPDATA%\farever-minimap\combatlogs\<filename>
// The filename is sanitised hard (a single path component of [A-Za-z0-9._-],
// no separators / traversal) so a plugin can never escape that folder. The
// write is atomic (.tmp + rename) like the plugin store.
// Lua: farever.write_combatlog(filename, contents) -> path  | nil, errmsg
int api_write_combatlog(lua_State* L) {
    const char* fname = luaL_checkstring(L, 1);
    size_t clen = 0;
    const char* contents = luaL_checklstring(L, 2, &clen);

    std::string fn(fname);
    if (fn.empty() || fn.size() > 128) {
        lua_pushnil(L);
        lua_pushstring(L, "write_combatlog: filename must be 1..128 chars");
        return 2;
    }
    for (char c : fn) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) {
            lua_pushnil(L);
            lua_pushstring(L, "write_combatlog: filename may only contain "
                              "A-Z a-z 0-9 . - _");
            return 2;
        }
    }
    if (fn.find("..") != std::string::npos) {
        lua_pushnil(L);
        lua_pushstring(L, "write_combatlog: invalid filename");
        return 2;
    }
    // Bound the payload so a runaway plugin can't fill the disk in one call.
    if (clen > (4u << 20)) {   // 4 MiB
        lua_pushnil(L);
        lua_pushstring(L, "write_combatlog: contents exceed 4 MiB");
        return 2;
    }

    std::wstring dir = user_data_path(L"combatlogs");
    CreateDirectoryW(dir.c_str(), nullptr);   // ok if it already exists

    int wn = MultiByteToWideChar(CP_UTF8, 0, fn.c_str(), -1, nullptr, 0);
    std::wstring wfn(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fn.c_str(), -1, wfn.data(), wn);
    if (!wfn.empty() && wfn.back() == L'\0') wfn.pop_back();

    std::wstring final_path = dir + L"\\" + wfn;
    std::wstring tmp_path   = final_path + L".tmp";

    HANDLE h = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD gle = GetLastError();   // capture before any other call clobbers it
        lua_pushnil(L);
        lua_pushfstring(L, "write_combatlog: cannot create file (GLE=%d)",
                        (int)gle);
        return 2;
    }
    DWORD written = 0;
    BOOL wrote_ok = WriteFile(h, contents, (DWORD)clen, &written, nullptr);
    if (wrote_ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!wrote_ok || written != clen) {
        DeleteFileW(tmp_path.c_str());
        lua_pushnil(L);
        lua_pushstring(L, "write_combatlog: write failed");
        return 2;
    }
    if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        lua_pushnil(L);
        lua_pushstring(L, "write_combatlog: rename failed");
        return 2;
    }

    int u8n = WideCharToMultiByte(CP_UTF8, 0, final_path.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    std::string u8(u8n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, final_path.c_str(), -1, u8.data(), u8n,
                        nullptr, nullptr);
    if (!u8.empty() && u8.back() == '\0') u8.pop_back();
    lua_pushstring(L, u8.c_str());
    return 1;
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

// Pops { color = "...", icon = "..." } from the Lua stack at `idx` and
// applies it to waypoint `id`. Each field optional (nil/missing = keep
// current). Raises a Lua error on unknown string. #229
static void apply_style_opts(lua_State* L, int idx, int id) {
    uint8_t cur_color = 0, cur_icon = 0;
    const auto& wps = farever::waypoints_get();
    for (const auto& w : wps)
        if (w.id == id) { cur_color = w.color; cur_icon = w.icon; break; }

    uint8_t color = cur_color, icon = cur_icon;

    lua_getfield(L, idx, "color");
    if (!lua_isnil(L, -1)) {
        const char* s = luaL_checkstring(L, -1);
        int ci = farever::color_index(s);
        if (ci < 0) luaL_error(L, "unknown color: %s", s);
        color = (uint8_t)ci;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "icon");
    if (!lua_isnil(L, -1)) {
        const char* s = luaL_checkstring(L, -1);
        int ii = farever::icon_index(s);
        if (ii < 0) luaL_error(L, "unknown icon: %s", s);
        icon = (uint8_t)ii;
    }
    lua_pop(L, 1);

    farever::waypoints_set_style(id, color, icon);
}

// farever.waypoints.add(x, y, [z], [name], [opts]) -> id (nil if full).
// opts is { color = "...", icon = "..." } -- both optional, strings.
int api_waypoints_add(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float z = (float)luaL_optnumber(L, 3, 0.0);
    const char* name = luaL_optstring(L, 4, "Waypoint");
    int id = farever::waypoints_add(x, y, z, name);
    if (id == 0) { lua_pushnil(L); return 1; }
    if (lua_istable(L, 5)) apply_style_opts(L, 5, id);
    lua_pushinteger(L, id);
    return 1;
}

// farever.waypoints.set_style(id, opts) -> bool.
// opts is { color = "...", icon = "..." } -- both individually optional.
// Returns false if no waypoint has that id. Raises on unknown string. #229
int api_waypoints_set_style(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Guard: only apply if the id exists, so an unknown id is a clean
    // return false instead of raising on color name lookup against zeros.
    const auto& wps = farever::waypoints_get();
    bool exists = false;
    for (const auto& w : wps) if (w.id == id) { exists = true; break; }
    if (!exists) { lua_pushboolean(L, 0); return 1; }

    apply_style_opts(L, 2, id);
    lua_pushboolean(L, 1);
    return 1;
}

// farever.waypoints.list() -> { {id, name, x, y, z, color, icon}, ... }
int api_waypoints_list(lua_State* L) {
    const auto& wps = farever::waypoints_get();
    lua_createtable(L, (int)wps.size(), 0);
    int i = 1;
    for (const auto& w : wps) {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, w.id);   lua_setfield(L, -2, "id");
        lua_pushstring (L, w.name); lua_setfield(L, -2, "name");
        lua_pushnumber (L, w.x);    lua_setfield(L, -2, "x");
        lua_pushnumber (L, w.y);    lua_setfield(L, -2, "y");
        lua_pushnumber (L, w.z);    lua_setfield(L, -2, "z");
        lua_pushstring (L, farever::kColorNames[w.color < 8 ? w.color : 0]);
        lua_setfield(L, -2, "color");
        lua_pushstring (L, farever::kIconNames[w.icon < 6 ? w.icon : 0]);
        lua_setfield(L, -2, "icon");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// farever.waypoints.remove(id) -> bool
int api_waypoints_remove(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, farever::waypoints_remove(id));
    return 1;
}

// farever.waypoints.rename(id, name) -> bool. Empty / nil name becomes
// "Waypoint" (matches the in-game rename modal). Returns false if no
// waypoint has that id.
int api_waypoints_rename(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    const char* name = luaL_optstring(L, 2, "Waypoint");
    lua_pushboolean(L, farever::waypoints_rename(id, name));
    return 1;
}

// farever.waypoints.set_primary(id) -> bool. id=0 (or nil) clears the
// current primary. Setting fails (returns false) if no waypoint has
// that id; clearing always succeeds.
int api_waypoints_set_primary(lua_State* L) {
    int id = (int)luaL_optinteger(L, 1, 0);
    lua_pushboolean(L, farever::waypoints_set_primary(id));
    return 1;
}

// farever.waypoints.get_primary() -> id or nil if none.
int api_waypoints_get_primary(lua_State* L) {
    int id = farever::waypoints_get_primary();
    if (id == 0) lua_pushnil(L);
    else         lua_pushinteger(L, id);
    return 1;
}

// farever.compass.is_visible() -> bool
int api_compass_is_visible(lua_State* L) {
    lua_pushboolean(L, farever::compass_is_visible());
    return 1;
}
// farever.compass.radius() -> meters (float)
int api_compass_radius(lua_State* L) {
    lua_pushnumber(L, farever::compass_radius_m());
    return 1;
}
// farever.compass.cardinals_on() -> bool
int api_compass_cardinals_on(lua_State* L) {
    lua_pushboolean(L, farever::compass_cardinals_on());
    return 1;
}

// farever.compass.add_marker(x, y, [opts]) -> handle (nil if the
// store is full). opts = { z=0, name="Marker", color="cyan",
// icon="pin", ttl_sec=0 (0 = until explicitly removed) }.
int api_compass_add_marker(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float z = 0.0f;
    char name_buf[64]; std::strcpy(name_buf, "Marker");
    int color = 0;
    int icon  = 0;
    double ttl = 0.0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "z");
        if (!lua_isnil(L, -1)) z = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "name");
        if (!lua_isnil(L, -1)) {
            const char* s = luaL_checkstring(L, -1);
            std::strncpy(name_buf, s, sizeof(name_buf) - 1);
            name_buf[sizeof(name_buf) - 1] = '\0';
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "color");
        if (!lua_isnil(L, -1)) {
            const char* s = luaL_checkstring(L, -1);
            int ci = farever::color_index(s);
            if (ci < 0) luaL_error(L, "unknown color: %s", s);
            color = ci;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "icon");
        if (!lua_isnil(L, -1)) {
            const char* s = luaL_checkstring(L, -1);
            int ii = farever::icon_index(s);
            if (ii < 0) luaL_error(L, "unknown icon: %s", s);
            icon = ii;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "ttl_sec");
        if (!lua_isnil(L, -1)) ttl = luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    int handle = farever::compass_add_marker(
        x, y, z, name_buf, (unsigned char)color, (unsigned char)icon, ttl);
    if (handle == 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, handle);
    return 1;
}

// farever.compass.remove_marker(handle) -> bool
int api_compass_remove_marker(lua_State* L) {
    int h = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, farever::compass_remove_marker(h));
    return 1;
}

// farever.party.count() -> integer (other members, excluding self).
int api_party_count(lua_State* L) {
    const auto& s = farever::party_state_read();
    lua_pushinteger(L, s.count);
    return 1;
}

// farever.party.is_in_party() -> bool.
int api_party_is_in_party(lua_State* L) {
    const auto& s = farever::party_state_read();
    lua_pushboolean(L, s.count > 0);
    return 1;
}

// farever.party.list() -> array of tables, one per other member.
int api_party_list(lua_State* L) {
    const auto& s = farever::party_state_read();
    lua_createtable(L, s.count, 0);
    for (int i = 0; i < s.count; ++i) {
        const farever::PartyMember& m = s.members[i];
        lua_createtable(L, 0, 11);
        lua_pushstring (L, m.name);              lua_setfield(L, -2, "name");
        lua_pushstring (L, m.class_id);          lua_setfield(L, -2, "class");      // #73
        lua_pushinteger(L, (lua_Integer)m.uid);  lua_setfield(L, -2, "uid");
        lua_pushnumber (L, m.x);                 lua_setfield(L, -2, "x");
        lua_pushnumber (L, m.y);                 lua_setfield(L, -2, "y");
        lua_pushnumber (L, m.z);                 lua_setfield(L, -2, "z");
        lua_pushnumber (L, m.rot_z);             lua_setfield(L, -2, "rot_z");
        lua_pushnumber (L, m.health);            lua_setfield(L, -2, "health");     // #73
        lua_pushnumber (L, m.max_health);        lua_setfield(L, -2, "max_health"); // #73
        lua_pushboolean(L, m.attr_ok);           lua_setfield(L, -2, "attr_ok");    // #73
        lua_pushboolean(L, m.hero_valid);        lua_setfield(L, -2, "hero_valid");
        lua_rawseti(L, -2, i + 1);
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

// #99: imgui.icon(skill_kind [, size_px]) -> bool
// Draws the game's own icon for that skill at the cursor. Returns false
// when the icon is not resolved yet (or the atlas is missing) so the
// plugin can fall back to text.
int api_imgui_icon(lua_State* L) {
    const char* kind = luaL_checkstring(L, 1);
    float px = (float)luaL_optnumber(L, 2, 24.0);
    SkillGfx g{};
    bool ok = kind && *kind && skill_resolve_lookup(kind, &g) &&
              overlay_draw_atlas_cell(g.atlas_filename, g.x, g.y, g.size,
                                      g.width, g.height, px);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// #99: imgui.atlas_icon(atlas, cell_x, cell_y, cell_size, size_px
//                       [, cell_w, cell_h]) -> bool
// Raw form for plugins that carry their own atlas/cell table (e.g. built
// from the `icon` field of skills()). Same shipped atlases under
// data/atlases/UI/icons/.
int api_imgui_atlas_icon(lua_State* L) {
    const char* atlas = luaL_checkstring(L, 1);
    int cx   = (int)luaL_checkinteger(L, 2);
    int cy   = (int)luaL_checkinteger(L, 3);
    int csz  = (int)luaL_checkinteger(L, 4);
    float px = (float)luaL_optnumber(L, 5, 24.0);
    int cw   = (int)luaL_optinteger(L, 6, 1);
    int ch   = (int)luaL_optinteger(L, 7, 1);
    lua_pushboolean(
        L, overlay_draw_atlas_cell(atlas, cx, cy, csz, cw, ch, px) ? 1 : 0);
    return 1;
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
    BIND(name);   // #71: character name
    BIND(class);  // #90: player class (Rogue / Mage / Priest / Warrior)
    BIND(uid);    // #87: stable account uid (FareverLogs correlation key)

    // Equipped weapon (Hero.weaponInHand chase)
    BIND(weapon_kind); BIND(weapon_level); BIND(weapon_upgrade);

    // Full loadout + active statuses (Lua array returns)
    BIND(equipment); BIND(statuses);

    // #94: arsenal / selected weapon skills (Lua array of {slot, kind})
    BIND(weapon_skills);

    // #96: skills with effective/base cooldown (Lua array of
    //      {kind, cooldown, base_cooldown, charges})
    BIND(skills);

    // #93: currencies / gold (Lua array of {kind, amount})
    BIND(currencies);

    // #93: bag inventory (Lua array of {kind, level, upgrade})
    BIND(inventory);

    // #71: full character sheet (primaries + secondaries) as displayed
    BIND(stats);

    // #109: stable per-character id, "" until a character has been observed
    // for a few seconds. Same key the mod uses for its own per-character
    // files, so it also identifies which profile is active.
    BIND(character_key);

    // Player codex lookup by Unit.kind id (issue #44)
    BIND(codex);
    // #105: the whole bestiary as an array, for index-based access
    BIND(codex_list);

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
    // #91: matching caps (rage_max, combo_point_max, ...)
    BIND(poise_max); BIND(oxygen_max); BIND(rage_max); BIND(spark_max);
    BIND(combo_point_max); BIND(focus_max); BIND(energy_max);
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
    lua_pushcfunction(L, api_store_get_char);            // #109
    lua_setfield(L, -2, "get_char");
    lua_pushcfunction(L, api_store_set_char);            // #109
    lua_setfield(L, -2, "set_char");
    lua_setfield(L, -2, "store");

    lua_pushcfunction(L, api_toast); lua_setfield(L, -2, "toast");
    lua_pushcfunction(L, api_sound); lua_setfield(L, -2, "sound");
    lua_pushcfunction(L, api_farever_now);  lua_setfield(L, -2, "now");
    lua_pushcfunction(L, api_farever_pois); lua_setfield(L, -2, "pois");
    lua_pushcfunction(L, api_write_combatlog);
    lua_setfield(L, -2, "write_combatlog");   // #87 FareverLogs.com

    lua_newtable(L);  // farever.waypoints
    lua_pushcfunction(L, api_waypoints_add);         lua_setfield(L, -2, "add");
    lua_pushcfunction(L, api_waypoints_set_style);   lua_setfield(L, -2, "set_style");
    lua_pushcfunction(L, api_waypoints_rename);      lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, api_waypoints_set_primary); lua_setfield(L, -2, "set_primary");
    lua_pushcfunction(L, api_waypoints_get_primary); lua_setfield(L, -2, "get_primary");
    lua_pushcfunction(L, api_waypoints_list);        lua_setfield(L, -2, "list");
    lua_pushcfunction(L, api_waypoints_remove);      lua_setfield(L, -2, "remove");
    lua_setfield(L, -2, "waypoints");

    lua_newtable(L);  // farever.compass
    lua_pushcfunction(L, api_compass_is_visible);    lua_setfield(L, -2, "is_visible");
    lua_pushcfunction(L, api_compass_radius);        lua_setfield(L, -2, "radius");
    lua_pushcfunction(L, api_compass_cardinals_on);  lua_setfield(L, -2, "cardinals_on");
    lua_pushcfunction(L, api_compass_add_marker);    lua_setfield(L, -2, "add_marker");
    lua_pushcfunction(L, api_compass_remove_marker); lua_setfield(L, -2, "remove_marker");
    lua_setfield(L, -2, "compass");

    lua_newtable(L);  // farever.icons  (#99)
    lua_pushcfunction(L, api_icons_skill);  lua_setfield(L, -2, "skill");
    lua_pushcfunction(L, api_icons_cached); lua_setfield(L, -2, "cached");  // #104
    lua_setfield(L, -2, "icons");

    lua_newtable(L);  // farever.camera  (#105)
    lua_pushcfunction(L, api_camera_rotation_z); lua_setfield(L, -2, "rotation_z");
    lua_pushcfunction(L, api_camera_available);  lua_setfield(L, -2, "available");
    lua_setfield(L, -2, "camera");

    lua_newtable(L);  // farever.party
    lua_pushcfunction(L, api_party_count);       lua_setfield(L, -2, "count");
    lua_pushcfunction(L, api_party_is_in_party); lua_setfield(L, -2, "is_in_party");
    lua_pushcfunction(L, api_party_list);        lua_setfield(L, -2, "list");
    lua_setfield(L, -2, "party");

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
    lua_pushcfunction(L, api_imgui_icon);               lua_setfield(L, -2, "icon");
    lua_pushcfunction(L, api_imgui_atlas_icon);         lua_setfield(L, -2, "atlas_icon");
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

// #105: the plugin manager can switch individual plugins off. The choice is
// persisted as one plugin name per line in the user-data folder, so it is a
// file the user can inspect and edit by hand. Absent file means everything is
// on, which is the pre-v1.2.6 behaviour.
constexpr const wchar_t* kDisabledFile = L"plugins_disabled.txt";

std::unordered_set<std::string> load_disabled_plugins() {
    std::unordered_set<std::string> out;
    std::wstring path = user_data_path(kDisabledFile);
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty() && line[0] != '#') out.insert(line);
    }
    return out;
}

void save_disabled_plugins() {
    std::wstring path = user_data_path(kDisabledFile);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        logf("plugins: could not write %ls", path.c_str());
        return;
    }
    out << "# Plugins switched off in the plugin manager, one name per line.\n";
    for (const auto& p : g_plugins)
        if (!p.enabled) out << p.name << "\n";
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
    // #105: optional settings hook. A plugin that defines on_settings() gets a
    // gear button in the plugin manager which opens a separate window and
    // calls it there, so options do not have to clutter the main window.
    lua_getglobal(p.L, "on_settings");
    p.wants_settings = lua_isfunction(p.L, -1); lua_pop(p.L, 1);

    // A plugin can opt out of the shared overlay theme with a global
    // `plugin_theme = false`. Only an explicit false opts out; absent or any
    // other value keeps the plugin themed.
    lua_getglobal(p.L, "plugin_theme");
    p.theme_opt_out = (lua_isboolean(p.L, -1) && !lua_toboolean(p.L, -1));
    lua_pop(p.L, 1);

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
    logf("plugins: '%s' loaded (render=%d event=%d settings=%d)%s",
         p.name.c_str(), p.has_render ? 1 : 0, p.has_event ? 1 : 0,
         p.wants_settings ? 1 : 0, p.enabled ? "" : " [off]");
    return true;
}

void scan_plugins_dir() {
    std::wstring dir = plugins_dll_dir() + L"\\data\\plugins";
    std::wstring pattern = dir + L"\\*.lua";
    const auto disabled_names = load_disabled_plugins();   // #105

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf("plugins: no plugins folder or no .lua files at %ls", dir.c_str());
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // #108: a plugin's persistent store is itself a .lua file living
        // next to the plugin, so the *.lua pattern picked it up and listed
        // "<name>.store" as a plugin of its own. It loaded (it is a bare
        // `return { ... }`), so it had no hooks and did nothing, but it
        // still showed up with a checkbox and a reload button.
        if (is_store_file(fd.cFileName)) continue;

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

        p.enabled = disabled_names.find(p.name) == disabled_names.end();
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
            // #87: victim name (== farever.target.name()) + blocked amount
            lua_pushstring(L, ev.target_kind.c_str());
            lua_setfield(L, -2, "target");
            lua_pushnumber(L, ev.blocked);       lua_setfield(L, -2, "blocked");
            break;
        case PluginEvent::Kind::HealDealt:   // #70
            lua_pushstring(L, ev.skill.c_str()); lua_setfield(L, -2, "skill");
            lua_pushnumber(L, ev.amount);        lua_setfield(L, -2, "amount");
            lua_pushboolean(L, ev.is_crit);      lua_setfield(L, -2, "is_crit");
            lua_pushstring(L, ev.target_kind.c_str());   // #87 recipient name
            lua_setfield(L, -2, "target");
            break;
        case PluginEvent::Kind::ShieldApplied:   // #70
            lua_pushstring(L, ev.skill.c_str()); lua_setfield(L, -2, "skill");
            lua_pushnumber(L, ev.amount);        lua_setfield(L, -2, "amount");
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
        case PluginEvent::Kind::CharacterLogin:   // #109
            lua_pushstring(L, ev.character_key.c_str());
            lua_setfield(L, -2, "key");
            lua_pushstring(L, ev.character_name.c_str());
            lua_setfield(L, -2, "name");
            break;
    }
}

const char* event_name(PluginEvent::Kind k) {
    switch (k) {
        case PluginEvent::Kind::HeroLocked:    return "hero_locked";
        case PluginEvent::Kind::DamageDealt:   return "damage_dealt";
        case PluginEvent::Kind::HealDealt:     return "heal_dealt";
        case PluginEvent::Kind::ShieldApplied: return "shield_applied";
        case PluginEvent::Kind::FightStart:    return "fight_start";
        case PluginEvent::Kind::FightEnd:      return "fight_end";
        case PluginEvent::Kind::TargetChanged: return "target_changed";
        case PluginEvent::Kind::CastStart:     return "cast_start";
        case PluginEvent::Kind::CastEnd:       return "cast_end";
        case PluginEvent::Kind::WeaponChanged: return "weapon_changed";
        case PluginEvent::Kind::CharacterLogin: return "character_login";
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
        if (!p.enabled) continue;                          // #105
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
    // Auto-theme plugins to match the overlay unless the user turned it off.
    const bool theme_on = overlay_plugin_theme_enabled();
    for (auto& p : g_plugins) {
        if (!p.enabled) continue;                          // #105
        if (!p.init_ok || !p.has_render || !p.L) continue;
        // Open one ImGui window per plugin. Plugin author cannot
        // begin / end windows themselves, keeping the surface area
        // small. Plugin name doubles as the ImGui window ID, so two
        // plugins with the same filename would collide - but a path
        // scan would have to land on two same-named files for that to
        // happen, which the .lua extension + FindFirstFile pattern
        // prevents.
        //
        // Bracket the whole window (and its widgets) with the overlay theme so
        // the plugin renders in our look with no plugin-side change. The pop
        // must run on every path out (including the Begin-failed continue) to
        // keep the ImGui style stack balanced.
        const bool themed = theme_on && !p.theme_opt_out;
        if (themed) overlay_push_plugin_theme();
        bool open = true;
        ImGuiWindowFlags wf = ImGuiWindowFlags_AlwaysAutoResize;
        if (!ImGui::Begin(p.name.c_str(), &open, wf)) {
            ImGui::End();
            if (themed) overlay_pop_plugin_theme();
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
        if (themed) overlay_pop_plugin_theme();
    }
}

// #105: separate window per plugin that asked for one, driven from the render
// pass so a disabled plugin's settings stay reachable (you may well want to
// change an option before switching it back on).
void run_settings_hooks() {
    const bool theme_on = overlay_plugin_theme_enabled();
    for (auto& p : g_plugins) {
        if (!p.settings_open || !p.wants_settings || !p.init_ok || !p.L)
            continue;
        const bool themed = theme_on && !p.theme_opt_out;
        if (themed) overlay_push_plugin_theme();
        std::string title = p.name + " settings";
        bool open = true;
        if (!ImGui::Begin(title.c_str(), &open,
                          ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            if (themed) overlay_pop_plugin_theme();
            if (!open) p.settings_open = false;
            continue;
        }
        lua_getglobal(p.L, "on_settings");
        if (lua_isfunction(p.L, -1)) {
            if (lua_pcall(p.L, 0, 0, 0) != LUA_OK) {
                p.last_error = lua_tostring(p.L, -1);
                logf("plugins: '%s' on_settings error: %s",
                     p.name.c_str(), p.last_error.c_str());
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                                   "error: %s", p.last_error.c_str());
                lua_pop(p.L, 1);
            }
        } else {
            lua_pop(p.L, 1);
        }
        ImGui::End();
        if (themed) overlay_pop_plugin_theme();
        if (!open) p.settings_open = false;
    }
}

}  // namespace

void plugins_set_disabled(bool disabled) {
    g_disabled.store(disabled);
}

bool plugins_start() {
    if (g_disabled.load()) {
        logf("plugins: disabled by data/no_plugins.flag - skipping scan");
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
    run_settings_hooks();   // #105
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

// #109: called from the hl_pump worker when the per-character profile settles
// on a new key. That switch is already debounced (the same name has to be read
// on three consecutive checks), which is exactly the semantics asked for:
// "hero_locked but more basic". hero_locked fires on every Hero re-lock,
// including a zone change or a dungeon exit on the SAME character; this fires
// once per actual character.
void plugins_set_character(const char* key, const char* name) {
    if (!key || !*key) return;
    PluginEvent ev;
    ev.k = PluginEvent::Kind::CharacterLogin;
    {
        std::lock_guard<std::mutex> lk(g_character_mtx);
        if (g_character_key == key) return;          // unchanged
        g_character_key  = key;
        g_character_name = name ? name : "";
        ev.character_key  = g_character_key;
        ev.character_name = g_character_name;
    }
    logf("plugins: character is now '%s' (key=%s) - emitting character_login",
         ev.character_name.c_str(), ev.character_key.c_str());
    enqueue_event(std::move(ev));
}

void plugins_emit_damage_dealt(const char* skill_name, double amount,
                               bool is_crit, bool is_kill,
                               const char* target_name, double blocked) {
    PluginEvent ev;
    ev.k           = PluginEvent::Kind::DamageDealt;
    ev.skill       = skill_name ? skill_name : "";
    ev.amount      = amount;
    ev.is_crit     = is_crit;
    ev.is_kill     = is_kill;
    ev.target_kind = target_name ? target_name : "";   // #87 victim name
    ev.blocked     = blocked;
    enqueue_event(std::move(ev));
}

// #70: per-heal event for plugins. Fed from aggregator::record_heal, which
// already drains local-player heals (effect==1 DamageResult). Shields are a
// separate effect value and are not read yet, so no shield event is emitted.
void plugins_emit_heal_dealt(const char* skill_name, double amount,
                             bool is_crit, const char* target_name) {
    PluginEvent ev;
    ev.k           = PluginEvent::Kind::HealDealt;
    ev.skill       = skill_name ? skill_name : "";
    ev.amount      = amount;
    ev.is_crit     = is_crit;
    ev.target_kind = target_name ? target_name : "";   // #87 recipient name
    enqueue_event(std::move(ev));
}

// #70: shield-applied event, synthesized by hero_state when the player's total
// active-status absorb rises from ~0 to positive. `skill` is the status kind
// that granted it (e.g. "Mage_ShieldOfSpark_Status").
void plugins_emit_shield_applied(const char* status_kind, double amount) {
    PluginEvent ev;
    ev.k      = PluginEvent::Kind::ShieldApplied;
    ev.skill  = status_kind ? status_kind : "";
    ev.amount = amount;
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
            ImGui::PushID(&p);
            // #105: on / off switch. Unchecking keeps the plugin loaded but
            // stops it drawing and stops it receiving events, so it costs
            // nothing and switching back on is immediate.
            if (ImGui::Checkbox("##enabled", &p.enabled)) {
                save_disabled_plugins();
                logf("plugins: '%s' switched %s", p.name.c_str(),
                     p.enabled ? "on" : "off");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show this plugin and send it events");
            ImGui::SameLine();

            ImU32 col = !p.enabled  ? IM_COL32(150, 150, 150, 255)
                        : p.init_ok ? IM_COL32(180, 220, 130, 255)
                                    : IM_COL32(255, 140, 100, 255);
            ImGui::TextColored(
                ImVec4(((col >> 0) & 0xff) / 255.0f,
                       ((col >> 8) & 0xff) / 255.0f,
                       ((col >> 16) & 0xff) / 255.0f, 1.0f),
                "%s%s", p.name.c_str(),
                !p.init_ok ? "  (error)" : (p.enabled ? "" : "  (off)"));

            ImGui::SameLine();
            if (ImGui::SmallButton("reload")) {
                load_plugin(p);
            }
            // #105: gear button, only for plugins that define on_settings().
            if (p.wants_settings) {
                ImGui::SameLine();
                if (ImGui::SmallButton("settings")) {
                    p.settings_open = !p.settings_open;
                }
            }
            if (!p.last_error.empty()) {
                ImGui::Indent();
                ImGui::TextWrapped("%s", p.last_error.c_str());
                ImGui::Unindent();
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
