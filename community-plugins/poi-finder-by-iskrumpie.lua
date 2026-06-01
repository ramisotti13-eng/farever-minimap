-- ============================================================
-- POI Finder v2.2 — by @iSkrumpie
-- Tested against: farever-mod v1.1.1
-- License: MIT
--
-- v2.0.0 (2026-05-22):
--   + Navigation arrow panel (draw_triangle + draw_line, player-
--     relative heading from farever.player.rot_z())
--   + Clickable list — click a row to lock the arrow on that POI,
--     click again to release back to AUTO (nearest)
--   + Proximity pulse — draw_circle ring pulses when < 30 m XY
--   + Out-of-range lock — arrow keeps pointing even when the
--     selected POI leaves the radius (cached world coords)
--   + Arrow panel toggle (enable/disable via checkbox)
--   + subkind label — ore/plant rows show the resource type
--   + hero_locked event clears the lock on zone transitions
--
-- v2.2.0 (2026-06-01):
--   + Sort by true 3D distance (√XY²+Δz²) instead of
--     same-level priority + XY-only — nearest POI always first
--   + Radius slider replaced with drag_float for finer control
--
-- v2.1.0 (2026-05-22):
--   + Manual "collected" tracking — [✓] button on each row
--   + Ignored/collected POIs hidden from main list
--   + "Show Collected" toggle reveals a de-ignore section
--   + Collected set persisted across sessions via farever.store
--   + Locking a POI then marking it collected clears the lock
-- ============================================================

-- ── persistent settings ───────────────────────────────────────────────────────
local radius, show_important, show_activity, show_collect, show_resource
local show_arrow
local show_ignored_section

-- ── runtime state (not persisted) ────────────────────────────────────────────
local selected_id    = nil   -- p.id of the locked POI (nil = AUTO)
local selected_cache = nil   -- {x,y,z,label} saved when POI goes out of radius
local ignored_set    = {}    -- set: { [id_str] = true } — persisted as store string

local MAX_ROWS = 12
local NAV_W    = 360   -- panel + dummy width; forces window to this content width

-- ── ignored-set helpers ───────────────────────────────────────────────────────

local function load_ignored()
    local s = farever.store.get("ignored_ids", "")
    local t = {}
    for id in s:gmatch("[^,]+") do
        t[id] = true
    end
    return t
end

local function save_ignored()
    local ids = {}
    for id in pairs(ignored_set) do
        table.insert(ids, id)
    end
    farever.store.set("ignored_ids", table.concat(ids, ","))
end

local function ignore_poi(id)
    ignored_set[tostring(id)] = true
    save_ignored()
    -- clear lock if the locked POI is now ignored
    if selected_id and tostring(selected_id) == tostring(id) then
        selected_id    = nil
        selected_cache = nil
    end
end

local function unignore_poi(id)
    ignored_set[tostring(id)] = nil
    save_ignored()
end

local function is_ignored(id)
    return ignored_set[tostring(id)] == true
end

-- ── lifecycle ─────────────────────────────────────────────────────────────────

function on_init()
    radius                = farever.store.get("radius",          150)
    show_important        = farever.store.get("show_important",   true)
    show_activity         = farever.store.get("show_activity",    true)
    show_collect          = farever.store.get("show_collect",     true)
    show_resource         = farever.store.get("show_resource",    false)
    show_arrow            = farever.store.get("show_arrow",       true)
    show_ignored_section  = farever.store.get("show_ignored",     false)
    ignored_set           = load_ignored()
    -- reset lock on every reload (zone change / hot-reload)
    selected_id    = nil
    selected_cache = nil
end

function on_event(name, data)
    if name == "hero_locked" then
        -- new zone / new session — clear the directional lock
        selected_id    = nil
        selected_cache = nil
    end
end

-- ── helpers ───────────────────────────────────────────────────────────────────

local function get_category(kind)
    if kind == "ore" or kind == "plant"           then return "resource"
    elseif kind == "chest" or kind == "red_orb"   then return "collect"
    elseif kind == "activity"                     then return "activity"
    else                                               return "important"
    end
end

-- Returns r, g, b (no alpha) — shared between text and draw_* primitives
local function dz_rgb(dz)
    local a = math.abs(dz)
    if     a <  5 then return 0.4, 1.0, 0.4    -- green  — same floor
    elseif a < 20 then return 1.0, 0.9, 0.2    -- yellow — moderate gap
    else               return 1.0, 0.4, 0.3    -- red    — large gap
    end
end

local function dz_arrow(dz)
    if     dz >  3 then return "▲"
    elseif dz < -3 then return "▼"
    else                return "●"
    end
end

local KIND_LABEL = {
    ore = "Ore",    plant    = "Plant",
    chest = "Chest", red_orb = "Red Orb",
    activity = "Activity", dungeon = "Dungeon",
    merchant = "Merchant", obelisk = "Obelisk",
    respawn = "Respawn",
}

local function kind_label(kind, subkind, name)
    -- Named activities and dungeons: show actual name if it fits
    if (kind == "activity" or kind == "dungeon")
       and name and #name > 0 and #name <= 26 then
        return name
    end
    -- Ore / plant: show resource sub-type from subkind field
    if (kind == "ore" or kind == "plant")
       and subkind and #subkind > 0 and #subkind <= 16 then
        return subkind
    end
    return KIND_LABEL[kind] or kind
end

-- ── navigation arrow panel ────────────────────────────────────────────────────

local function draw_nav_panel(target, px, py, pz, is_locked, out_of_range)
    local t          = farever.now()
    local ax, ay     = imgui.cursor_pos()
    local box_w      = NAV_W
    local box_h      = 128
    local cx, cy     = ax + box_w * 0.5, ay + box_h * 0.5

    -- Panel background + border
    imgui.draw_rect_filled(ax, ay, ax + box_w, ay + box_h,
                           0.08, 0.10, 0.13, 0.75)
    imgui.draw_rect(       ax, ay, ax + box_w, ay + box_h,
                           0.28, 0.30, 0.42, 0.85, 1.5)

    if not target then
        imgui.draw_text(ax + 14, cy - 7, 0.42, 0.42, 0.42, 1.0,
                        "No POI in range")
        imgui.dummy(box_w, box_h)
        return
    end

    -- World-delta → player-local frame (same math as nav_arrow.lua)
    local dx      = target.x - px
    local dy      = target.y - py
    local dz      = target.z - pz
    local heading = farever.player.rot_z()
    local cosh    = math.cos(heading)
    local sinh    = math.sin(heading)
    local fwd     =  dx * cosh + dy * sinh
    local rgt     = -dx * sinh + dy * cosh
    local d_h     = math.sqrt(fwd * fwd + rgt * rgt)
    local theta_h = math.atan(rgt, fwd)             -- 0 = straight ahead
    local theta_v = math.atan(dz, math.max(d_h, 1.0))

    -- Screen-space arrow direction (ImGui Y grows downward, forward = up)
    local dir_x  =  math.sin(theta_h)
    local dir_y  = -math.cos(theta_h)
    local perp_x = -dir_y
    local perp_y =  dir_x

    local r, g, b = dz_rgb(dz)

    -- ── proximity pulse (< 30 m XY) ──────────────────────────────────────
    if d_h < 30 then
        local freq  = d_h < 10 and 8.0 or 4.0   -- faster when very close
        local pulse = (math.sin(t * freq) + 1) / 2
        local pr    = 26 + pulse * 14
        imgui.draw_circle_filled(cx, cy, pr, r, g, b,
                                 0.07 + pulse * 0.11, 32)
        imgui.draw_circle(cx, cy, pr, r, g, b,
                          0.35 + pulse * 0.30, 1.0 + pulse, 32)
    end

    -- ── arrow (shaft + filled head + dark outline) ────────────────────────
    local ps     = 0.95 + 0.05 * math.sin(t * 3)   -- gentle size pulse
    local L      = 48  * ps
    local head_l = 20  * ps
    local head_w = 12  * ps
    local tilt   = math.sin(theta_v) * 15           -- tip lifts/drops with height

    local tail_x = cx - dir_x * L * 0.35
    local tail_y = cy - dir_y * L * 0.35
    local tip_x  = cx + dir_x * L
    local tip_y  = cy + dir_y * L - tilt
    local back_x = tip_x - dir_x * head_l
    local back_y = tip_y - dir_y * head_l
    local hl_x   = back_x + perp_x * head_w
    local hl_y   = back_y + perp_y * head_w
    local hr_x   = back_x - perp_x * head_w
    local hr_y   = back_y - perp_y * head_w

    imgui.draw_line(tail_x, tail_y, back_x, back_y, r, g, b, 1.0, 5.0)
    imgui.draw_triangle_filled(tip_x, tip_y, hl_x, hl_y, hr_x, hr_y,
                               r, g, b, 1.0)
    imgui.draw_triangle(       tip_x, tip_y, hl_x, hl_y, hr_x, hr_y,
                               0, 0, 0, 0.60, 1.5)

    -- ── vertical tick (height delta indicator on the right side) ─────────
    if math.abs(dz) > 3 then
        local tx2    = ax + box_w - 22
        local ttop   = cy - 36
        local tbot   = cy + 36
        local mark_y = cy - math.max(-36, math.min(36, dz * 0.6))
        imgui.draw_line(tx2, ttop, tx2, tbot, 0.50, 0.50, 0.50, 0.55, 1.0)
        imgui.draw_triangle_filled(
            tx2 - 7, mark_y - 4,
            tx2 - 7, mark_y + 4,
            tx2,     mark_y,
            r, g, b, 1.0)
    end

    -- ── overlay text inside the panel ─────────────────────────────────────
    -- POI name (top-left)
    local name_str = target.label:sub(1, 24)
    if out_of_range then name_str = name_str .. " (!)" end
    imgui.draw_text(ax + 6, ay + 5, 0.88, 0.88, 0.88, 0.95, name_str)

    -- Distance + Δz (bottom-left)
    imgui.draw_text(ax + 6, ay + box_h - 19, r, g, b, 1.0,
        string.format("%.0f m   %+.1f m", d_h, dz))

    -- Mode tag AUTO / LOCKED (top-right)
    local mode_str = is_locked and "LOCKED" or "AUTO"
    local mr = is_locked and 1.0 or 0.30
    local mg = is_locked and 0.65 or 0.85
    local mb = is_locked and 0.15 or 0.35
    imgui.draw_text(ax + box_w - 56, ay + 5, mr, mg, mb, 1.0, mode_str)

    imgui.dummy(box_w, box_h)
end

-- ── main render ───────────────────────────────────────────────────────────────

function on_render()
    imgui.separator()

    -- force window content width = NAV_W so the panel fills the full frame
    imgui.dummy(NAV_W, 0)

    if not farever.player.locked() then
        imgui.text_colored(1.0, 0.6, 0.2, 1.0, "Waiting for player lock...")
        return
    end

    local px = farever.player.x()
    local py = farever.player.y()
    local pz = farever.player.z()

    -- ── settings controls ─────────────────────────────────────────────────
    local nv, ch
    nv, ch = imgui.drag_float("Radius (m)", radius, 1.0, 20, 500)
    if ch then radius = nv; farever.store.set("radius", radius) end

    imgui.spacing()

    local ni, c1 = imgui.checkbox("Important", show_important)
    if c1 then show_important = ni; farever.store.set("show_important", ni) end
    imgui.same_line()
    local na, c2 = imgui.checkbox("Activities", show_activity)
    if c2 then show_activity = na; farever.store.set("show_activity", na) end
    imgui.same_line()
    local nc, c3 = imgui.checkbox("Collectibles", show_collect)
    if c3 then show_collect = nc; farever.store.set("show_collect", nc) end
    imgui.same_line()
    local nr, c4 = imgui.checkbox("Resources", show_resource)
    if c4 then show_resource = nr; farever.store.set("show_resource", nr) end

    local sw, c5 = imgui.checkbox("Navigation Arrow", show_arrow)
    if c5 then show_arrow = sw; farever.store.set("show_arrow", sw) end

    imgui.separator()

    -- ── load and filter POIs ──────────────────────────────────────────────
    local pois = farever.pois and farever.pois() or {}
    if #pois == 0 then
        imgui.text_colored(0.6, 0.6, 0.6, 1.0, "POI data not available.")
        imgui.text_colored(0.5, 0.5, 0.5, 1.0, "(requires farever-mod v0.5.6.1+)")
        return
    end

    local r2           = radius * radius
    local nearby       = {}   -- visible, not ignored
    local nearby_ign   = {}   -- within radius but ignored (for "Show Collected")

    for _, p in ipairs(pois) do
        local cat  = get_category(p.kind)
        local show = (cat == "important" and show_important)
                  or (cat == "activity"  and show_activity)
                  or (cat == "collect"   and show_collect)
                  or (cat == "resource"  and show_resource)
        if show then
            local ddx = px - p.x
            local ddy = py - p.y
            local d2  = ddx * ddx + ddy * ddy
            if d2 <= r2 then
                local xyd = math.sqrt(d2)
                local dz  = p.z - pz
                local d3  = math.sqrt(d2 + dz * dz)
                local entry = {
                    id    = p.id,
                    x     = p.x,
                    y     = p.y,
                    z     = p.z,
                    dz    = dz,
                    xyd   = xyd,
                    d3    = d3,
                    label = kind_label(p.kind, p.subkind, p.name),
                    kind  = p.kind,
                }
                if is_ignored(p.id) then
                    table.insert(nearby_ign, entry)
                else
                    table.insert(nearby, entry)
                end
            end
        end
    end

    -- sort: nearest 3D distance first (sqrt(dx²+dy²+dz²))
    local function sort_fn(a, b)
        return a.d3 < b.d3
    end
    table.sort(nearby,     sort_fn)
    table.sort(nearby_ign, sort_fn)

    -- ── determine arrow target ────────────────────────────────────────────
    local arrow_target = nil
    local is_locked    = false
    local out_of_range = false

    if selected_id then
        for _, p in ipairs(nearby) do
            if p.id == selected_id then
                arrow_target = p
                is_locked    = true
                selected_cache = { x=p.x, y=p.y, z=p.z, label=p.label }
                break
            end
        end

        if not arrow_target and selected_cache then
            local cdx = selected_cache.x - px
            local cdy = selected_cache.y - py
            local cdz = selected_cache.z - pz
            arrow_target = {
                id    = selected_id,
                x     = selected_cache.x,
                y     = selected_cache.y,
                z     = selected_cache.z,
                dz    = cdz,
                xyd   = math.sqrt(cdx * cdx + cdy * cdy),
                label = selected_cache.label,
            }
            is_locked    = true
            out_of_range = true
        end
    end

    -- AUTO: nearest POI in the visible (non-ignored) list
    if not arrow_target and #nearby > 0 then
        arrow_target = nearby[1]
    end

    -- ── nav arrow panel ───────────────────────────────────────────────────
    if show_arrow then
        draw_nav_panel(arrow_target, px, py, pz, is_locked, out_of_range)
    end

    -- ── POI list ──────────────────────────────────────────────────────────
    if #nearby == 0 and #nearby_ign == 0 then
        imgui.text_colored(0.6, 0.6, 0.6, 1.0, "No POIs within radius.")
        imgui.text_colored(0.5, 0.5, 0.5, 1.0,
            string.format("(%d POIs loaded)", #pois))
        imgui.separator()
        if selected_id then
            if imgui.button("Clear lock") then
                selected_id = nil; selected_cache = nil
            end
            imgui.same_line()
        end
        imgui.text_colored(0.5, 0.5, 0.5, 1.0,
            string.format("[%s]  Z: %.1f", is_locked and "LOCKED" or "AUTO", pz))
        return
    end

    -- column header
    imgui.text(string.format("    %-22s  %6s  %6s  ",
        "POI", "XY (m)", "Δz (m)"))
    imgui.separator()

    local shown = 0
    for i, p in ipairs(nearby) do
        if shown >= MAX_ROWS then
            imgui.text_colored(0.58, 0.58, 0.58, 1.0,
                string.format("  ... %d more", #nearby - shown))
            break
        end

        local is_sel = (p.id == selected_id)

        -- Row select button
        if imgui.button(tostring(i) .. ".") then
            if is_sel then
                selected_id    = nil
                selected_cache = nil
            else
                selected_id    = p.id
                selected_cache = { x=p.x, y=p.y, z=p.z, label=p.label }
            end
        end
        imgui.same_line()

        -- Row text
        local prefix  = is_sel and "►" or dz_arrow(p.dz)
        local cr, cg, cb = dz_rgb(p.dz)
        local ca = is_sel and 1.0 or 0.92

        local line = string.format("%s %-22s %5.0fm %+6.1fm",
            prefix, p.label:sub(1, 22), p.xyd, p.dz)
        imgui.text_colored(cr, cg, cb, ca, line)
        imgui.same_line()

        -- [v] ignore button (ASCII — font has no checkmark glyph)
        if imgui.button("collected##ign" .. i) then
            ignore_poi(p.id)
        end

        shown = shown + 1
    end

    -- ── collected (ignored) section ───────────────────────────────────────
    local ign_count = #nearby_ign

    imgui.separator()

    -- Toggle line: show count even when section is hidden
    local ign_label
    if ign_count == 0 then
        ign_label = "Collected (none in range)"
    elseif ign_count == 1 then
        ign_label = "Collected (1 hidden)"
    else
        ign_label = string.format("Collected (%d hidden)", ign_count)
    end

    local si, c6 = imgui.checkbox(ign_label, show_ignored_section)
    if c6 then
        show_ignored_section = si
        farever.store.set("show_ignored", si)
    end

    if show_ignored_section and ign_count > 0 then
        imgui.separator()

        -- "Uncheck All" shortcut
        if imgui.button("< Uncheck All") then
            for _, p in ipairs(nearby_ign) do
                unignore_poi(p.id)
            end
        end
        imgui.separator()

        for i, p in ipairs(nearby_ign) do
            if i > MAX_ROWS then
                imgui.text_colored(0.45, 0.45, 0.45, 1.0,
                    string.format("  ... %d more collected", ign_count - MAX_ROWS))
                break
            end

            -- [↩] de-ignore button
            if imgui.button("x##unign" .. i) then
                unignore_poi(p.id)
            end
            imgui.same_line()

            -- Dimmed row (grayed out)
            local line = string.format("  %-22s %5.0fm %+6.1fm",
                p.label:sub(1, 22), p.xyd, p.dz)
            imgui.text_colored(0.42, 0.42, 0.42, 1.0, line)
        end
    end

    -- ── footer ────────────────────────────────────────────────────────────
    imgui.separator()
    if selected_id then
        if imgui.button("Clear lock") then
            selected_id = nil; selected_cache = nil
        end
        imgui.same_line()
    end
    imgui.text_colored(0.48, 0.48, 0.48, 1.0, string.format(
        "[%s]  %d / %d   Z: %.1f",
        is_locked and "LOCKED" or "AUTO",
        shown, #nearby, pz))
end
