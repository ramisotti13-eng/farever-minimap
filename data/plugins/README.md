# Writing plugins for farever-mod

This is the folder where plugins live. Drop a `.lua` file in here and
the mod loads it automatically the next time it sees the file
(actually it picks up changes about once a second, so if you have the
game running you do not need to restart).

A plugin is one `.lua` file. The mod does not ship any by default,
the folder starts empty. Two example plugins live in the repo at
[`examples/plugins/`](https://github.com/ramisotti13-eng/farever-minimap/tree/main/examples/plugins).
Download either one, drop it in here, you have a working starting
point.

If your plugin breaks, the mod does not. The Lua state is sandboxed
and every call into it goes through `pcall`, so an error in your
script lands in `farever-mod.log` and in the in-game Plugin Manager
window. Your script keeps running, the rest of the mod keeps running.

To see the Plugin Manager, click the Filter button on the minimap
(the funnel-shaped one), then check "Show plugin manager". You will
see each loaded plugin, any errors, and a reload button.

## The smallest possible plugin

```lua
function on_render()
    imgui.text("Hello from Lua")
end
```

Save that as `data/plugins/hello.lua`. Within a second a window
labelled "hello" appears in the game with that text. Done.

## Lifecycle

You can define up to three global functions. None of them are
required.

```lua
function on_init()
    -- Called once when the plugin loads, and again on every hot
    -- reload (when you save your .lua file). Use it to load state
    -- from farever.store or set up any locals you need.
end

function on_render()
    -- Called every frame, inside an ImGui window the mod opens for
    -- you. The window title is your plugin's filename without
    -- the .lua extension. You only put widgets in here, never call
    -- imgui.begin or imgui.end yourself.
end

function on_event(name, data)
    -- Called when something happens in the game. Names: "hero_locked",
    -- "fight_start", "damage_dealt", "fight_end". See the section
    -- below for what data each one carries.
end
```

## What you can read

```lua
-- Position and orientation
farever.player.x()                          -- world X
farever.player.y()                          -- world Y
farever.player.z()                          -- world Z
farever.player.rot_z()                      -- heading in radians
farever.player.locked()                     -- true once the mod has identified you

-- Combat state
farever.player.in_combat()                  -- the game's in-combat flag
farever.player.combat_start()               -- game-time when combat began
farever.player.has_target()                 -- true if Hero.target is non-zero

-- Progression
farever.player.level()                      -- character level

-- Health and energy
farever.player.health()                     -- current HP
farever.player.max_health()                 -- max HP
farever.player.health_pct()                 -- health / max_health, 0.0 .. 1.0
farever.player.health_regen()               -- HP regen rate
farever.player.shield()                     -- shield value
farever.player.energy()                     -- special energy pool
farever.player.energy_regen()               -- special energy regen rate

-- Primary stats
farever.player.vitality()
farever.player.strength()
farever.player.dexterity()
farever.player.faith()
farever.player.intellect()

-- Combat numbers (post-rating, ready-to-display values)
farever.player.crit_chance()                -- 0.0 .. 1.0
farever.player.crit_damage()                -- multiplier
farever.player.armor_penetration()
farever.player.spell_penetration()
farever.player.fervor()
farever.player.block_mitigation()
farever.player.dodge_chance()               -- 0.0 .. 1.0
farever.player.magic_mastery()
farever.player.physical_mastery()
farever.player.spell_cast_time_reduction()
farever.player.knock_resistance()
farever.player.cooldown_reduction()

-- Defense
farever.player.armor()                      -- physical armor
farever.player.magic_armor()                -- magic armor
farever.player.magic_reduction()

-- Generic modifiers
farever.player.move_speed_factor()
farever.player.damage()                     -- base damage modifier
farever.player.heal()                       -- base heal output

-- Hero-only class resources (zero if the runtime type is not Hero)
farever.player.poise()
farever.player.poise_regen()
farever.player.oxygen()
farever.player.rage()
farever.player.rage_regen()
farever.player.spark()
farever.player.spark_regen()
farever.player.combo_point()
farever.player.focus()
farever.player.damage_modifier()            -- post-buff outgoing damage scale
farever.player.damage_taken_modifier()      -- incoming damage scale
farever.player.heal_given_multiplier()
farever.player.shield_power_multiplier()
farever.player.glide_speed()

-- DPS meter snapshot
farever.dps.current()                       -- current pull's DPS (float)
farever.dps.total()                         -- current pull's total damage
farever.dps.elapsed()                       -- seconds since the pull started
farever.dps.in_combat()                     -- true while the pull is still active

-- Current target (the foe or hero your character is engaging right now;
-- driven by Hero.lockedTarget / autoTarget / target in that priority).
-- All these return 0 / "" / false when nothing is targeted.
farever.target.exists()                     -- true if a target is locked
farever.target.name()                       -- internal kind id ("Boar_Z1W_E", ...)
farever.target.x()                          -- world X
farever.target.y()                          -- world Y
farever.target.z()                          -- world Z
farever.target.level()                      -- target's level
farever.target.hp()                         -- current HP
farever.target.max_hp()                     -- max HP
farever.target.hp_pct()                     -- hp / max_hp, 0.0 .. 1.0

-- Cast bar. is_casting is true while the target is in a non-auto skill
-- (auto-attacks are filtered out). cast_total_sec is 0 the first time
-- you see a given skill; the mod learns the duration from that cast
-- and serves it back on every subsequent cast of the same skill.
farever.target.is_casting()                 -- true while a real cast runs
farever.target.cast_skill()                 -- skill id ("Boar_Skill1")
farever.target.cast_elapsed_sec()           -- seconds since the cast started
farever.target.cast_total_sec()             -- cached duration, 0 if unknown yet
farever.target.cast_remaining_sec()         -- total - elapsed, 0 if unknown
farever.target.cast_progress()              -- elapsed / total, 0.0 .. 1.0

-- Target defense (v0.5.5+). Reads return 0 for many builds because
-- the inline UnitAttributes fields hold only the base values; the
-- real final stats live in a MapData side-channel that the mod
-- doesn't decode yet. Useful as a placeholder slot for plugins that
-- want to switch over once the deeper read lands.
farever.target.armor()
farever.target.magic_armor()
farever.target.magic_reduction()

-- Equipped weapon (v0.5.6+). Hero.weaponInHand chase. The kind is
-- the internal id ("Staff_Craft_C"); level / upgrade are integers.
-- All three are empty / 0 mid-swap.
farever.player.weapon_kind()                -- "Staff_Craft_C"
farever.player.weapon_level()
farever.player.weapon_upgrade()

-- Full loadout (v0.5.6+). Walks Hero.loadout.equipment.content[].
-- Each entry is a Lua table { kind, level, upgrade }. Order matches
-- the game's content array; the mod refreshes the snapshot at ~1 Hz.
local items = farever.player.equipment()
for i, it in ipairs(items) do
    print(i, it.kind, it.level, it.upgrade)
end

-- Active statuses / buffs (v0.5.6+). Walks Unit.instigatedStatuses.
-- Each entry is { kind, duration, stacks }. Plugins compute remaining
-- time client-side from farever.now() if they want a countdown.
local buffs = farever.player.statuses()
for i, s in ipairs(buffs) do
    print(i, s.kind, s.duration, s.stacks)
end
```

All of these are functions you call. They return the value at the
moment you ask. They never block. If the mod has not identified your
character yet (`locked()` returns false) the resource and defense
readers return 0 so plugin code can use them unconditionally.

> **Foes API note.** v0.5.3.1 shipped a `farever.foes.*` table for
> tracking *every* mob in range. It was the source of a crash a few
> seconds after the hero locks, so v0.5.3.2 pulled it out. A full
> multi-foe tracker is still on the roadmap. The replacement that
> ships now is `farever.target.*` (above): it tracks only your
> currently-targeted foe, which covers the vast majority of
> boss-helper / cast-warning use cases at a fraction of the read
> surface and zero observed crashes.

## Events

When you define `on_event`, the mod calls it once per event with a
name and a Lua table.

```lua
function on_event(name, data)
    if name == "hero_locked" then
        -- data is empty. Fired when the mod identifies your
        -- character (initial lock and on zone transitions).

    elseif name == "fight_start" then
        -- data.fight_id   (integer, monotonic)

    elseif name == "damage_dealt" then
        -- data.skill    (string, e.g. "Mage_RayOfSpark")
        -- data.amount   (number)
        -- data.is_crit  (boolean)
        -- data.is_kill  (boolean)

    elseif name == "fight_end" then
        -- data.fight_id     (integer)
        -- data.duration     (seconds, float)
        -- data.total_damage (float)
        -- data.dps          (float)
        -- data.top_skill    (string, highest-total skill of the fight)

    elseif name == "target_changed" then
        -- Fires whenever the player's auto / locked target switches.
        -- data.kind is the new target's internal id string ("Boar_Z1W_E",
        -- "Skunk_Z1W", ...). Empty string when the target is cleared.

    elseif name == "cast_start" then
        -- A boss / mob your hero is targeting has started a (non-auto)
        -- skill. Use this to play a warning sound or pop a toast.
        -- data.skill     (string, internal id like "Boar_Skill1")
        -- data.total_sec (float, learned duration of this skill from
        --                 a previous observation; 0.0 the very first
        --                 time we see this skill)

    elseif name == "cast_end" then
        -- The cast finished (runningCtx cleared). The duration the mod
        -- measured is also fed back into the duration cache so future
        -- cast_start events for the same skill carry that value in
        -- data.total_sec.
        -- data.skill    (string)
        -- data.duration (float, seconds)

    elseif name == "weapon_changed" then
        -- (v0.5.6+) Hero.weaponInHand transitioned to a new kind.
        -- Also fires on the initial observation after hero lock with
        -- prev_kind == "".
        -- data.kind      (string, new weapon's internal id)
        -- data.prev_kind (string, previous kind or "")
        -- data.level     (int)
        -- data.upgrade   (int)
    end
end
```

The events run on the render thread, same as `on_render`. They are
synchronous and ordered. You do not need to worry about locks or
threading inside Lua.

## UI

Inside `on_render`, the mod has already opened a window for you.
Anything you draw goes into that window.

```lua
imgui.text("plain text")
imgui.text_colored(1.0, 0.6, 0.2, 1.0, "orange text")

if imgui.button("click me") then
    -- code runs when the user clicks
end

-- Stateful widgets return (new_value, changed). You hand back the
-- old value, the widget hands you back the new one plus a flag
-- that tells you whether the user touched it this frame.
local enabled = imgui.checkbox("Active", enabled)
local volume,  c = imgui.slider_float("Volume", volume, 0, 100)
local speed,   c = imgui.drag_float("Speed", speed, 0.1, 0, 10)
local name,    c = imgui.input_text("Name", name)
local r,g,b,   c = imgui.color_edit("Color", r, g, b)

local items = { "Apple", "Banana", "Cherry" }
local idx, c = imgui.combo("Fruit", idx, items)

imgui.progress(0.7, "70 percent")

imgui.separator()
imgui.spacing()
imgui.same_line()
```

If you do not store the returned values back into your locals, the
widget will not remember what the user typed. This catches a lot of
beginners.

### Animation surface (v0.5.6+)

For boss-mechanic alerts, telegraphs and other custom HUD elements,
the mod also exposes raw drawing primitives plus a time source. All
coordinates are absolute screen-space; pair `cursor_pos()` with
`dummy(w, h)` to anchor draws relative to the flowing layout and to
reserve the height so subsequent widgets do not overlap.

```lua
farever.now()                                      -- seconds (double, monotonic)
imgui.font_scale(2.0)                              -- scale text in current window
imgui.font_scale(1.0)                              -- reset before next widget
local x, y = imgui.cursor_pos()                    -- absolute screen anchor
imgui.dummy(width, height)                         -- reserve flow space

-- All draw_* take r, g, b, a in 0..1; the last argument of stroke
-- variants is the line thickness in pixels.
imgui.draw_rect_filled(x1, y1, x2, y2, r, g, b, a)
imgui.draw_rect(x1, y1, x2, y2, r, g, b, a, thickness)
imgui.draw_circle_filled(x, y, radius, r, g, b, a)
imgui.draw_circle(x, y, radius, r, g, b, a, thickness)
imgui.draw_line(x1, y1, x2, y2, r, g, b, a, thickness)
imgui.draw_text(x, y, r, g, b, a, "text at this exact screen pos")
```

A worked example with cast-bar, telegraph circle, blinking alert and
pulsing font size is in [`examples/plugins/animation_demo.lua`](../../examples/plugins/animation_demo.lua).

## Persistent state

If you want something to survive a restart, put it in the store.

```lua
function on_init()
    counter = farever.store.get("counter", 0)
end

function on_render()
    imgui.text("Counter: " .. counter)
    if imgui.button("+1") then
        counter = counter + 1
        farever.store.set("counter", counter)
    end
end
```

The store is private to your plugin. It lives in
`data/plugins/<your_plugin>.store.lua`. Values can be strings,
numbers, booleans, or nil. Nothing fancier in v1.1, so if you need
a list, encode it with `table.concat` or `string.format` and parse
it back on read.

`set` writes the file immediately, so you do not lose state when
the game crashes.

## Toasts

```lua
farever.toast("Got it!")
farever.toast("Big news", 5.0)   -- duration in seconds, default 2
```

Centered at the top of the screen, stacks if you call it multiple
times in quick succession, fades out near the end. Good for "new
record" or "warning" messages where opening a whole window would be
overkill.

## Sounds

```lua
farever.sound("alert")    -- sharp ping (SystemAsterisk)
farever.sound("warning")  -- lower ping (SystemExclamation)
farever.sound("info")     -- soft notification ping
farever.sound("beep")     -- generic beep
```

Plays a Windows system sound asynchronously. No audio files are
bundled, the names just route to the system's existing event sounds
so they always work regardless of mod install path. Useful in
combination with `cast_start` for boss telegraph warnings:

```lua
function on_event(name, data)
    if name == "cast_start" and data.skill == "Boar_Skill1" then
        farever.sound("warning")
    end
end
```

If a player has the system sound muted, the call simply does
nothing. There is no fallback PC beep on a muted system.

## Boss-mechanic plugins

The `target_changed` / `cast_start` / `cast_end` events plus
`farever.target.*` are designed for boss-helper plugins: warn the
player before a telegraphed skill lands, count phase transitions
based on HP percentages, etc. Sketch:

```lua
-- Per-skill "dodge in" hard-coded offsets. The mod's
-- cast_total_sec is the full skill window including recovery,
-- so the actual impact moment is usually a bit earlier. Bosses
-- you've fought a few times get their own line here.
local IMPACT_AT = {
    Boar_Skill1 = 2.0,
}

function on_event(name, data)
    if name == "cast_start" then
        local impact = IMPACT_AT[data.skill]
        if impact then
            farever.toast(string.format("%s incoming!", data.skill))
            -- Schedule the warning in on_render via cast_elapsed_sec.
        end
    end
end

local warned = false
function on_render()
    if not farever.target.is_casting() then warned = false; return end
    local skill   = farever.target.cast_skill()
    local impact  = IMPACT_AT[skill]
    local elapsed = farever.target.cast_elapsed_sec()
    if impact and not warned and (impact - elapsed) <= 1.0 then
        farever.sound("warning")
        warned = true
    end
end
```

## Logging

```lua
farever.log.info("loaded settings")
farever.log.warn("something looks off")
```

Both go to `farever-mod.log` in the game folder, prefixed with your
plugin name. Useful for debugging since you can keep the log open in
a separate editor while you iterate.

## Hot reload

Save your file. About a second later, the mod rebuilds your plugin
fresh. `on_init` runs again, your locals reset (because they are
inside a fresh Lua state), but the store survives because it lives
on disk.

If your plugin grows stateful and you want a value to persist across
reloads as you edit, put it in the store and read it from `on_init`.

## What the sandbox blocks

You cannot do these things from a plugin. This is on purpose:

- Open files outside the store (`io` is removed)
- Run shell commands (`os.execute`, `os.remove`, `os.exit` are gone)
- Load other Lua files (`require`, `dofile`, `loadfile`, `load` are gone)
- Reach into the mod's globals via `debug` (also gone)
- Read other players' positions (the mod itself never reads them)
- Cast spells, click for the user, modify game memory
- Play arbitrary audio files (only the four named system sounds in
  `farever.sound()` work — no custom WAV / MP3 paths)

If you find a real-world use case that needs one of these, open an
issue. We can probably expose a safe wrapper for it.

## Example plugins (optional download)

The mod ships empty. Two folders in the repo host ready-to-use
plugins for you to grab as starting points:

### First-party reference plugins (`examples/plugins/`)

- [`hello_world.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/hello_world.lua) — render, button, log, event basics.
- [`personal_best.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/personal_best.lua) — tracks your best DPS across sessions in the store, fires a toast on new records.
- [`target_probe.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/target_probe.lua) — every piece of the `farever.target.*` boss-helper surface in one file.
- [`api_inspector.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/api_inspector.lua) — living documentation of every read-surface getter. Drop it in to see what your character / target currently exposes.
- [`damage_planner.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/damage_planner.lua) — in-game version of Aragon's PvE damage calculator, with two-build comparison sliders and per-weapon damage memory.
- [`animation_demo.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/animation_demo.lua) — showcases the v0.5.6 animation surface (blinking text, pulsing size, custom cast bar, telegraph circle, big-red-alert pattern).

### Community submissions (`community-plugins/`)

Plugins authored by users of the mod. See
[`community-plugins/README.md`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/community-plugins/README.md)
for the current list and the submission process.

Save any of these into your `data/plugins/` folder, wait a second for
hot reload, and it shows up in the Plugin Manager.

## When things break

The Plugin Manager window shows the last error per plugin in red.
The same error is in `farever-mod.log`. Hit the reload button after
you fix it (or just save the file, hot reload will catch it).

The most common errors:

- Forgetting to return the new value from a stateful widget. Your
  checkbox keeps flipping back to unchecked because you did not
  store the result.
- Calling `imgui.begin` or `imgui.end`. Those are not exposed. The
  mod opens and closes the window for you.
- Using `1`-based for combo / table indices when you assumed
  `0`-based, or the other way around. Lua tables are `1`-based, and
  `imgui.combo` follows that convention.

If something is unclear, the source for the API is at
`src/farever-mod/plugins.cpp` in the mod's source tree.
