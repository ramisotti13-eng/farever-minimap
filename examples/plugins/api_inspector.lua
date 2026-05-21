-- ==============================================================
-- api_inspector.lua
--
-- Living documentation of the farever-mod plugin API. Shows every
-- read-surface getter as a labelled value in an ImGui window. Use it
-- as a reference when writing your own plugins: each section maps to
-- a `farever.player.*`, `farever.target.*` namespace and demonstrates
-- the value format you get back from each call.
--
-- Drop this file into data/plugins/ and the panel appears next to the
-- other plugin windows. No state, no events handled, just reads.
-- ==============================================================

local function fmt_pct(v) return string.format("%.2f%%", v * 100.0) end
local function fmt_num(v) return string.format("%.1f", v) end

function on_init()
    farever.log.info("api_inspector: ready (read-only API demo)")
end

local function section_player_position()
    imgui.text("--- farever.player.* (position) ---")
    imgui.text(string.format("locked     = %s", tostring(farever.player.locked())))
    imgui.text(string.format("x, y, z    = %.1f, %.1f, %.1f",
        farever.player.x(), farever.player.y(), farever.player.z()))
    imgui.text(string.format("rot_z      = %.3f rad", farever.player.rot_z()))
    imgui.text(string.format("in_combat  = %s", tostring(farever.player.in_combat())))
    imgui.text(string.format("level      = %d", farever.player.level()))
    imgui.text(string.format("has_target = %s", tostring(farever.player.has_target())))
end

local function section_player_health()
    imgui.text("--- farever.player.* (health / energy) ---")
    imgui.text(string.format("health     = %.0f / %.0f  (%.0f%%)",
        farever.player.health(), farever.player.max_health(),
        farever.player.health_pct() * 100.0))
    imgui.text(string.format("shield     = %.0f", farever.player.shield()))
    imgui.text(string.format("energy     = %.0f  (+%.1f /s)",
        farever.player.energy(), farever.player.energy_regen()))
end

local function section_player_stats()
    imgui.text("--- farever.player.* (attributes) ---")
    imgui.text(string.format("VIT %.0f  STR %.0f  DEX %.0f  FAI %.0f  INT %.0f",
        farever.player.vitality(), farever.player.strength(),
        farever.player.dexterity(), farever.player.faith(),
        farever.player.intellect()))
    imgui.text(string.format("crit %s / %s   armor pen %s",
        fmt_pct(farever.player.crit_chance()),
        fmt_pct(farever.player.crit_damage()),
        fmt_pct(farever.player.armor_penetration())))
    imgui.text(string.format("fervor %s   spell pen %s",
        fmt_pct(farever.player.fervor()),
        fmt_pct(farever.player.spell_penetration())))
    imgui.text(string.format("armor %.0f   magic_armor %.0f",
        farever.player.armor(), farever.player.magic_armor()))
    imgui.text("(note: many values read BASE only, real values live in")
    imgui.text(" UnitAttributes.attributes MapData. See damage_planner.lua)")
end

local function section_player_resources()
    imgui.text("--- farever.player.* (hero-only resources) ---")
    imgui.text(string.format("rage %.0f (+%.1f /s)  spark %.0f (+%.1f /s)",
        farever.player.rage(), farever.player.rage_regen(),
        farever.player.spark(), farever.player.spark_regen()))
    imgui.text(string.format("focus %.0f   combo %.0f   poise %.0f",
        farever.player.focus(), farever.player.combo_point(),
        farever.player.poise()))
end

local function section_player_weapon()
    imgui.text("--- farever.player.* (equipped weapon) ---")
    local wk = farever.player.weapon_kind and farever.player.weapon_kind() or ""
    if wk == "" then
        imgui.text("(no weapon read, possibly mid-swap or older DLL)")
    else
        imgui.text(string.format("weapon_kind    = %s", wk))
        imgui.text(string.format("weapon_level   = %d",
            farever.player.weapon_level()))
        imgui.text(string.format("weapon_upgrade = %d",
            farever.player.weapon_upgrade()))
    end
end

local function section_player_equipment()
    imgui.text("--- farever.player.equipment() ---")
    local items = farever.player.equipment and farever.player.equipment() or nil
    if not items or #items == 0 then
        imgui.text("(no equipment list, older DLL or empty loadout)")
        return
    end
    for i, it in ipairs(items) do
        imgui.text(string.format("[%d] %s   lvl %d   upg %d",
            i, it.kind, it.level, it.upgrade))
    end
end

local function section_player_statuses()
    imgui.text("--- farever.player.statuses() ---")
    local s = farever.player.statuses and farever.player.statuses() or nil
    if not s or #s == 0 then
        imgui.text("(no statuses active)")
        return
    end
    for i, st in ipairs(s) do
        imgui.text(string.format("[%d] %s   duration %.1fs   stacks %d",
            i, st.kind, st.duration, st.stacks))
    end
end

local function section_target()
    imgui.text("--- farever.target.* ---")
    if not farever.target.exists() then
        imgui.text("(no target locked)")
        return
    end
    imgui.text(string.format("name       = %s", farever.target.name()))
    imgui.text(string.format("level      = %d", farever.target.level()))
    imgui.text(string.format("hp         = %.0f / %.0f  (%.0f%%)",
        farever.target.hp(), farever.target.max_hp(),
        farever.target.hp_pct() * 100.0))
    imgui.text(string.format("x, y, z    = %.1f, %.1f, %.1f",
        farever.target.x(), farever.target.y(), farever.target.z()))
    imgui.text(string.format("armor      = %.0f", farever.target.armor()))
    imgui.text(string.format("magic_armor= %.0f", farever.target.magic_armor()))
    if farever.target.is_casting() then
        imgui.text(string.format("CASTING    = %s   %.1f/%.1fs",
            farever.target.cast_skill(),
            farever.target.cast_elapsed_sec(),
            farever.target.cast_total_sec()))
    else
        imgui.text("(not casting)")
    end
end

function on_render()
    imgui.text("=== farever-mod plugin API inspector ===")
    section_player_position(); imgui.separator()
    section_player_health();   imgui.separator()
    section_player_stats();    imgui.separator()
    section_player_resources();imgui.separator()
    section_player_weapon();   imgui.separator()
    section_player_equipment();imgui.separator()
    section_player_statuses(); imgui.separator()
    section_target()
end

-- Reference for plugin authors:
--   farever.player.*       see sections above
--   farever.target.*       see sections above
--   farever.log.{info,warn,error}(msg)
--   farever.toast(msg)     short floating notification
--   farever.sound(name)    "alert" / "warning" / "info" / "beep"
--   farever.store.get(key, default)
--   farever.store.set(key, value)        persisted to <plugin>.store.lua
--
--   imgui.text(s) / imgui.text_colored(r,g,b,a,s)
--   imgui.separator() / imgui.same_line() / imgui.spacing()
--   imgui.button(label) -> bool
--   imgui.checkbox(label, current) -> value, changed
--   imgui.slider_float(label, val, min, max) -> value, changed
--   imgui.drag_float(label, val, speed, min, max) -> value, changed
--   imgui.input_text(label, current) -> string, changed
--   imgui.combo(label, current_index, table_of_strings) -> index, changed
--   imgui.color_edit(label, r, g, b) -> r, g, b, changed
--   imgui.progress(value_0_to_1, overlay_text)
--
--   Events (declare a global on_event function to receive):
--     hero_locked
--     fight_start    { fight_id }
--     fight_end      { fight_id, duration, total_damage, dps, top_skill }
--     damage_dealt   { skill, amount, is_crit, is_kill }
--     target_changed { kind }
--     cast_start     { skill, total_sec }
--     cast_end       { skill, duration }
--     weapon_changed { kind, prev_kind, level, upgrade }
