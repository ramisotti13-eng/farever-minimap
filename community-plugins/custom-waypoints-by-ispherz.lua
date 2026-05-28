-- ==============================================================
-- custom-waypoints-by-ispherz.lua
-- Submitted by @IsPherz (local draft: issue-custom-waypoints-body.md)
-- Tested against farever-mod v1.0.0-beta4
-- License: MIT
--
-- Personal waypoint markers (plugin window): add at current position,
-- list/rename/delete, and heading nav arrow.
--
-- Description / Notes
-- - Uses native waypoint API when available:
--     farever.waypoints.add/list/remove
-- - Falls back to local store-backed waypoints if native API is missing.
-- - Native waypoint fields currently exposed by the mod: id/name/x/y/z.
-- - Notes are plugin-local (stored in farever.store as waypoint_notes_blob).
-- - Native rename is emulated by remove+add at same coordinates.
-- - Arrow heading is based on character facing (farever.player.rot_z),
--   not camera direction.
-- ==============================================================

local PLUGIN_VERSION = "1.1.0"

local waypoints = {}       -- { id, name, x, y, z, note }
local waypoint_notes = {}  -- local notes keyed by waypoint id
local selected_id = nil
local show_arrow = true
local arrow_only_mode = false
local arrow_panel_offset_x = 0.0
local arrow_panel_offset_y = 0.0

local new_name = "Waypoint"
local edit_name = ""
local edit_note = ""
local use_native_waypoints = false
local last_native_sync = 0.0
local NATIVE_SYNC_SEC = 0.75

local NAV_BOX_W, NAV_BOX_H = 200, 128
local ARRIVE_XY_M = 2.0
local ARRIVE_Z_M = 2.0

-- Warm minimap-like palette (amber/gold on dark brown).
local C_HINT = { 0.83, 0.77, 0.58, 1.0 }
local C_NATIVE = { 0.92, 0.84, 0.54, 1.0 }
local C_LEGACY = { 0.86, 0.72, 0.38, 1.0 }
local C_WAIT = { 0.95, 0.72, 0.30, 1.0 }
local C_SELECTED = { 1.00, 0.90, 0.55, 1.0 }
local C_NOTE = { 0.72, 0.66, 0.52, 1.0 }
local C_COORD = { 0.64, 0.58, 0.47, 1.0 }
local C_PANEL_BG = { 0.11, 0.08, 0.05, 0.78 }
local C_PANEL_BORDER = { 0.78, 0.62, 0.27, 0.85 }

-- -- persistence (store allows string / number / bool only) -----

local function escape_field(s)
    if not s or s == "" then return "" end
    return (tostring(s):gsub("\\", "\\\\"):gsub("|", "\\|"):gsub("\n", " "))
end

local function unescape_field(s)
    if not s or s == "" then return "" end
    return (s:gsub("\\|", "|"):gsub("\\\\", "\\"))
end

local function serialize_waypoints()
    local lines = { "v1" }
    for _, w in ipairs(waypoints) do
        table.insert(lines, string.format(
            "%s|%s|%.4f|%.4f|%.4f|",
            w.id,
            escape_field(w.name),
            w.x, w.y, w.z,
            ""
        ))
    end
    return table.concat(lines, "\n")
end

local function serialize_notes()
    local lines = { "v1" }
    for id, note in pairs(waypoint_notes) do
        if note and note ~= "" then
            table.insert(lines, string.format("%s|%s", escape_field(id), escape_field(note)))
        end
    end
    return table.concat(lines, "\n")
end

local function deserialize_notes(blob)
    waypoint_notes = {}
    if not blob or blob == "" then return end
    local first = true
    for line in blob:gmatch("[^\n]+") do
        if first then
            first = false
        else
            local id, note = line:match("^([^|]+)|(.*)$")
            if id then
                waypoint_notes[unescape_field(id)] = unescape_field(note or "")
            end
        end
    end
end

local function deserialize_waypoints(blob)
    waypoints = {}
    if not blob or blob == "" then return end
    local first = true
    for line in blob:gmatch("[^\n]+") do
        if first then
            first = false
            if line ~= "v1" then
                farever.log.warn("custom_waypoints: unknown store version")
            end
        else
            local id, name, xs, ys, zs = line:match(
                "^([^|]+)|(.-)|([^|]+)|([^|]+)|([^|]+)|(.*)$")
            if id and name and xs then
                table.insert(waypoints, {
                    id   = id,
                    name = unescape_field(name),
                    x    = tonumber(xs) or 0,
                    y    = tonumber(ys) or 0,
                    z    = tonumber(zs) or 0,
                    note = "",
                })
            end
        end
    end
end

local function save_all()
    if not use_native_waypoints then
        farever.store.set("waypoints_blob", serialize_waypoints())
    end
    farever.store.set("show_arrow", show_arrow)
    farever.store.set("arrow_only_mode", arrow_only_mode)
    farever.store.set("arrow_panel_offset_x", arrow_panel_offset_x)
    farever.store.set("arrow_panel_offset_y", arrow_panel_offset_y)
    farever.store.set("waypoint_notes_blob", serialize_notes())
    if selected_id then
        farever.store.set("selected_id", selected_id)
    else
        farever.store.set("selected_id", "")
    end
end

local function new_id()
    return string.format("wp_%d", math.floor((farever.now() or 0) * 1000))
end

local function native_available()
    return farever.waypoints
        and type(farever.waypoints.add) == "function"
        and type(farever.waypoints.list) == "function"
        and type(farever.waypoints.remove) == "function"
end

local function refresh_native_waypoints()
    if not use_native_waypoints then return end
    local ok, native_list = pcall(farever.waypoints.list)
    if not ok or type(native_list) ~= "table" then
        farever.log.warn("custom_waypoints: farever.waypoints.list() failed")
        return
    end
    waypoints = {}
    for _, w in ipairs(native_list) do
        if w and w.id and w.x and w.y and w.z then
            table.insert(waypoints, {
                id = tostring(w.id),
                name = tostring(w.name or "Waypoint"),
                x = tonumber(w.x) or 0,
                y = tonumber(w.y) or 0,
                z = tonumber(w.z) or 0,
                note = waypoint_notes[tostring(w.id)] or "",
            })
        end
    end
    last_native_sync = farever.now() or last_native_sync
end

local function add_waypoint(name, x, y, z)
    if use_native_waypoints then
        local ok, id = pcall(farever.waypoints.add, x, y, z, name)
        if not ok or not id then
            return nil
        end
        refresh_native_waypoints()
        return tostring(id)
    end

    local wp = {
        id   = new_id(),
        name = name,
        x    = x,
        y    = y,
        z    = z,
        note = "",
    }
    table.insert(waypoints, wp)
    return wp.id
end

local function remove_waypoint(id)
    if use_native_waypoints then
        local ok, removed = pcall(farever.waypoints.remove, id)
        if not ok or not removed then
            return false
        end
        refresh_native_waypoints()
        return true
    end

    for i, w in ipairs(waypoints) do
        if w.id == id then
            table.remove(waypoints, i)
            return true
        end
    end
    return false
end

local function find_waypoint(id)
    for i, w in ipairs(waypoints) do
        if w.id == id then return w, i end
    end
    return nil, nil
end

local function get_active_waypoint(px, py)
    local sel = find_waypoint(selected_id)
    if sel then return sel end
    if #waypoints == 0 then return nil end
    local best, best_d2 = nil, nil
    for _, w in ipairs(waypoints) do
        local d2 = xy_dist_sq(px, py, w.x, w.y)
        if not best_d2 or d2 < best_d2 then
            best, best_d2 = w, d2
        end
    end
    if best then
        selected_id = best.id
    end
    return best
end

local function xy_dist_sq(px, py, wx, wy)
    local dx, dy = px - wx, py - wy
    return dx * dx + dy * dy
end

local function waypoint_count()
    return #waypoints
end

-- -- nav arrow (same math as nav_arrow.lua) ----------------------

local function color_for_dz(dz)
    if math.abs(dz) < 5 then return 0.4, 1.0, 0.4
    elseif dz > 0 then return 0.4, 0.7, 1.0
    else return 1.0, 0.4, 0.4 end
end

local function draw_nav_arrow(cx, cy, tx, ty, tz)
    local px = farever.player.x()
    local py = farever.player.y()
    local pz = farever.player.z()
    local heading = farever.player.rot_z()

    local dx, dy, dz = tx - px, ty - py, tz - pz
    local cosh, sinh = math.cos(heading), math.sin(heading)
    local forward = dx * cosh + dy * sinh
    local right = -dx * sinh + dy * cosh
    local d_h = math.sqrt(forward * forward + right * right)
    local theta_h = math.atan(right, forward)
    local theta_v = math.atan(dz, math.max(d_h, 1.0))

    local dir_x = math.sin(theta_h)
    local dir_y = -math.cos(theta_h)
    local perp_x, perp_y = -dir_y, dir_x

    local pulse = 0.95 + 0.05 * math.sin(farever.now() * 3)
    local L, head_l, head_w = 50.0 * pulse, 20.0 * pulse, 13.0 * pulse
    local tilt = math.sin(theta_v) * 16.0

    local tail_x = cx - dir_x * L * 0.35
    local tail_y = cy - dir_y * L * 0.35
    local tip_x = cx + dir_x * L
    local tip_y = cy + dir_y * L - tilt
    local back_x = tip_x - dir_x * head_l
    local back_y = tip_y - dir_y * head_l
    local r, g, b = color_for_dz(dz)

    imgui.draw_line(tail_x, tail_y, back_x, back_y, r, g, b, 1.0, 4.0)
    imgui.draw_triangle_filled(tip_x, tip_y,
        back_x + perp_x * head_w, back_y + perp_y * head_w,
        back_x - perp_x * head_w, back_y - perp_y * head_w,
        r, g, b, 1.0)
    return d_h, dz
end

local function draw_text_bold(x, y, r, g, b, a, text)
    imgui.draw_text(x, y, r, g, b, a, text)
    imgui.draw_text(x + 1, y, r, g, b, a, text)
end

local function draw_soft_panel(x1, y1, x2, y2, r, bg, border, alpha_mul)
    -- Clean rectangular panel (no corner circles).
    alpha_mul = alpha_mul or 1.0
    local bga = bg[4] * alpha_mul
    local boa = border[4] * alpha_mul
    imgui.draw_rect_filled(x1, y1, x2, y2, bg[1], bg[2], bg[3], bga)
    imgui.draw_rect(x1, y1, x2, y2, border[1], border[2], border[3], boa, 1.4)
end

-- -- lifecycle --------------------------------------------------

function on_init()
    deserialize_notes(farever.store.get("waypoint_notes_blob", ""))
    use_native_waypoints = native_available()
    if use_native_waypoints then
        refresh_native_waypoints()
    else
        deserialize_waypoints(farever.store.get("waypoints_blob", ""))
        for _, w in ipairs(waypoints) do
            w.note = waypoint_notes[w.id] or ""
        end
    end
    show_arrow = farever.store.get("show_arrow", true)
    arrow_only_mode = farever.store.get("arrow_only_mode", false)
    arrow_panel_offset_x = farever.store.get("arrow_panel_offset_x", 0.0)
    arrow_panel_offset_y = farever.store.get("arrow_panel_offset_y", 0.0)
    local sid = farever.store.get("selected_id", "")
    selected_id = (sid ~= "" and sid) or nil
    if selected_id and not find_waypoint(selected_id) then
        selected_id = nil
    end
    local w = find_waypoint(selected_id)
    if w then
        edit_name = w.name
        edit_note = w.note or ""
    end
    farever.log.info("custom_waypoints v" .. PLUGIN_VERSION .. " loaded (native="
        .. tostring(use_native_waypoints) .. "), "
        .. waypoint_count() .. " waypoints")
end

function on_event(name, data)
    if name == "hero_locked" then
        if use_native_waypoints then
            refresh_native_waypoints()
        end
    end
end

function on_render()
    if use_native_waypoints then
        local now = farever.now() or 0
        if now - last_native_sync >= NATIVE_SYNC_SEC then
            refresh_native_waypoints()
        end
    end

    local px = farever.player.x()
    local py = farever.player.y()
    local pz = farever.player.z()
    local heading = farever.player.rot_z()

    if arrow_only_mode then
        local sel = get_active_waypoint(px, py)
        local ax, ay = imgui.cursor_pos()
        -- Keep arrow-only panel anchored in-window so it never clips out.
        local panel_x = ax
        local panel_y = ay
        local alpha_mul = 1.0

        if show_arrow and sel then
            local cx, cy = panel_x + NAV_BOX_W * 0.5, panel_y + NAV_BOX_H * 0.42
            draw_soft_panel(panel_x, panel_y, panel_x + NAV_BOX_W, panel_y + NAV_BOX_H, 9, C_PANEL_BG, C_PANEL_BORDER, alpha_mul)
            local d_h = math.sqrt(xy_dist_sq(px, py, sel.x, sel.y))
            local dz = sel.z - pz
            local arrived = (d_h <= ARRIVE_XY_M and math.abs(dz) <= ARRIVE_Z_M)
            if not arrived then
                d_h, dz = draw_nav_arrow(cx, cy, sel.x, sel.y, sel.z)
            end
            local vertical_label
            if math.abs(dz) < 1.0 then
                vertical_label = "Same height"
            elseif dz > 0 then
                vertical_label = string.format("Up %.1fm", dz)
            else
                vertical_label = string.format("Down %.1fm", math.abs(dz))
            end
            draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 38, C_HINT[1], C_HINT[2], C_HINT[3], alpha_mul,
                string.format("Distance: %.1fm", d_h))
            if arrived then
                draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 20, C_NATIVE[1], C_NATIVE[2], C_NATIVE[3], alpha_mul,
                    "At waypoint")
            else
                draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 20, C_NATIVE[1], C_NATIVE[2], C_NATIVE[3], alpha_mul,
                    vertical_label)
            end
            draw_text_bold(panel_x + NAV_BOX_W - 32, panel_y + 8, C_HINT[1], C_HINT[2], C_HINT[3], alpha_mul, "*")
            imgui.dummy(NAV_BOX_W, NAV_BOX_H)
        else
            -- Keep a visible fallback in arrow-only mode so user can always restore UI.
            local mini_w, mini_h = 92, 36
            draw_soft_panel(panel_x, panel_y, panel_x + mini_w, panel_y + mini_h, 8, C_PANEL_BG, C_PANEL_BORDER, alpha_mul)
            draw_text_bold(panel_x + 10, panel_y + 10, C_HINT[1], C_HINT[2], C_HINT[3], alpha_mul, "*")
            draw_text_bold(panel_x + 24, panel_y + 10, C_HINT[1], C_HINT[2], C_HINT[3], alpha_mul, "UI")
            imgui.dummy(mini_w, mini_h)
        end

        imgui.same_line()
        if imgui.button("*") then
            arrow_only_mode = false
            save_all()
        end
        return
    end

    imgui.text("Custom waypoints v" .. PLUGIN_VERSION)
    imgui.text_colored(C_HINT[1], C_HINT[2], C_HINT[3], C_HINT[4],
        "Markers use world X/Y/Z like the built-in minimap POIs.")
    if use_native_waypoints then
        imgui.text_colored(C_NATIVE[1], C_NATIVE[2], C_NATIVE[3], C_NATIVE[4],
            "Native waypoint backend enabled (farever.waypoints.*).")
    else
        imgui.text_colored(C_LEGACY[1], C_LEGACY[2], C_LEGACY[3], C_LEGACY[4],
            "Legacy plugin-store backend enabled.")
    end

    if not farever.player.locked() then
        imgui.text_colored(C_WAIT[1], C_WAIT[2], C_WAIT[3], C_WAIT[4], "Waiting for player lock...")
        return
    end

    -- -- settings -------------------------------------------------
    local sa, c2 = imgui.checkbox("Nav arrow", show_arrow)
    if c2 then
        show_arrow = sa
        if not show_arrow then
            arrow_only_mode = false
        end
        save_all()
    end
    if show_arrow then
        imgui.same_line()
        local ao, coa = imgui.checkbox("Arrow-only mode", arrow_only_mode)
        if coa then arrow_only_mode = ao; save_all() end
    end

    imgui.separator()

    -- -- add waypoint ---------------------------------------------
    imgui.text("Add at current position")
    local nn, cn = imgui.input_text("Name", new_name)
    if cn then new_name = nn end

    if imgui.button("Add waypoint here") then
        local name = (new_name ~= "" and new_name) or ("Waypoint " .. (waypoint_count() + 1))
        local new_id_value = add_waypoint(name, px, py, pz)
        if new_id_value then
            selected_id = new_id_value
            local sel_wp = find_waypoint(selected_id)
            edit_name = (sel_wp and sel_wp.name) or name
            edit_note = ""
            new_name = "Waypoint"
            save_all()
            farever.toast("Waypoint added: " .. name, 2.0)
        else
            farever.toast("Waypoint add failed (full or unavailable)", 2.0)
        end
    end

    imgui.same_line()
    if imgui.button("Clear selection") then
        selected_id = nil
        save_all()
    end

    imgui.separator()

    -- -- arrow ----------------------------------------------------
    local sel = get_active_waypoint(px, py)
    if show_arrow and sel then
        local ax, ay = imgui.cursor_pos()
        local panel_x = ax + arrow_panel_offset_x
        local panel_y = ay + arrow_panel_offset_y
        local cx, cy = panel_x + NAV_BOX_W * 0.5, panel_y + NAV_BOX_H * 0.42
        local alpha_mul = 1.0
        draw_soft_panel(panel_x, panel_y, panel_x + NAV_BOX_W, panel_y + NAV_BOX_H, 9, C_PANEL_BG, C_PANEL_BORDER, alpha_mul)
        local d_h = math.sqrt(xy_dist_sq(px, py, sel.x, sel.y))
        local dz = sel.z - pz
        local arrived = (d_h <= ARRIVE_XY_M and math.abs(dz) <= ARRIVE_Z_M)
        if not arrived then
            d_h, dz = draw_nav_arrow(cx, cy, sel.x, sel.y, sel.z)
        end
        local vertical_label
        if math.abs(dz) < 1.0 then
            vertical_label = "Same height"
        elseif dz > 0 then
            vertical_label = string.format("Up %.1fm", dz)
        else
            vertical_label = string.format("Down %.1fm", math.abs(dz))
        end
        draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 38, C_HINT[1], C_HINT[2], C_HINT[3], alpha_mul,
            string.format("Distance: %.1fm", d_h))
        if arrived then
            draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 20, C_NATIVE[1], C_NATIVE[2], C_NATIVE[3], alpha_mul,
                "At waypoint")
        else
            draw_text_bold(panel_x + 10, panel_y + NAV_BOX_H - 20, C_NATIVE[1], C_NATIVE[2], C_NATIVE[3], alpha_mul,
                vertical_label)
        end
        imgui.dummy(NAV_BOX_W, NAV_BOX_H)
        imgui.text(string.format("Navigating: %s", sel.name))
        imgui.separator()
    end

    -- -- edit selected --------------------------------------------
    if sel then
        imgui.text("Edit selected")
        local en, ce = imgui.input_text("Rename", edit_name)
        if ce then edit_name = en end
        local nt, cn2 = imgui.input_text("Note", edit_note)
        if cn2 then edit_note = nt end
        if imgui.button("Apply name / note") then
            local desired_name = (edit_name ~= "" and edit_name) or sel.name
            if use_native_waypoints and desired_name ~= sel.name then
                -- No native rename API yet: emulate by remove+add at same position.
                local x, y, z = sel.x, sel.y, sel.z
                if remove_waypoint(sel.id) then
                    local replacement = add_waypoint(desired_name, x, y, z)
                    if replacement then
                        selected_id = replacement
                        sel = find_waypoint(selected_id)
                    end
                end
            else
                sel.name = desired_name
            end
            sel = find_waypoint(selected_id)
            if sel then
                sel.note = edit_note
                waypoint_notes[sel.id] = edit_note or ""
                save_all()
                farever.toast("Updated " .. sel.name, 1.5)
            end
        end
        imgui.same_line()
        if imgui.button("Delete selected") then
            if remove_waypoint(selected_id) then
                waypoint_notes[selected_id] = nil
                selected_id = nil
                edit_name = ""
                edit_note = ""
                save_all()
                farever.toast("Waypoint deleted", 1.5)
            else
                farever.toast("Waypoint delete failed", 1.5)
            end
        end
        imgui.separator()
    end

    -- -- list (sorted by distance) ---------------------------------
    imgui.text(string.format("All waypoints (%d)", waypoint_count()))
    if waypoint_count() == 0 then
        imgui.text_colored(C_NOTE[1], C_NOTE[2], C_NOTE[3], C_NOTE[4], "No waypoints yet — add one above.")
        return
    end

    local rows = {}
    for _, w in ipairs(waypoints) do
        local d2 = xy_dist_sq(px, py, w.x, w.y)
        table.insert(rows, {
            w = w,
            xyd = math.sqrt(d2),
            dz = w.z - pz,
        })
    end
    table.sort(rows, function(a, b) return a.xyd < b.xyd end)

    for _, row in ipairs(rows) do
        local w = row.w
        local height_label = "same height"
        if row.dz > 1.0 then
            height_label = string.format("%.0fm above", row.dz)
        elseif row.dz < -1.0 then
            height_label = string.format("%.0fm below", math.abs(row.dz))
        end
        local label = string.format("%s  %.0fm  %s", w.name, row.xyd, height_label)
        if w.id == selected_id then
            imgui.text_colored(C_SELECTED[1], C_SELECTED[2], C_SELECTED[3], C_SELECTED[4], label)
        else
            imgui.text(label)
        end
        imgui.same_line()
        if imgui.button("Go##" .. w.id) then
            selected_id = w.id
            edit_name = w.name
            edit_note = w.note or ""
            save_all()
        end
        if w.note and w.note ~= "" then
            imgui.text_colored(C_NOTE[1], C_NOTE[2], C_NOTE[3], C_NOTE[4], "    " .. w.note)
        end
    end
end
