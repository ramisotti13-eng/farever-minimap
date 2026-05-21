-- ==============================================================
-- damage_planner.lua  (v1.1.0)
--
-- In-game version of Aragon's PvE damage calculator for Farever.
-- Two builds side by side, type your stats in once, tweak with
-- sliders, see the resulting average damage. Same math as Aragon's
-- spreadsheet, verified against his two reference builds.
--
-- This file is intentionally a clean reference plugin: it ships
-- the formula and the UX so other plugin authors can fork it and
-- add the bits that are still missing (auto-read of player stats
-- and equipped-item stats; see "Future work" at the bottom).
-- ==============================================================

-- ----------------------------------------------------------------
-- Aragon's PvE damage formula
-- ----------------------------------------------------------------
--
--   pre_def    = (weapon * skill_mod + attribute)
--              * (1 + fervor)
--              * (1 + dmg_bonus1)
--              * (1 + dmg_bonus2)
--   def_factor = def_mod / (def_mod + enemy_armor * (1 - armor_pen))
--   normal_hit = pre_def * def_factor
--   crit_hit   = normal_hit * (crit_bonus / 100)
--   avg        = normal * (1 - crit_chance) + crit * crit_chance
--
-- All percentage inputs come in as PERCENT here (8.3 = 8.3%).
-- crit_bonus is the FULL crit multiplier in percent (150 = 1.50x).
-- def_mod is a game-level constant (Aragon uses 2285).
--
-- Verified against Aragon's two reference builds (both compute to
-- 52 normal, 79 crit, 52 avg at crit_chance 0).

-- ----------------------------------------------------------------
-- Where the inputs come from in-game
-- ----------------------------------------------------------------
--
-- Open your character sheet ("Attribute" tab). All values you need
-- are on the two attribute pages. Mapping (German label -> field):
--
--   Vitalität                 -> (not used in damage formula)
--   Stärke                    -> attribute   (if your skill scales on STR)
--   Geschicklichkeit          -> attribute   (DEX-scaling skills)
--   Glaube                    -> attribute   (FAITH-scaling skills)
--   Intelligenz               -> attribute   (INT-scaling skills, e.g. Staff)
--   Krit. Trefferchance       -> crit_chance %
--   Krit. Bonus               -> crit_bonus %
--   Rüstungsdurchdringung     -> armor_pen % (for physical skills)
--   Mag. Durchdringung        -> armor_pen % (for magic skills)
--   Eifer                     -> fervor %
--   Rüstung                   -> NOT used for outgoing damage; that's
--                                YOUR defense against incoming hits.
--                                The "enemy armor" field in this plugin
--                                is the FOE's armor, not yours.
--
-- Weapon damage and skill modifier are not on the attribute page:
--   weapon damage  -> on the equipped weapon's tooltip (a number or
--                     range like "Damage 36-42"; use the average or
--                     max if a range)
--   skill_mod %    -> per-skill, in the skill's tooltip text
--                     ("verursacht X% Waffenschaden")
--
-- Buff / affix damage bonuses go into dmg_bonus 1 and 2. Most builds
-- leave them at 0 unless a specific buff is active.

-- ----------------------------------------------------------------
-- What this plugin does NOT auto-read (yet)
-- ----------------------------------------------------------------
--
-- The mod can read live HashLink memory and exposes a lot of player
-- attributes via `farever.player.*` getters. We tried wiring those
-- straight into Build A, but it turned out the f64 fields on the
-- UnitAttributes class are BASE values only:
--
--   farever.player.crit_chance()   -> 5    (class default, not your 8.9%)
--   farever.player.crit_damage()   -> 150  (class default, not your 150.6%)
--   farever.player.strength()      -> 0    (your real 31 is somewhere else)
--   farever.player.fervor()        -> 0    (your real 9.4% is elsewhere)
--   farever.player.damage()        -> 0    (weapon damage is elsewhere)
--
-- The real final stats apparently live in the hxbit MapData hanging
-- off `UnitAttributes.attributes @ 16`, layered on top of those base
-- values by gear / talents / buffs. Reading that MapData is a bigger
-- project (different keys/values than progress_state's bytes_map).
--
-- For now: type the values from your character sheet once, the
-- sliders do the rest.
--
-- See "Future work" at the bottom for the pointer chain to the
-- equipped weapon if someone wants to take the next step.

-- ==============================================================
-- Plugin code below
-- ==============================================================

local function default_build()
    return {
        weapon       = 36.0,
        attr         = 34.0,
        skill_mod    = 78.75,     -- %
        fervor       = 5.8,       -- %
        crit_chance  = 0.0,       -- % (0..100)
        crit_bonus   = 150.7,     -- % full crit multiplier (150 = 1.50x)
        armor_pen    = 10.5,      -- %
        dmg_bonus    = 25.0,      -- % buff/affix bonus 1
        dmg_bonus2   = 0.0,       -- % buff/affix bonus 2
    }
end

local A = default_build()
local B = default_build()
B.fervor    = 8.3
B.armor_pen = 4.5

local enemy_armor = 1500.0    -- Aragon's reference: generic Z1 mob
local def_mod     = 2285.0    -- game-level defense scaling constant

local edit_target = 1   -- 1 = Build A, 2 = Build B
local edit_choices = { "Build A", "Build B" }

-- Per-weapon weapon-damage memory. weapon_damages[kind] = number.
-- When the equipped weapon changes (weapon_changed event) we remember
-- the current Build A's weapon_damage under the OLD kind, then look up
-- the saved value (if any) for the new kind and apply it. Saves typing
-- weapon damage every time you swap.
local weapon_damages = {}    -- "Staff_Craft_C" -> 36.0
local last_weapon_kind = ""

-- ---- persistence -----------------------------------------------

local function save()
    for _, side in ipairs({ "A", "B" }) do
        local tbl = (side == "A") and A or B
        for k, v in pairs(tbl) do
            farever.store.set(side .. "_" .. k, v)
        end
    end
    farever.store.set("enemy_armor", enemy_armor)
    farever.store.set("def_mod",     def_mod)
    farever.store.set("edit_target", edit_target)
    -- Flatten per-weapon damages into per-key entries
    for kind, dmg in pairs(weapon_damages) do
        farever.store.set("wd_" .. kind, dmg)
    end
end

local function load()
    for _, side in ipairs({ "A", "B" }) do
        local tbl = (side == "A") and A or B
        for k, dflt in pairs(default_build()) do
            tbl[k] = farever.store.get(side .. "_" .. k, dflt)
        end
    end
    -- First-run Aragon split if nothing saved yet:
    if not farever.store.get("A_fervor", nil) then
        A.fervor = 5.8;  A.armor_pen = 10.5
        B.fervor = 8.3;  B.armor_pen = 4.5
    end
    enemy_armor = farever.store.get("enemy_armor", 1500.0)
    def_mod     = farever.store.get("def_mod",     2285.0)
    edit_target = farever.store.get("edit_target", 1)
end

-- ---- formula ---------------------------------------------------

local function compute(b, armor, defmod)
    local fervor    = b.fervor      / 100.0
    local arm_pen   = b.armor_pen   / 100.0
    local crit_pct  = b.crit_chance / 100.0
    local crit_mult = b.crit_bonus  / 100.0
    local bonus1    = b.dmg_bonus   / 100.0
    local bonus2    = b.dmg_bonus2  / 100.0
    local skill     = b.skill_mod   / 100.0

    local pre_def = (b.weapon * skill + b.attr)
                  * (1 + fervor)
                  * (1 + bonus1)
                  * (1 + bonus2)
    local eff_armor = armor * (1 - arm_pen)
    if eff_armor < 0 then eff_armor = 0 end
    local def_factor = defmod / (defmod + eff_armor)
    local normal = pre_def * def_factor
    local crit   = normal * crit_mult
    local avg    = normal * (1 - crit_pct) + crit * crit_pct
    return normal, crit, avg
end

-- ---- plugin entry points ---------------------------------------

function on_init()
    if not farever.player then return end
    load()
    farever.log.info("damage_planner: ready")
end

-- Auto-track weapon: when the equipped weapon kind changes, save the
-- current Build A's weapon_damage under the previous kind so swapping
-- back later restores it; then apply any saved damage for the new kind.
-- Build B is left alone so it can act as a "saved baseline build".
function on_event(name, data)
    if name == "weapon_changed" then
        if data.prev_kind ~= "" then
            weapon_damages[data.prev_kind] = A.weapon
        end
        local restored = weapon_damages[data.kind]
        if restored then
            A.weapon = restored
            farever.toast(string.format(
                "weapon: %s (lvl %d upg %d) -> dmg %.0f",
                data.kind, data.level, data.upgrade, A.weapon))
        else
            farever.toast(string.format(
                "weapon: %s (lvl %d upg %d) -- enter weapon damage",
                data.kind, data.level, data.upgrade))
        end
        last_weapon_kind = data.kind
        farever.store.set("wd_" .. data.kind, A.weapon)
    end
end

function on_render()
    imgui.text("--- damage planner (Aragon PvE) ---")

    -- Show the live equipped weapon. If weapon_kind reads as empty
    -- the chain failed this frame (e.g. mid-swap) -- skip the line.
    if farever.player and farever.player.weapon_kind then
        local wk = farever.player.weapon_kind()
        if wk ~= "" then
            imgui.text(string.format("Equipped: %s   lvl %d   upg %d",
                wk, farever.player.weapon_level(),
                farever.player.weapon_upgrade()))
        end
    end

    local nA, cA, aA = compute(A, enemy_armor, def_mod)
    local nB, cB, aB = compute(B, enemy_armor, def_mod)

    local target_name = "(reference)"
    if farever.target and farever.target.exists() then
        target_name = farever.target.name()
    end
    imgui.text(string.format("vs %s   armor %.0f   def_mod %.0f",
                             target_name, enemy_armor, def_mod))

    imgui.separator()
    imgui.text(string.format("A   Normal %6.1f   Crit %6.1f   Avg %6.1f",
                             nA, cA, aA))
    imgui.text(string.format("B   Normal %6.1f   Crit %6.1f   Avg %6.1f",
                             nB, cB, aB))

    if aB > 0 and aA > 0 then
        local g, label
        if edit_target == 1 then
            g     = (aA - aB) / aB * 100.0
            label = "A vs B"
        else
            g     = (aB - aA) / aA * 100.0
            label = "B vs A"
        end
        if g > 0.05 then
            imgui.text_colored(0.4, 1.0, 0.4, 1.0,
                               string.format("%s: +%.2f%%", label, g))
        elseif g < -0.05 then
            imgui.text_colored(1.0, 0.5, 0.5, 1.0,
                               string.format("%s: %.2f%%", label, g))
        else
            imgui.text(string.format("%s: %.2f%%", label, g))
        end
    end

    imgui.separator()
    imgui.text("Enemy / scaling:")
    local changed = false
    local v, c

    v, c = imgui.drag_float("enemy armor", enemy_armor, 1.0, 0.0, 50000.0)
    if c then enemy_armor = v; changed = true end
    v, c = imgui.drag_float("def_mod (game const)", def_mod, 1.0, 100.0, 20000.0)
    if c then def_mod = v; changed = true end

    imgui.separator()
    v, c = imgui.combo("edit", edit_target, edit_choices)
    if c then edit_target = v; changed = true end

    local b = (edit_target == 1) and A or B

    v, c = imgui.drag_float("weapon damage",   b.weapon,      0.1, 0.0, 10000.0)
    if c then b.weapon = v; changed = true end
    v, c = imgui.drag_float("attribute",       b.attr,        0.1, 0.0, 1000.0)
    if c then b.attr = v; changed = true end
    v, c = imgui.drag_float("skill modifier %",b.skill_mod,   0.1, 0.0, 1000.0)
    if c then b.skill_mod = v; changed = true end
    v, c = imgui.drag_float("fervor %",        b.fervor,      0.05, 0.0, 200.0)
    if c then b.fervor = v; changed = true end
    v, c = imgui.drag_float("crit chance %",   b.crit_chance, 0.1, 0.0, 100.0)
    if c then b.crit_chance = v; changed = true end
    v, c = imgui.drag_float("crit bonus %",    b.crit_bonus,  0.1, 100.0, 500.0)
    if c then b.crit_bonus = v; changed = true end
    v, c = imgui.drag_float("armor pen %",     b.armor_pen,   0.1, 0.0, 100.0)
    if c then b.armor_pen = v; changed = true end
    v, c = imgui.drag_float("damage bonus 1 %",b.dmg_bonus,   0.5, 0.0, 500.0)
    if c then b.dmg_bonus = v; changed = true end
    v, c = imgui.drag_float("damage bonus 2 %",b.dmg_bonus2,  0.5, 0.0, 500.0)
    if c then b.dmg_bonus2 = v; changed = true end

    if imgui.button("Copy A to B") then
        for k in pairs(default_build()) do B[k] = A[k] end
        changed = true
    end
    imgui.same_line()
    if imgui.button("Copy B to A") then
        for k in pairs(default_build()) do A[k] = B[k] end
        changed = true
    end
    imgui.same_line()
    if imgui.button("Reset both") then
        A = default_build()
        B = default_build()
        B.fervor = 8.3; B.armor_pen = 4.5
        changed = true
    end

    if changed then save() end
end

-- ==============================================================
-- Future work (for whoever wants to take this further)
-- ==============================================================
--
-- 1. (DONE in v0.5.6) Auto-read the equipped weapon kind / level /
--    upgrade. Available as farever.player.weapon_kind() / .weapon_level()
--    / .weapon_upgrade() plus a weapon_changed event (data.kind,
--    data.prev_kind, data.level, data.upgrade). This plugin uses both to
--    remember weapon_damage per weapon kind.
--
--    Still missing: the actual base weapon damage value, which lives
--    in Item.inf @ 96 (HVIRTUAL[17 fields]) on st.item.Weapon. The
--    17 field names are NOT in the bytecode dump. Two routes to recover
--    them and unlock fully-automatic damage planning:
--
--    a) Decode the HVIRTUAL header at runtime. The HashLink mod-side
--       already has helpers for HVIRTUAL unwrap (see target_state.cpp,
--       the HKIND_HVIRTUAL=15 branch). From inf+8 you get a vdynamic*
--       and from there hl_dyn_getp / hl_dyn_geti / hl_hash_utf8 can
--       look up fields by name once you know the names.
--
--    b) CDB lookup. The game ships a compiled `data.cdb` with the
--       static definition of every item kind. The mod has unfinished
--       extraction tools at tools/extract_cdb_xml.py + cdb_atlas.cpp.
--       Reading Item.kind ("Staff_Craft_C") as a key into the CDB
--       gets you weapon damage, scaling stat, scaling factor etc.
--       without any HVIRTUAL decoding.
--
-- 2. Auto-read the full loadout (armor, helmet, ring, etc.).
--
--      ent.Hero.loadout        @ 1192  -> st.Loadout
--      st.Loadout.equipment    @   96  -> st.Equipment
--      st.Equipment.content    @   96  -> ArrayObj of items
--
--    Each item in the array is either st.item.Weapon, st.item.Armor,
--    or st.item.Gear. They all share the .inf HVIRTUAL at offset 96.
--    Walking this gives you per-slot stat contributions.
--
-- 3. Auto-read final player stats (not just base).
--
--      ent.UnitAttributes.attributes @ 16  -> hxbit.MapData
--
--    This MapData is presumably keyed by an attribute enum (Strength,
--    Vitality, ...) with f64 values. Different key/value layout than
--    the bytes_map progress_state already reads for activities.
--    Decoding it gets you the LIVE final stats (base + gear + buffs)
--    in one read, no per-item walking required.
--
-- 4. Per-skill modifier.
--
--    Skills attached to the weapon are at:
--      ent.Hero.weaponSkills @ 1376  -> ArrayObj of skills
--    The currently casting / last-cast skill could feed `skill_mod`
--    automatically. BaseSkill.inf @ 152 holds the multiplier info.
--
-- Any of these would make the planner truly live. Until then,
-- type your stats once, slide, compare. It is still useful that way.
