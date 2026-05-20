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

```

All of these are functions you call. They return the value at the
moment you ask. They never block. If the mod has not identified your
character yet (`locked()` returns false) the resource and defense
readers return 0 so plugin code can use them unconditionally.

> **Foes API note.** v0.5.3.1 shipped a `farever.foes.*` table for
> tracking mobs and bosses. It was the source of a crash a few
> seconds after the hero locks, so v0.5.3.2 pulled the whole thing
> out. If you already wrote a plugin against it, the table is `nil`
> now. A new foe-tracker is on the roadmap once the read path is
> rebuilt in isolation.

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

If you find a real-world use case that needs one of these, open an
issue. We can probably expose a safe wrapper for it.

## Example plugins (optional download)

The mod ships empty. Two examples live in the repo for you to grab
if you want a starting point:

[`examples/plugins/hello_world.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/hello_world.lua)
shows the basics: render, button, log, event.

[`examples/plugins/personal_best.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/personal_best.lua)
is the most complete example. It listens for `fight_end`, tracks your
best DPS across sessions in the store, and fires a toast when you set
a new record.

Save either one into this folder, restart the game (or just wait a
second for hot reload), and it shows up in the Plugin Manager.

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
