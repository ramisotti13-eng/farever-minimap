-- ============================================================
-- POI Finder — by @iSkrumpie
-- Submitted: community-plugins / issues#36
-- Updated:   2026-05-21 — v0.5.6.1 (farever.pois() API)
-- Tested against: farever-mod v0.5.6.1
-- License: MIT
--
-- Lists nearby POIs within a configurable radius.
-- Each row: direction arrow, label, XY distance, Δz.
-- Category filters and radius configurable in-window.
--
-- v0.5.6.1: replaced hardcoded POI table with farever.pois().
--           POI data stays current with the mod's own json file.
--           Removed ~800 lines of baked-in coordinates.
-- ============================================================

-- ── settings (persisted) ──────────────────────────────────────────────────────
local radius
local show_important
local show_activity
local show_collect
local show_resource

function on_init()
    radius         = farever.store.get("radius",        150)
    show_important = farever.store.get("show_important", true)
    show_activity  = farever.store.get("show_activity",  true)
    show_collect   = farever.store.get("show_collect",   true)
    show_resource  = farever.store.get("show_resource",  false)
end

-- ── helpers ───────────────────────────────────────────────────────────────────

-- Map POI kind → category bucket
local function get_category(kind)
    if kind == "ore" or kind == "plant" then
        return "resource"
    elseif kind == "chest" or kind == "red_orb" then
        return "collect"
    elseif kind == "activity" then
        return "activity"
    else
        -- dungeon, merchant, obelisk, respawn, ...
        return "important"
    end
end

local function xy_dist_sq(x1, y1, x2, y2)
    local dx = x1 - x2
    local dy = y1 - y2
    return dx*dx + dy*dy
end

local function dz_arrow(dz)
    if     dz >  3.0 then return "▲"
    elseif dz < -3.0 then return "▼"
    else                   return "●"
    end
end

-- green = level, yellow = moderate gap, red = large gap
local function dz_color(dz)
    local a = math.abs(dz)
    if     a <  5 then return 0.4, 1.0, 0.4, 1.0   -- green
    elseif a < 20 then return 1.0, 0.9, 0.2, 1.0   -- yellow
    else               return 1.0, 0.4, 0.3, 1.0   -- red
    end
end

-- Kind → short display label
local KIND_LABEL = {
    ore      = "Ore",
    plant    = "Plant",
    chest    = "Chest",
    red_orb  = "Red Orb",
    activity = "Activity",
    dungeon  = "Dungeon",
    merchant = "Merchant",
    obelisk  = "Obelisk",
    respawn  = "Respawn",
}
local function kind_label(kind, name)
    -- For named activities / dungeons, show their actual name if short enough
    if (kind == "activity" or kind == "dungeon") and name and #name > 0 and #name <= 28 then
        return name
    end
    return KIND_LABEL[kind] or kind
end

-- ── render ────────────────────────────────────────────────────────────────────
local MAX_ROWS = 12

function on_render()
    imgui.separator()

    if not farever.player.locked() then
        imgui.text_colored(1.0, 0.6, 0.2, 1.0, "Waiting for player lock...")
        return
    end

    local px = farever.player.x()
    local py = farever.player.y()
    local pz = farever.player.z()

    -- Radius slider
    local new_radius, changed = imgui.slider_float("Radius (m)", radius, 20, 500)
    if changed then
        radius = new_radius
        farever.store.set("radius", radius)
    end

    imgui.spacing()

    -- Category toggles
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

    imgui.separator()

    -- Load POIs from mod (v0.5.6.1+), fall back gracefully if not available
    local pois = farever.pois and farever.pois() or {}

    if #pois == 0 then
        imgui.text_colored(0.6, 0.6, 0.6, 1.0, "POI data not available.")
        imgui.text_colored(0.5, 0.5, 0.5, 1.0, "(requires farever-mod v0.5.6.1+)")
        return
    end

    -- Gather nearby POIs
    local r2 = radius * radius
    local nearby = {}

    for _, p in ipairs(pois) do
        local cat = get_category(p.kind)
        local show = (cat == "important" and show_important)
                  or (cat == "activity"  and show_activity)
                  or (cat == "collect"   and show_collect)
                  or (cat == "resource"  and show_resource)

        if show then
            local d2 = xy_dist_sq(px, py, p.x, p.y)
            if d2 <= r2 then
                local dz  = p.z - pz
                local xyd = math.sqrt(d2)
                local lbl = kind_label(p.kind, p.name)
                table.insert(nearby, { dz = dz, xyd = xyd, label = lbl, kind = p.kind })
            end
        end
    end

    if #nearby == 0 then
        imgui.text_colored(0.6, 0.6, 0.6, 1.0, "No POIs within radius.")
        imgui.text_colored(0.5, 0.5, 0.5, 1.0,
            string.format("(%d total POIs loaded)", #pois))
        return
    end

    -- Sort: level-first (|dz| < 3), then by XY distance
    table.sort(nearby, function(a, b)
        local al = math.abs(a.dz) < 3
        local bl = math.abs(b.dz) < 3
        if al ~= bl then return al end
        return a.xyd < b.xyd
    end)

    -- Column header
    imgui.text(string.format(" %-26s %6s %8s", "POI", "XY (m)", "Δz (m)"))
    imgui.separator()

    local shown = 0
    for _, p in ipairs(nearby) do
        if shown >= MAX_ROWS then
            imgui.text_colored(0.6, 0.6, 0.6, 1.0,
                string.format("  ... %d more", #nearby - shown))
            break
        end
        local arrow     = dz_arrow(p.dz)
        local r, g, b, a = dz_color(p.dz)
        local line = string.format("%s %-26s %5.0fm %+7.1fm",
            arrow, p.label:sub(1, 26), p.xyd, p.dz)
        imgui.text_colored(r, g, b, a, line)
        shown = shown + 1
    end

    imgui.separator()
    imgui.text_colored(0.5, 0.5, 0.5, 1.0,
        string.format("Showing %d / %d   |   Z: %.1f", shown, #nearby, pz))
end
