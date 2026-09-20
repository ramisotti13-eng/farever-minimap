#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace farever {

// One slot in Hero.loadout.equipment. kind is the internal id string
// (e.g. "Helmet_Bronze_A"); level and upgrade come from the item
// object's level / upgradeLevel fields. #87: `slot` is the fixed
// EquipmentSlot ordinal (the item's index in the game's content array,
// stable across characters) and `slot_name` the canonical name for it
// ("Head", "Chest", "FingerLeft", "Weapon1", ...) so two rings can be
// told apart. slot_name is empty for an unmapped index. For inventory
// items (which are not gear-slotted) slot is -1 and slot_name empty.
struct EquippedItem {
    std::string kind;
    int         level     = 0;
    int         upgrade   = 0;
    int         slot      = -1;
    std::string slot_name;
    int         stack     = 1;   // #93: inventory slot stack count (itemCount);
                                 // 1 for equipment/non-stacking items
};

// #93: one entry of the player's currencies (gold + other counted currencies).
// These live in Loadout.currencies (NOT the item bag); each is an ItemKind id
// with an amount. Crafting materials live in the item bag (inventory()) as
// stackable st.Item entries instead.
struct CurrencyStack {
    std::string kind;      // ItemKind id ("Gold", ...)
    int         amount = 0;
};

// One active status (buff or debuff) on the hero. kind is the internal
// id ("Bleed", "ManaSurge", ...). duration is the total in seconds
// (from the status' duration field). stacks is the current stack count.
// All read from st.skill.Status; ordered as the game's statuses array
// exposes them.
struct ActiveStatus {
    std::string kind;
    double      duration = 0.0;
    int         stacks   = 0;
    double      shield_amount = 0.0;   // #70: st.skill.Status.shieldAmount @ 424,
                                       // the live absorb this status grants (0 for
                                       // non-shield statuses)
};

// One row of the in-game character sheet, captured live from its UI
// widgets (#71). `label` is the localized attribute name exactly as the
// player sees it ("Vitalität", "Krit. Trefferchance", ...); `value` is the
// displayed number parsed out (124, 11.1, 879); `text` is the raw display
// string ("124", "11.1%", "879"). This is the complete, language- and
// class-agnostic view - the typed HeroSnapshot fields cover the primaries.
struct UiStat {
    std::string label;
    std::string text;
    double      value = 0.0;
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
    bool   dying;           // ent.Unit.dying (local hero is dead/dying)

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
    double shield;            // #70: live TOTAL absorb, summed from active statuses
                             // (st.skill.Status.shieldAmount). NOT the inline UA
                             // `shield` field, which reads a dead 0.
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
    // #91: the cap for each class resource, read off the same HUD widget as
    // the current value. 0 means "no bar seen for this resource" (a class that
    // does not have it, or the HUD has not built yet). max_combo_point in
    // particular is not a constant, a rune can raise it.
    double poise_max;
    double oxygen_max;
    double rage_max;
    double spark_max;
    double combo_point_max;
    double focus_max;
    double energy_max;
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

    // #90: the player's class, one of "Rogue" / "Mage" / "Priest" /
    // "Warrior", or empty when it hasn't resolved yet (pre-spawn). Derived
    // from the class-specific skill scripts the hero has learned (their
    // runtime class name is "script.skills.<Class>_..."); not a guess from
    // equipped gear. Exposed as farever.player.class().
    std::string player_class;

    // Full loadout (everything other than the weapon-in-hand). Walked
    // from Hero.loadout @ 1192 -> Loadout.equipment @ 96 ->
    // Equipment.content @ 96 (ArrayObj). Empty when the chain hasn't
    // resolved yet (mid-spawn, transition). Capped to a sane size to
    // bound the per-frame cost. Order matches the game's content array.
    std::vector<EquippedItem> equipment;

    // #94: the arsenal / selected weapon skills, in slot order. Walked from
    // ent.Hero.weaponSkills @ 1408 (ArrayObj of st.skill.Skill, same layout
    // as Hero.skills), each entry is the skill's `kind` id ("Mage_RayOfSpark",
    // "DS_Bladeleaf_Skill2"). The arsenal holds up to two skills; a two-hander
    // in the arsenal still shows its weapon skill here, which is what lets a
    // proc tracker tell whether e.g. Pyroclasm is actually slotted. Empty until
    // resolved (pre-spawn / no arsenal). Exposed as
    // farever.player.weapon_skills() -> {{slot=1, kind=...}, ...}.
    std::vector<std::string> weapon_skills;

    // #93: the player's bag inventory (everything not equipped). Walked from
    // Hero.loadout -> Loadout.inventory @ 136 -> Inventory.content @ 120, the
    // same item layout as equipment (kind / level / upgrade). Empty until
    // resolved. Capped to bound per-frame cost. Exposed as
    // farever.player.inventory().
    std::vector<EquippedItem> inventory;

    // #93: currencies (gold + other counted currencies) from Loadout.currencies
    // @ 168 (an hxbit.ArrayProxyData of {amount, kind} proxies). Crafting
    // materials are NOT here - they are stackable items in the bag (inventory()).
    // Exposed as farever.player.currencies().
    std::vector<CurrencyStack> currencies;

    // Active statuses (buffs / debuffs) on the hero. Walked from
    // ent.Unit.statuses and filtered to the ones the hero actually owns
    // (#110 - the old source was instigatedStatuses, which is what the hero
    // applied to OTHERS). Each entry carries the status kind string
    // ("Bleed", "ManaSurge"), total duration in seconds, and current stack
    // count. Plugins compute remaining time client-side from
    // startTime + duration - now if they need a count-down.
    std::vector<ActiveStatus> statuses;
};

// Register the ent.Hero alloc-hook watcher. The watcher pushes raw
// Hero pointers into a pending list; hero_state_tick (called per
// frame from Present) waits until each candidate's constructor has
// populated ownerPlayer, then verifies isMe + bidirectional Player.hero.
//
// Multiple Heroes get allocated per zone transition (you + remote
// players + NPCs in some cases). Exactly one of them ever satisfies
// the isMe check - that's the local player. Once locked, we
// re-validate every 64 ticks to survive dungeon transitions.
void hero_state_start();
void hero_state_stop();

// Per-frame validate + position read. Called from the Present hook
// (same thread as damage_tick / overlay).
void hero_state_tick();

HeroSnapshot hero_state_read();

// #71: the full set of character-sheet rows captured from the live UI
// (see UiStat). Empty until the character menu has been opened at least
// once this session; refreshes whenever it (re)builds. Returns a copy,
// safe to call from the plugin / render thread.
std::vector<UiStat> hero_state_ui_stats();

// Drop everything cached about the character we were playing: the throttled
// loadout snapshots (equipment, inventory, currencies, class, arsenal) and the
// values scraped from the HUD and character sheet.
//
// Without this a switch inherits the previous character's numbers, because the
// blip guards on those caches deliberately keep the old value when a scan comes
// back empty, and "empty" is exactly what a character who owns none of that
// reads. Reported by @dpiza for currencies (gold carried over between
// characters); it applied to every cached list.
//
// Called from the character-switch path in hl_pump, so it runs on the pump
// thread and must stay lock-safe.
void hero_state_reset_caches();

// Raw pointer to the currently-locked Hero, or 0 if no lock. Cheap
// atomic load - used by the damage filter to drop DamageResults whose
// serverSource isn't us (= incoming damage, bleeds etc.).
std::uintptr_t hero_state_locked_ptr();

// #62: the local character's display name (UTF-8), read from
// ent.Hero.name @ 1240 off the locked hero. Returns true and fills `out`
// when a hero is locked and the name is non-empty; false otherwise (e.g.
// before lock). Used to pick the per-character POI-progress profile.
bool hero_state_local_name(std::string& out);

// #87: the local player's stable account uid (st.Player.uid @ 160). Unlike
// Hero.__uid (per-login), this is constant across sessions, so FareverLogs
// can use it to correlate one player's logs. Returns true and fills `out`
// (UTF-8) when a player is locked and the uid is non-empty; false otherwise.
// #87 (2026-07-25): a per-CHARACTER id is NOT reachable from the client. The
// id exists as st.player.HeroData.databaseID, but Player.heroData@232 reads
// NULL on our side (its neighbours accountProgress@224 and progress@240 both
// resolve to their expected classes, so this is replication, not a bad
// offset). HeroStats.firstConnectionTime is 0 client-side as well, and the
// game's own saves are keyed by account uid / character name. Don't re-derive
// this without new evidence.
bool hero_state_local_uid(std::string& out);

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
