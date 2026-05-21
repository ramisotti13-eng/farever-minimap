-- ==============================================================
-- animation_demo.lua
--
-- Showcases the v0.5.6 animation surface. Each section demonstrates
-- one pattern plugin authors can copy into their own plugins.
--
-- API used:
--   farever.now()                  monotonic seconds (double)
--   imgui.font_scale(s)            scale text in current window
--   imgui.cursor_pos()             absolute screen anchor
--   imgui.dummy(w, h)              reserve flow space without drawing
--   imgui.draw_rect_filled(x1,y1,x2,y2, r,g,b,a)
--   imgui.draw_rect(x1,y1,x2,y2, r,g,b,a, thickness)
--   imgui.draw_circle_filled(x,y,radius, r,g,b,a [, segments])
--   imgui.draw_circle(x,y,radius, r,g,b,a [, thickness, segments])
--   imgui.draw_line(x1,y1,x2,y2, r,g,b,a, thickness)
--   imgui.draw_text(x,y, r,g,b,a, "text")
-- ==============================================================

function on_init()
    farever.log.info("animation_demo: ready")
end

function on_render()
    imgui.text("--- animation demo ---")
    local t = farever.now()

    -- 1) Blinking alpha. Pure text_colored with sin-driven alpha.
    do
        local pulse = (math.sin(t * 6) + 1) / 2     -- 0..1, ~1Hz
        imgui.text_colored(1.0, 0.2, 0.2, pulse, "1) BLINKING ALERT")
    end

    -- 2) Growing/shrinking text. font_scale per-frame, reset to 1.0
    --    immediately after so following widgets stay normal.
    do
        local scale = 1.2 + 0.5 * math.sin(t * 3)   -- 0.7..1.7
        imgui.font_scale(scale)
        imgui.text("2) pulsing size")
        imgui.font_scale(1.0)
    end

    -- 3) RGB cycle. Each channel offset by 2pi/3.
    do
        local r = (math.sin(t * 2) + 1) / 2
        local g = (math.sin(t * 2 + 2.094) + 1) / 2
        local b = (math.sin(t * 2 + 4.189) + 1) / 2
        imgui.text_colored(r, g, b, 1.0, "3) rainbow")
    end

    imgui.separator()

    -- 4) Custom cast bar drawn with draw_rect_filled. Anchor at the
    --    current cursor, then reserve the height with dummy() so the
    --    next widget flows below it.
    do
        imgui.text("4) custom cast bar:")
        local x, y = imgui.cursor_pos()
        local w, h = 240.0, 18.0
        -- progress goes 0..1 over a 2-second period
        local progress = (t * 0.5) % 1.0

        -- background
        imgui.draw_rect_filled(x, y, x + w, y + h, 0.1, 0.1, 0.15, 0.8)
        -- fill (turns red on the last 25%)
        local r, g, b = 0.3, 0.7, 1.0
        if progress > 0.75 then
            local alarm_pulse = (math.sin(t * 12) + 1) / 2
            r, g, b = 1.0, 0.3 * alarm_pulse, 0.3 * alarm_pulse
        end
        imgui.draw_rect_filled(x, y, x + w * progress, y + h, r, g, b, 1.0)
        -- border
        imgui.draw_rect(x, y, x + w, y + h, 0.7, 0.7, 0.8, 1.0, 1.5)
        -- overlay text centered
        imgui.draw_text(x + 8, y + 2, 1, 1, 1, 1,
                        string.format("Cast %.0f%%", progress * 100.0))

        imgui.dummy(w, h)   -- reserve the space we just painted into
    end

    -- 5) Telegraph circle. Two concentric circles, the outer one
    --    "fills in" as a count-down ring. Background-thread plugin
    --    authors can use this same shape to mark world positions.
    do
        imgui.text("5) telegraph circle:")
        local x, y = imgui.cursor_pos()
        local cx, cy, r = x + 60, y + 60, 40
        local fill = (t * 0.5) % 1.0

        -- inner filled disc (transparent red)
        imgui.draw_circle_filled(cx, cy, r, 1, 0, 0, 0.20, 32)
        -- progress ring on the outside
        local thickness = 5.0
        local outer_r = r + 5
        if fill > 0.85 then
            -- final flash before "impact"
            local f = (math.sin(t * 20) + 1) / 2
            imgui.draw_circle(cx, cy, outer_r, 1, f * 0.5, f * 0.5, 1, 4, 64)
        else
            imgui.draw_circle(cx, cy, outer_r * fill, 1, 1, 1, 1,
                              thickness, 64)
        end

        imgui.dummy(140, 130)
    end

    -- 6) Absolute-position text. draw_text places at exact screen
    --    coords, ignoring the window's flow. Pair with cursor_pos
    --    for relative positioning.
    do
        local x, y = imgui.cursor_pos()
        local fade = (math.sin(t * 2) + 1) / 2
        imgui.draw_text(x, y, 1, 1, 0, fade,
                        "6) draw_text at absolute coords")
        imgui.dummy(280, 18)
    end

    -- 7) Big alarm pattern. The thing a boss-mechanic plugin would
    --    fire 1s before an impact: large red flashing text + sound
    --    (pretend the trigger is "cast remaining < 1s"). Here we
    --    pulse it once every 4 seconds as a demo.
    do
        local cycle = t % 4.0
        if cycle < 0.8 then
            local pulse = (math.sin(t * 14) + 1) / 2
            imgui.font_scale(2.0 + pulse * 0.6)
            imgui.text_colored(1.0, 0.2, 0.2, 0.6 + pulse * 0.4,
                               "7) DODGE!")
            imgui.font_scale(1.0)
        else
            imgui.text("7) (alarm pulses every 4s)")
        end
    end
end
