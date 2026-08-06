-- ==============================================================
-- codex-tracker-by-kubuzer.lua
-- Submitted by @Kubuzer (https://github.com/Kubuzer)
-- Tested against farever-mod v1.2.4
-- License: MIT
--
-- Target Codex & Bestiary completion assistant with current-zone prioritization, auto-updating minimap encounter waypoints, and compact UI.
-- ==============================================================

local discovered_set = {}
local mob_levels = {}
local mob_waypoints = {}
local incomplete_cache = {}
local total_tracked_count = 0
local is_dirty = false
local last_save_time = 0
local last_cache_update = 0
local current_zone_prefix = ""

function on_init()
    if farever and farever.store then
        local saved_str = farever.store.get("discovered_mobs", "")
        for k in string.gmatch(saved_str, "([^,]+)") do
            discovered_set[k] = true
        end

        local saved_lvls = farever.store.get("mob_levels_json", "")
        for pair in string.gmatch(saved_lvls, "([^;]+)") do
            local k, lvl = string.match(pair, "([^:]+):(%d+)")
            if k and lvl then
                mob_levels[k] = tonumber(lvl)
            end
        end
    end
end

local function flush_store_if_dirty(now)
    if not is_dirty then return end
    if (now - last_save_time) < 5.0 then return end
    if not (farever and farever.store) then return end

    local list = {}
    for k, _ in pairs(discovered_set) do
        table.insert(list, k)
    end
    farever.store.set("discovered_mobs", table.concat(list, ","))

    local lvl_list = {}
    for k, lvl in pairs(mob_levels) do
        table.insert(lvl_list, k .. ":" .. tostring(lvl))
    end
    farever.store.set("mob_levels_json", table.concat(lvl_list, ";"))

    is_dirty = false
    last_save_time = now
end

local function remove_waypoint_for_mob(display_name, kind)
    if not (farever and farever.waypoints) then return end
    local target_wp_name = "Bestiary: " .. display_name

    -- Check active waypoints list
    for _, w in ipairs(farever.waypoints.list()) do
        if w.name == target_wp_name or w.name == ("Bestiary: " .. kind) then
            farever.waypoints.remove(w.id)
        end
    end

    if mob_waypoints[kind] then
        farever.waypoints.remove(mob_waypoints[kind])
        mob_waypoints[kind] = nil
    end
end

local function update_waypoint_for_mob(display_name, kind, x, y, z)
    if not (farever and farever.waypoints) then return end
    if not x or not y or (x == 0 and y == 0) then return end

    local wp_name = "Bestiary: " .. display_name

    -- Remove previous waypoint for this mob to overwrite with last-seen position
    for _, w in ipairs(farever.waypoints.list()) do
        if w.name == wp_name or w.name == ("Bestiary: " .. kind) then
            farever.waypoints.remove(w.id)
        end
    end

    -- Add updated waypoint at the last-seen coordinates
    local wp_id = farever.waypoints.add(x, y, z or 0, wp_name, { color = "orange", icon = "pin" })
    if wp_id then
        mob_waypoints[kind] = wp_id
    end
end

local function register_mob(mob_id)
    if not mob_id or mob_id == "" then return end
    if not discovered_set[mob_id] then
        discovered_set[mob_id] = true
        is_dirty = true
    end

    if farever and farever.target and farever.target.exists() then
        local lvl = farever.target.level()
        if lvl and lvl > 0 and mob_levels[mob_id] ~= lvl then
            mob_levels[mob_id] = lvl
            is_dirty = true
        end
    end
end

function on_event(name, data)
    if not data then return end
    if name == "target_changed" and data.kind then
        register_mob(data.kind)
    elseif name == "damage_dealt" and data.target then
        register_mob(data.target)
    end
end

local function clean_category_path(path, name, kind)
    if not path or path == "" then return "" end
    local clean = string.gsub(path, "/" .. name .. "$", "")
    clean = string.gsub(clean, "/" .. kind .. "$", "")
    clean = string.gsub(clean, "/", " > ")
    return clean
end

local function extract_zone_name(path)
    if not path or path == "" then return "" end
    local zone = string.match(path, "^([^/]+)")
    return zone or ""
end

local function rebuild_incomplete_cache()
    for i = #incomplete_cache, 1, -1 do
        incomplete_cache[i] = nil
    end

    total_tracked_count = 0

    for kind, _ in pairs(discovered_set) do
        total_tracked_count = total_tracked_count + 1
        local info = farever.player.codex(kind)
        if info then
            if info.completed or info.state == "complete" then
                -- Auto-remove waypoint if completed 100%
                local display_name = info.name or kind
                remove_waypoint_for_mob(display_name, kind)
            else
                local lvl = mob_levels[kind] or 0
                local entry_zone = extract_zone_name(info.path)
                table.insert(incomplete_cache, {
                    kind = kind,
                    info = info,
                    level = lvl,
                    path = info.path or "",
                    zone = entry_zone
                })
            end
        end
    end

    -- Sorting: Current Zone First -> Lowest level -> Codex Path -> Name
    table.sort(incomplete_cache, function(a, b)
        local a_in_zone = (current_zone_prefix ~= "" and a.zone == current_zone_prefix)
        local b_in_zone = (current_zone_prefix ~= "" and b.zone == current_zone_prefix)

        if a_in_zone ~= b_in_zone then
            return a_in_zone
        end
        if a.level ~= b.level then
            return a.level < b.level
        end
        if a.path ~= b.path then
            return a.path < b.path
        end
        local name_a = a.info.name or a.kind
        local name_b = b.info.name or b.kind
        return name_a < name_b
    end)
end

function on_render()
    local now = os.clock()
    flush_store_if_dirty(now)

    if is_dirty or (now - last_cache_update) > 0.5 then
        rebuild_incomplete_cache()
        last_cache_update = now
    end

    local current_target_kind = ""

    -- Live Target Section
    if farever.target.exists() then
        current_target_kind = farever.target.name()
        if current_target_kind ~= "" then
            register_mob(current_target_kind)

            local codex_info = farever.player.codex(current_target_kind)
            if codex_info then
                local display_name = codex_info.name or current_target_kind
                if display_name == "" then display_name = current_target_kind end

                local current_lvl = mob_levels[current_target_kind] or farever.target.level() or 0
                local lvl_str = (current_lvl > 0) and string.format(" [Lvl %d]", current_lvl) or ""
                
                -- Update zone prefix dynamically from target path
                local target_zone = extract_zone_name(codex_info.path)
                if target_zone ~= "" and target_zone ~= current_zone_prefix then
                    current_zone_prefix = target_zone
                    last_cache_update = 0 -- force resort
                end

                local progress = codex_info.progress or 0
                local max_val = codex_info.max or 1
                if max_val <= 0 then max_val = 1 end
                local pct = math.min(1.0, math.max(0.0, progress / max_val))

                imgui.text_colored(0.65, 0.65, 0.65, 1.0, "Target:")
                imgui.same_line()
                imgui.text_colored(1.0, 0.85, 0.25, 1.0, display_name .. lvl_str)

                if codex_info.completed or codex_info.state == "complete" then
                    imgui.same_line()
                    imgui.text_colored(0.2, 1.0, 0.2, 1.0, " - [100% COMPLETED]")
                    remove_waypoint_for_mob(display_name, current_target_kind)
                else
                    imgui.same_line()
                    if pct >= 0.85 then
                        imgui.text_colored(0.2, 1.0, 0.4, 1.0, string.format(" -  %d / %d", progress, max_val))
                    else
                        imgui.text_colored(0.7, 0.7, 0.7, 1.0, string.format(" -  %d / %d", progress, max_val))
                    end

                    -- Update/overwrite waypoint with the LAST-SEEN coordinates of the mob
                    local tx, ty, tz = farever.target.x(), farever.target.y(), farever.target.z()
                    update_waypoint_for_mob(display_name, current_target_kind, tx, ty, tz)
                end

                -- Target Health Bar
                local target_hp = farever.target.hp() or 0
                local target_max_hp = farever.target.max_hp() or 0
                if target_max_hp > 0 then
                    local hp_pct = farever.target.hp_pct() or 0.0
                    local hp_label = string.format("%.0f / %.0f HP (%.0f%%)", target_hp, target_max_hp, hp_pct * 100)
                    imgui.progress(hp_pct, hp_label)
                end

                local clean_cat = clean_category_path(codex_info.path, display_name, current_target_kind)
                if clean_cat ~= "" then
                    imgui.text_colored(0.55, 0.55, 0.55, 1.0, "Category:")
                    imgui.same_line()
                    imgui.text_colored(0.75, 0.75, 0.75, 1.0, clean_cat)
                end
            else
                imgui.text_colored(0.65, 0.65, 0.65, 1.0, "Target:")
                imgui.same_line()
                imgui.text_colored(1.0, 0.85, 0.25, 1.0, current_target_kind)
                imgui.text_colored(0.5, 0.5, 0.5, 1.0, "Codex entry: Not in Bestiary / Unknown")
            end
        end
    else
        imgui.text_colored(0.6, 0.6, 0.6, 1.0, "Select a target to view live Codex progress.")
    end

    imgui.spacing()
    imgui.separator()
    imgui.spacing()

    -- Filter out currently targeted mob from summary to avoid duplicates
    local pending_count = 0
    local filtered_list = {}
    for i = 1, #incomplete_cache do
        local entry = incomplete_cache[i]
        if entry.kind ~= current_target_kind then
            table.insert(filtered_list, entry)
        end
    end
    pending_count = #incomplete_cache

    -- Summary Section (Always capped to Top 3 non-targeted entries)
    local display_limit = 3
    local zone_badge = (current_zone_prefix ~= "") and (" [" .. current_zone_prefix .. "]") or ""
    local header_count_str = (pending_count > display_limit) and string.format(" (Top %d of %d Pending)", display_limit, pending_count) or string.format(" (%d Pending)", pending_count)
    imgui.text_colored(0.4, 0.8, 1.0, 1.0, "--- Incomplete Bestiary" .. zone_badge .. header_count_str .. " ---")
    imgui.spacing()

    if #filtered_list == 0 then
        if total_tracked_count > 0 then
            imgui.text_colored(0.2, 1.0, 0.2, 1.0, "All tracked Bestiary entries completed!")
        else
            imgui.text_colored(0.6, 0.6, 0.6, 1.0, "Explore zones and battle mobs to build your Bestiary tracker.")
        end
    else
        for i = 1, math.min(#filtered_list, display_limit) do
            local entry = filtered_list[i]
            local info = entry.info
            local name = info.name or entry.kind
            if name == "" then name = entry.kind end

            local prog = info.progress or 0
            local max_v = info.max or 1
            if max_v <= 0 then max_v = 1 end
            local pct = math.min(1.0, math.max(0.0, prog / max_v))

            local lvl_prefix = (entry.level > 0) and string.format("[Lvl %d] ", entry.level) or ""
            imgui.text_colored(1.0, 0.75, 0.3, 1.0, string.format("%s%s", lvl_prefix, name))
            
            -- High-contrast Emerald Green highlight for mobs >= 85% complete
            imgui.same_line()
            if pct >= 0.85 then
                imgui.text_colored(0.2, 1.0, 0.4, 1.0, string.format(" -  %d / %d", prog, max_v))
            else
                imgui.text_colored(0.7, 0.7, 0.7, 1.0, string.format(" -  %d / %d", prog, max_v))
            end
        end
    end
end
