#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace farever {

// One slot in Hero.loadout.equipment. kind is the internal id string
// (e.g. "Helmet_Bronze_A"); level and upgrade come from the item
// object's level / upgradeLevel fields. Equipment slots are not
// labelled with their semantic position (helmet vs ring vs etc.) at
// this layer; the order matches the game's content array.
struct EquippedItem {
    std::string kind;
    int         level   = 0;
    int         upgrade = 0;
};

// One active status (buff or debuff) on the hero. kind is the internal
// id ("Bleed", "ManaSurge", ...). duration is the total in seconds
// (from the status' duration field). stacks is the current stack count.
// All read from st.skill.Status; ordered as the game's instigatedStatuses
// array exposes them.
struct ActiveStatus {
    std::string kind;
    double      duration = 0.0;
    int         stacks   = 0;
};

// Snapshot of the local Hero used by the minimap render and the plugin
// API. Numeric attributes come from Hero.attr (HeroAttributes /
// UnitAttributes). All `attr_ok`-gated values are zero when the lock
// is not held or when the attr chase fails this tick.
struct HeroSnapshot {
    bool   locked;          // false until isMe + bidirectional check passes
    double x, y, z;
    double rot_z;
    bool   in_combat;       // ent.Hero.isInCombat
    double combat_start;    // Hero.combatStartTime (game seconds)
    int    level;           // Hero._level
    bool   has_target;      // Hero.target != 0

    bool   attr_ok;         // true if attr pointer + reads succeeded

    // Primary character stats
    double vitality;
    double strength;
    double dexterity;
    double faith;
    double intellect;

    // Combat
    double crit_chance;
    double crit_damage;
    double armor_penetration;
    double spell_penetration;
    double fervor;
    double block_mitigation;
    double dodge_chance;
    double magic_mastery;
    double physical_mastery;
    double spell_cast_time_reduction;
    double knock_resistance;
    double cooldown_reduction;

    // Defense
    double armor;
    double magic_armor;
    double magic_reduction;

    // Health and energy
    double health;
    double max_health;
    double health_regen;
    double shield;
    double energy;
    double energy_regen;

    // Misc
    double move_speed_factor;
    double damage;            // base damage modifier
    double heal;              // base heal output

    // Hero-only (HeroAttributes layer)
    bool   hero_attr_ok;
    double poise;
    double poise_regen;
    double oxygen;
    double rage;
    double rage_regen;
    double spark;
    double spark_regen;
    double combo_point;
    double focus;
    double damage_modifier;
    double damage_taken_modifier;
    double heal_given_multiplier;
    double shield_power_multiplier;
    double glide_speed;

    // Equipped weapon. Chased from Hero.weaponInHand @ 1304 each frame.
    // weapon_ok is true when the pointer + kind String read succeed.
    // weapon_kind is the internal id (e.g. "Staff_Craft_C"). Level /
    // upgrade are the live integer values. Plugins can use these to
    // partition state by weapon (per-weapon personal bests, etc.) or
    // to fire their own logic on weapon change. The kind change itself
    // is also surfaced as a `weapon_changed` event.
    bool        weapon_ok;
    std::string weapon_kind;
    int         weapon_level;
    int         weapon_upgrade;

    // Full loadout (everything other than the weapon-in-hand). Walked
    // from Hero.loadout @ 1192 -> Loadout.equipment @ 96 ->
    // Equipment.content @ 96 (ArrayObj). Empty when the chain hasn't
    // resolved yet (mid-spawn, transition). Capped to a sane size to
    // bound the per-frame cost. Order matches the game's content array.
    std::vector<EquippedItem> equipment;

    // Active statuses (buffs / debuffs) on the hero. Walked from
    // ent.Unit.instigatedStatuses. Each entry carries the status kind
    // string ("Bleed", "ManaSurge"), total duration in seconds, and
    // current stack count. Plugins compute remaining time client-side
    // from startTime + duration - now if they need a count-down.
    std::vector<ActiveStatus> statuses;
};

// Register the ent.Hero alloc-hook watcher. The watcher pushes raw
// Hero pointers into a pending list; hero_state_tick (called per
// frame from Present) waits until each candidate's constructor has
// populated ownerPlayer, then verifies isMe + bidirectional Player.hero.
//
// Multiple Heroes get allocated per zone transition (you + remote
// players + NPCs in some cases). Exactly one of them ever satisfies
// the isMe check — that's the local player. Once locked, we
// re-validate every 64 ticks to survive dungeon transitions.
void hero_state_start();
void hero_state_stop();

// Per-frame validate + position read. Called from the Present hook
// (same thread as damage_tick / overlay).
void hero_state_tick();

HeroSnapshot hero_state_read();

// Raw pointer to the currently-locked Hero, or 0 if no lock. Cheap
// atomic load — used by the damage filter to drop DamageResults whose
// serverSource isn't us (= incoming damage, bleeds etc.).
std::uintptr_t hero_state_locked_ptr();

// v0.4.15 anticrash mode. When enabled, hero_state_tick switches to
// polling Player.hero via the back-reference (instead of relying on
// the alloc-hook watcher for re-locks) once the initial lock has been
// stable for 5 seconds. At that moment hero_state asks hl_hook to
// disable the hl_alloc_obj trampoline entirely so the game's allocator
// runs with zero overhead from us. Trade-off: damage tracking stops.
// Engaged by dllmain at boot when data/anticrash.flag is present.
void hero_state_set_anticrash(bool on);
bool hero_state_anticrash_armed();
bool hero_state_anticrash_disarmed();   // true after the alloc-hook
                                        // has actually been removed

}  // namespace farever
