-- target_probe: tiny probe for the new farever.target.* API (Slice 1).
-- Shows whether the API exists, whether Hero.target is locked, and the
-- internal `kind` id of whatever the player is targeting right now.
-- Each new kind value is also written to farever-mod.log so we have a
-- record of what types passed the anchor set.

local last_name      = ""
local seen           = {}
local warned_cast    = false   -- per-cast "1s warning" gate

function on_init()
    if not farever.target then
        farever.log.warn("target_probe: farever.target is nil — DLL too old?")
        return
    end
    farever.log.info("target_probe: armed")
end

function on_event(name, data)
    if name == "target_changed" then
        if data.kind == "" then
            farever.log.info("target_probe: target lost")
        else
            farever.log.info("target_probe: target -> " .. data.kind)
        end
    elseif name == "cast_start" then
        local hint = data.total_sec > 0
            and string.format(" (~%.1fs)", data.total_sec)
            or  " (learning)"
        farever.log.info("target_probe: CAST " .. data.skill .. hint)
        farever.toast("cast: " .. data.skill)
        farever.sound("alert")
        warned_cast = false
    elseif name == "cast_end" then
        farever.log.info(string.format(
            "target_probe: cast %s done in %.2fs",
            data.skill, data.duration))
    end
end

function on_render()
    imgui.text("--- target_probe ---")

    if not farever.target then
        imgui.text("farever.target = nil (rebuild DLL)")
        return
    end

    if not farever.player.locked() then
        imgui.text("hero not locked")
        return
    end

    if not farever.target.exists() then
        imgui.text("no target")
        last_name = ""
        return
    end

    local name = farever.target.name()
    imgui.text(string.format("kind:  %s", name == "" and "<empty>" or name))
    imgui.text(string.format("level: %d", farever.target.level()))
    imgui.text(string.format("pos:   %.1f, %.1f, %.1f",
                             farever.target.x(),
                             farever.target.y(),
                             farever.target.z()))
    imgui.text(string.format("hp:    %.0f / %.0f  (%.1f%%)",
                             farever.target.hp(),
                             farever.target.max_hp(),
                             farever.target.hp_pct() * 100))

    -- Distance from local hero, simple horizontal Euclidean
    if farever.player.locked() then
        local dx = farever.target.x() - farever.player.x()
        local dy = farever.target.y() - farever.player.y()
        imgui.text(string.format("dist:  %.1f m", math.sqrt(dx*dx + dy*dy)))
    end

    -- Cast bar (Slice 3) — elapsed comes from game clock, so it
    -- jumps straight to the boss's real cast age even if we noticed
    -- the cast a bit late.
    if farever.target.is_casting() then
        imgui.separator()
        local elapsed = farever.target.cast_elapsed_sec()
        local total   = farever.target.cast_total_sec()
        imgui.text(string.format("casting: %s", farever.target.cast_skill()))
        if total > 0 then
            imgui.progress(farever.target.cast_progress(),
                           string.format("%.1fs / %.1fs", elapsed, total))
            -- 1 second before the cast finishes, ping once.
            if not warned_cast and (total - elapsed) <= 1.0
                                and (total - elapsed) > 0 then
                farever.sound("warning")
                warned_cast = true
            end
        else
            imgui.progress(0,
                           string.format("%.1fs (learning duration)",
                                         elapsed))
        end
    else
        warned_cast = false
    end

    if name ~= "" and name ~= last_name then
        last_name = name
        if not seen[name] then
            seen[name] = true
            farever.log.info("target_probe: first sighting of kind=" .. name)
        end
    end
end
