-- ==============================================================
-- mob-codex-checker-by-kaaregravesen.lua
-- Submitted by @KaareGravesen  (https://github.com/ramisotti13-eng/farever-minimap/issues/44)
-- Tested against farever-mod v0.6.3
-- License: MIT
--
-- Small widget that shows the codex completion of your current target
-- as a red / yellow / green dot plus the X/Y progress, using
-- farever.player.codex(). Refreshes on the target_changed event.
-- ==============================================================

local enabled = true
local current_kind = ""
local current_codex = nil

local function dot_color(state)
    if state == "complete" then
        return 0.2, 1.0, 0.2 -- green
    elseif state == "partial" then
        return 1.0, 0.85, 0.2 -- yellow
    else
        return 1.0, 0.2, 0.2 -- red
    end
end

local function refresh_current_codex()
    current_kind = farever.target.name() or ""

    if current_kind ~= "" and farever.player.codex then
        current_codex = farever.player.codex(current_kind)
    else
        current_codex = nil
    end
end

local function render_codex_dot(state)
    imgui.text("Codex:")
    imgui.same_line()

    local x, y = imgui.cursor_pos()
    local radius = 5
    local r, g, b = dot_color(state)

    imgui.draw_circle_filled(
        x + radius,
        y + 8,
        radius,
        r, g, b,
        1.0
    )

    imgui.dummy(radius * 2 + 2, radius * 2 + 2)
end

local function render_mob_stats()
    refresh_current_codex()

    if current_codex then
        imgui.text("Current Mob: " .. current_codex.name)
        render_codex_dot(current_codex.state)
        imgui.text(string.format("%d / %d", current_codex.progress or 0, current_codex.max or 0))
    elseif current_kind ~= "" then
        imgui.text("Current Mob: " .. current_kind)
        render_codex_dot("unknown")
        imgui.text("0 / 0")
    else
        imgui.text("Current Mob: none")
        render_codex_dot("unknown")
        imgui.text("0 / 0")
    end
end

function on_init()
    enabled = farever.store.get("enabled", true)
    current_kind = ""
    current_codex = nil
    farever.log.info("loaded: mob-codex-checker")
end

function on_render()
    imgui.text("Mob Codex Checker by Ooshraxa")
    imgui.separator()

    local new_enabled, changed = imgui.checkbox("Check the Codex", enabled)
    if changed then
        enabled = new_enabled
        farever.store.set("enabled", enabled)
    end

    if not enabled then
        return
    end

    imgui.separator()
    render_mob_stats()
end

function on_event(name, data)
    if not enabled then return end

    if name == "target_changed" then
        refresh_current_codex()
    end
end
