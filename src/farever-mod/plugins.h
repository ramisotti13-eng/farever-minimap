#pragma once

// Lua-based plugin system for farever-mod. Plugin authors drop .lua
// files into data/plugins/ in the game folder; the mod auto-loads
// them at startup and re-loads on file change.
//
// Each plugin runs in its own sandboxed lua_State on the render thread
// (same thread that touches the HashLink heap and our ImGui state).
// Plugins cannot read game memory directly, write files outside their
// own state, or call OS functions. They can:
//
//   * read player position / heading / lock state
//   * read DPS / current fight totals
//   * receive damage / hero-locked / fight-start / fight-end events
//   * draw their own ImGui window (a curated subset of widgets)
//   * log to farever-mod.log
//   * store/retrieve per-plugin string-keyed state (in-memory only)
//
// Lifecycle hooks the plugin may define as Lua globals:
//   on_init()                         once at load / reload
//   on_render()                       every frame, inside an
//                                     ImGui::Begin(plugin_name) scope
//                                     we open and close for them
//   on_event(name, table)             "hero_locked" / "damage_dealt"
//                                     / "fight_start" / "fight_end"

namespace farever {

// Spawned from overlay init after the overlay is ready (so ImGui is
// alive when plugins first try to draw). Scans data/plugins/*.lua and
// loads each one.
bool plugins_start();

// Called from the render thread once per frame, after our own overlay
// windows have been drawn. Drives on_render + drains the event queue
// into on_event. Also polls mtimes for hot-reload.
void plugins_tick();

// Called from overlay shutdown / DLL detach. Closes all lua_States.
void plugins_stop();

// Event emission from the rest of the mod. Cheap to call when no
// plugins are loaded (just bumps an atomic). Events are queued and
// dispatched on the next plugins_tick.
void plugins_emit_hero_locked();
void plugins_emit_damage_dealt(const char* skill_name, double amount,
                               bool is_crit, bool is_kill);
void plugins_emit_fight_start(int fight_id);
void plugins_emit_fight_end(int fight_id, double duration_s,
                            double total_damage, double dps,
                            const char* top_skill);

// Slice 4: target / cast events emitted by target_state. The render
// thread calls these on state-change ticks; plugins receive them via
// on_event("target_changed" / "cast_start" / "cast_end", table).
void plugins_emit_target_changed(const char* kind);
void plugins_emit_cast_start(const char* skill,
                             double total_sec);   // 0 if first sighting
void plugins_emit_cast_end(const char* skill, double duration_sec);

// v0.5.6: equipped weapon change. Fires from hero_state when
// Hero.weaponInHand transitions to a new kind (also on the initial
// observation after hero lock). Plugins receive on_event("weapon_changed",
// {kind, prev_kind, level, upgrade}).
void plugins_emit_weapon_changed(const char* kind, const char* prev_kind,
                                 int level, int upgrade);

// Plugin manager UI (ImGui window). Toggle with F8 by default.
// Drawn from overlay_render when visible.
void plugins_render_manager();
bool plugins_manager_visible();
void plugins_manager_toggle();

// v0.5.3.3 diagnostic kill switch. When true, plugins_start does
// nothing and plugins_tick / plugins_emit_* are no-ops. Set from
// overlay startup if data/no_plugins.flag is present next to the DLL.
void plugins_set_disabled(bool disabled);

}  // namespace farever
