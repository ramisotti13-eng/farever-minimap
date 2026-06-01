-- ==============================================================
-- damage-calculator-by-iskrumpie.lua
-- Submitted by @iSkrumpie  (https://github.com/ramisotti13-eng/farever-minimap/pull/51)
-- Tested against farever-mod v1.1.1
-- License: MIT
--
-- PvE damage calculator using Aragon's verified formula — rating inputs,
-- enemy armor presets, visual bars, stat gain analysis, dual-attribute support.
--
-- v1.3.0 (2026-06-01):
--   + Balance patch: Crit formula /1250 → /1555 (every 15.56 rating = 1%)
--   + Base crit 5.7% added as universal character constant
--   + Stat gain: absolute avg-dmg delta (+X.X avg) shown alongside %
--   + Stat gain: +20 Attribute simulation added
-- ==============================================================
-- Based on Aragon's formula: https://www.youtube.com/watch?v=zcIMQjmQXlI

-- ── constants ─────────────────────────────────────────────────────────────────
local NAV_W    = 360          -- forces window content width (see dummy below)
local DEF_MOD  = 2285.0       -- game defense scaling constant (Aragon verified)
local GAIN_SIM = 20           -- delta used in gain-analysis simulation (ratings + attr)
local BASE_CRIT = 5.7         -- base crit % all characters have (confirmed balance patch 2026-06)

-- ── rating → % converters ─────────────────────────────────────────────────────
local function fervor_pct(r)    return r / 15.0 end
local function armor_pen_pct(r) return math.min(r / 6.0, 100.0) end
local function crit_pct(r)      return r * 100.0 / 1555.0 end   -- PATCHED: was r/12.5 (/1250)

-- ── damage formula ────────────────────────────────────────────────────────────
-- All *_p arguments are actual % values (5.8, not 0.058).
--
-- Skill tooltip format: "36 + 78.75%[STR]" means:
--   base_weapon_flat  = 36
--   attribute_scaling = 78.75%  ← applied to the ATTRIBUTE, not the weapon
-- → pre_def base = attr × skill_mod% + weapon  (Aragon verified vs sheet)
local function compute(weapon, skill_mod, attr1, attr2,
                        fervor_p, mastery_p, armor_pen_p, crit_p, crit_bonus,
                        bonus1, bonus2, armor)
    local pre_def = ((attr1 + attr2) * (skill_mod / 100.0) + weapon)
                  * (1.0 + fervor_p  / 100.0)
                  * (1.0 + mastery_p / 100.0)
                  * (1.0 + bonus1    / 100.0)
                  * (1.0 + bonus2    / 100.0)
    local eff_armor  = math.max(0.0, armor * (1.0 - armor_pen_p / 100.0))
    local def_factor = DEF_MOD / (DEF_MOD + eff_armor)
    local normal     = pre_def * def_factor
    local crit_hit   = normal  * (crit_bonus / 100.0)
    local avg        = normal  * (1.0 - crit_p / 100.0)
                     + crit_hit * (crit_p / 100.0)
    return normal, crit_hit, avg, def_factor
end

-- Wrapper: takes raw ratings, converts, calls compute()
local function compute_r(w, sm, a1, a2, fr, mast, apr, cr, cb, b1, b2, armor)
    return compute(w, sm, a1, a2,
                   fervor_pct(fr), mast, armor_pen_pct(apr),
                   BASE_CRIT + crit_pct(cr),   -- total = 5.7% base + rating-derived
                   cb, b1, b2, armor)
end

-- ── default state ─────────────────────────────────────────────────────────────
-- Values match Aragon's reference build from the video.
local function make_defaults()
    return {
        weapon      = 36.0,
        skill_mod   = 78.75,  -- %  (applied to attribute, not weapon)
        attr1       = 34.0,
        attr2       = 0.0,
        dual_attr   = false,
        fervor_r    = 87.0,   -- rating → 5.8%
        mastery     = 0.0,    -- % direct (physical or magic mastery from buff/talent)
        armor_pen_r = 63.0,   -- rating → 10.5%
        crit_r      = 0.0,    -- rating → 0.0%
        crit_bonus  = 150.7,  -- % full multiplier (150 = 1.50x, i.e. +50%)
        bonus1      = 25.0,   -- % extra damage bonus 1 (skill rank-up, affix…)
        bonus2      = 0.0,
        enemy_armor = 1500.0,
    }
end

-- ── mutable state ─────────────────────────────────────────────────────────────
local s = make_defaults()

-- ── lifecycle ─────────────────────────────────────────────────────────────────
function on_init()
    local d = make_defaults()
    for k, v in pairs(d) do
        s[k] = farever.store.get("dc_" .. k, v)
    end
    farever.log.info("damage_calculator: ready")
end

local function save()
    for k, v in pairs(s) do
        farever.store.set("dc_" .. k, v)
    end
end

-- Track weapon swaps: remember damage per weapon kind.
function on_event(name, data)
    if name == "weapon_changed" then
        if data.prev_kind ~= "" then
            farever.store.set("dc_wdc_" .. data.prev_kind, s.weapon)
        end
        local saved = farever.store.get("dc_wdc_" .. data.kind, nil)
        if saved then
            s.weapon = saved
            farever.toast(string.format(
                "Weapon: %s (restored dmg %.0f)", data.kind, s.weapon))
        else
            farever.toast(string.format(
                "Weapon: %s lvl %d  →  enter weapon damage",
                data.kind, data.level))
        end
        save()
    end
end

-- ── draw helpers ──────────────────────────────────────────────────────────────

-- Full-width damage bar with label + value text.
-- max_val: reference maximum (used to scale bar width).
local function draw_damage_bar(label, value, max_val, r, g, b)
    local ax, ay  = imgui.cursor_pos()
    local lbl_w   = 54    -- label column width (left of bar)
    local val_w   = 46    -- value column width (right of bar)
    local bar_w   = NAV_W - lbl_w - val_w - 4
    local bar_h   = 13
    local bx      = ax + lbl_w
    local fill    = (max_val > 0) and math.min(value / max_val, 1.0) or 0.0

    -- label — left of bar
    imgui.draw_text(ax, ay + 1, 0.80, 0.80, 0.80, 1.0, label)

    -- bar background
    imgui.draw_rect_filled(bx, ay, bx + bar_w, ay + bar_h,
                           0.10, 0.10, 0.16, 0.85)
    -- bar fill
    if fill > 0 then
        imgui.draw_rect_filled(bx, ay, bx + bar_w * fill, ay + bar_h,
                               r, g, b, 0.85)
    end
    -- bar border
    imgui.draw_rect(bx, ay, bx + bar_w, ay + bar_h,
                    0.28, 0.28, 0.40, 0.75, 1.0)

    -- value — right of bar
    imgui.draw_text(bx + bar_w + 5, ay + 1, r, g, b, 1.0,
                    string.format("%.1f", value))

    imgui.dummy(NAV_W, bar_h + 2)
end

-- Stat gain row: label, absolute avg-dmg delta, gain%, optional "◀ best" marker.
local function draw_gain_row(label, abs_dmg, gain_pct, is_best)
    if is_best then
        imgui.text_colored(1.0, 0.85, 0.20, 1.0,
            string.format("  %-13s  %+5.1f avg  %+.2f%%  ◀ best", label, abs_dmg, gain_pct))
    else
        imgui.text_colored(0.55, 0.55, 0.55, 1.0,
            string.format("  %-13s  %+5.1f avg  %+.2f%%", label, abs_dmg, gain_pct))
    end
end

-- ── enemy presets ─────────────────────────────────────────────────────────────
local PRESETS = {
    { label = "Dummy",   armor = 0    },
    { label = "Z1 Mob",  armor = 300  },
    { label = "Z1 Boss", armor = 800  },
    { label = "Lady B",  armor = 1500 },
}

-- ── main render ───────────────────────────────────────────────────────────────
function on_render()
    -- force window content width = NAV_W (same trick as POI Finder)
    imgui.dummy(NAV_W, 0)

    local changed = false
    local function drag(lbl, key, speed, lo, hi)
        local v, c = imgui.drag_float(lbl, s[key], speed, lo, hi)
        if c then s[key] = v; changed = true end
    end

    -- ── equipped weapon (read-only info) ──────────────────────────────────────
    if farever.player and farever.player.weapon_kind then
        local wk = farever.player.weapon_kind()
        if wk ~= "" then
            imgui.text_colored(0.55, 0.55, 0.55, 1.0, string.format(
                "Equipped: %s  lvl %d  upg %d",
                wk, farever.player.weapon_level(),
                farever.player.weapon_upgrade()))
        end
    end

    imgui.separator()

    -- ── WEAPON & SKILL ────────────────────────────────────────────────────────
    imgui.text_colored(0.75, 0.75, 1.0, 1.0, "Weapon & Skill")
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        '  Skill tooltip shows e.g. "36 + 78.75% [STR]"')

    drag("Weapon base dmg  (the \"36\" in tooltip)",   "weapon",    0.5, 0.0, 10000.0)
    drag("Skill mod %      (the \"78.75%\" in tooltip)", "skill_mod", 0.1, 0.0,  1000.0)
    drag("Attribute value  (your STR/DEX/INT/FAI)",    "attr1",     0.5, 0.0,  1000.0)

    -- dual-attribute toggle
    local da, dc = imgui.checkbox("Dual-scaling skill  (e.g. FAI + INT)", s.dual_attr)
    if dc then s.dual_attr = da; changed = true end
    if s.dual_attr then
        drag("Attribute 2  (second stat value)", "attr2", 0.5, 0.0, 1000.0)
    else
        s.attr2 = 0.0
    end

    imgui.separator()

    -- ── CHARACTER RATINGS ─────────────────────────────────────────────────────
    imgui.text_colored(0.75, 0.75, 1.0, 1.0, "Character Ratings")
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        "  Enter the rating number from your stat page (not the %)")

    drag(string.format("Fervor rating       → %5.1f%%  (15 pts = 1%%)",
         fervor_pct(s.fervor_r)), "fervor_r", 1.0, 0.0, 2000.0)

    drag("Mastery %  (Phys./Magic Mastery buff value)", "mastery", 0.1, 0.0, 200.0)
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        "  Mastery: enter the % directly (e.g. 15 from Prayer of Virtue)")

    drag(string.format("Armor pen rating    → %5.1f%%  (6 pts = 1%%)",
         armor_pen_pct(s.armor_pen_r)), "armor_pen_r", 1.0, 0.0, 600.0)

    drag(string.format("Crit chance rating  → %5.1f%% total  (15.56 pts = 1%%)",
         BASE_CRIT + crit_pct(s.crit_r)), "crit_r", 1.0, 0.0, 1555.0)
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        string.format("  base %.1f%% + rating  (balance patch 2026-06)", BASE_CRIT))

    drag("Crit bonus %  (150% = 1.5x normal, i.e. +50%)",
         "crit_bonus", 0.1, 100.0, 500.0)

    imgui.separator()

    -- ── DAMAGE BONUSES ────────────────────────────────────────────────────────
    imgui.text_colored(0.75, 0.75, 1.0, 1.0, "Extra Damage Bonuses")
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        "  Flat % boosts from skill rank-ups, talents or affixes")
    imgui.text_colored(0.42, 0.42, 0.42, 1.0,
        "  e.g. Conquer rank-up gives +25% — check skill details")
    drag("Bonus 1 %", "bonus1", 0.5, 0.0, 500.0)
    drag("Bonus 2 %", "bonus2", 0.5, 0.0, 500.0)

    imgui.separator()

    -- ── ENEMY ─────────────────────────────────────────────────────────────────
    imgui.text_colored(0.75, 0.75, 1.0, 1.0, "Enemy")

    -- preset buttons on one line
    for i, p in ipairs(PRESETS) do
        local active = math.abs(s.enemy_armor - p.armor) < 1.0
        if active then
            imgui.text_colored(1.0, 0.85, 0.2, 1.0, "[" .. p.label .. "]")
        else
            if imgui.button(p.label) then
                s.enemy_armor = p.armor
                changed = true
            end
        end
        if i < #PRESETS then imgui.same_line() end
    end

    drag("Enemy armor", "enemy_armor", 5.0, 0.0, 10000.0)

    imgui.separator()

    -- ── RESULTS ───────────────────────────────────────────────────────────────
    local normal, crit_hit, avg, def_factor =
        compute_r(s.weapon, s.skill_mod, s.attr1, s.attr2,
                  s.fervor_r, s.mastery, s.armor_pen_r, s.crit_r,
                  s.crit_bonus, s.bonus1, s.bonus2, s.enemy_armor)

    local armor_redux = (1.0 - def_factor) * 100.0
    local max_bar = math.max(normal, crit_hit, avg) * 1.08  -- 8% headroom

    imgui.text_colored(0.75, 0.75, 1.0, 1.0, "Results")

    draw_damage_bar("Normal", normal,   max_bar, 0.35, 0.65, 1.00)
    draw_damage_bar("Crit",   crit_hit, max_bar, 1.00, 0.72, 0.20)
    draw_damage_bar("Avg",    avg,      max_bar, 0.40, 0.92, 0.40)

    if s.enemy_armor > 0 then
        imgui.text_colored(0.55, 0.55, 0.55, 1.0, string.format(
            "  Armor reduction: %.1f%%   (eff. armor: %.0f)",
            armor_redux,
            s.enemy_armor * (1.0 - armor_pen_pct(s.armor_pen_r) / 100.0)))
    else
        imgui.text_colored(0.55, 0.55, 0.55, 1.0, "  No armor (dummy)")
    end

    -- live damage_modifier from player (v0.6+) ─────────────────────────────
    if farever.player.locked() then
        local dm = farever.player.damage_modifier()
        if dm and math.abs(dm - 1.0) > 0.005 then
            imgui.text_colored(0.40, 0.95, 0.55, 1.0, string.format(
                "  Buff modifier: \xc3\x97%.3f  \xe2\x86\x92 effective avg: %.1f",
                dm, avg * dm))
        end
        -- live enemy armor hint if target exists
        if farever.target.exists() then
            local ta = farever.target.armor()
            if ta and ta > 0 then
                imgui.text_colored(0.42, 0.42, 0.42, 1.0, string.format(
                    "  Target armor (live): %.0f  [drag Enemy armor to match]", ta))
            end
        end
    end

    imgui.separator()

    -- ── STAT GAIN ANALYSIS ────────────────────────────────────────────────────
    imgui.text_colored(0.75, 0.75, 1.0, 1.0,
        string.format("Stat Gain  (+%d each: ratings | attribute)", GAIN_SIM))

    local function sim_avg(key, delta)
        local sc = {}
        for k, v in pairs(s) do sc[k] = v end
        sc[key] = sc[key] + delta
        local _, _, a = compute_r(sc.weapon, sc.skill_mod, sc.attr1, sc.attr2,
                                   sc.fervor_r, sc.mastery, sc.armor_pen_r, sc.crit_r,
                                   sc.crit_bonus, sc.bonus1, sc.bonus2, sc.enemy_armor)
        return a
    end

    local gains = {
        { label = "Fervor",    key = "fervor_r",    gain = 0, abs = 0 },
        { label = "Armor Pen", key = "armor_pen_r", gain = 0, abs = 0 },
        { label = "Crit",      key = "crit_r",      gain = 0, abs = 0 },
        { label = "Attribute", key = "attr1",        gain = 0, abs = 0 },
    }
    local best_i    = 1
    local best_gain = -math.huge

    for i, g in ipairs(gains) do
        local new_avg = sim_avg(g.key, GAIN_SIM)
        g.abs  = new_avg - avg
        g.gain = avg > 0 and ((new_avg - avg) / avg * 100.0) or 0.0
        if g.gain > best_gain then
            best_gain = g.gain
            best_i    = i
        end
    end

    for i, g in ipairs(gains) do
        draw_gain_row(g.label, g.abs, g.gain, i == best_i)
    end

    imgui.separator()

    -- ── footer ────────────────────────────────────────────────────────────────
    if imgui.button("Reset defaults") then
        s = make_defaults()
        changed = true
        farever.toast("Damage Calculator: reset to defaults")
    end

    if changed then save() end
end
