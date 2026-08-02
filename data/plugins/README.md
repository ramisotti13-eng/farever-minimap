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

-- Identity and progression
farever.player.name()                       -- character name ("" until loaded)
farever.player.class()                      -- "Rogue" / "Mage" / "Priest" / "Warrior" (v1.1.8+; "" until loaded)
farever.player.uid()                        -- stable account id, e.g. "S5a690f03" (v1.1.8+); constant across sessions
farever.player.level()                      -- character level

-- Health and energy
farever.player.health()                     -- current HP
farever.player.max_health()                 -- max HP
farever.player.health_pct()                 -- health / max_health, 0.0 .. 1.0
farever.player.health_regen()               -- HP regen rate
farever.player.shield()                     -- live total absorb, summed from
                                            -- active statuses (v1.1.4+)
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

-- Player codex lookup (v0.6.1+). Takes a Unit.kind id (exactly what
-- farever.target.name() returns) and reports your bestiary completion
-- for that monster. Returns nil if the codex isn't loaded yet OR the id
-- isn't a codex monster; otherwise a table:
--   { state    = "unknown" | "partial" | "complete",
--     completed = bool,    -- authoritative; can be true below max
--     progress  = int,     -- completionProgress (running counter)
--     max       = int,     -- maxProgress
--     name      = string,  -- localized display name ("Wolf")
--     path      = string } -- codex tree path
farever.player.codex(kind)
-- e.g.  local c = farever.player.codex(farever.target.name())
--       if c and c.state == "complete" then ... end

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
-- Each entry is { kind, duration, stacks, shield_amount }. shield_amount
-- (v1.1.4+) is the live absorb that status grants, 0 for non-shield ones;
-- sum them for your total shield. Plugins compute remaining time
-- client-side from farever.now() if they want a countdown.
local buffs = farever.player.statuses()
for i, s in ipairs(buffs) do
    print(i, s.kind, s.duration, s.stacks, s.shield_amount)
end
```

All of these are functions you call. They return the value at the
moment you ask. They never block. If the mod has not identified your
character yet (`locked()` returns false) the resource and defense
readers return 0 so plugin code can use them unconditionally.

### The character sheet (v1.1.5+)

The displayed stat values (the ones with gear, buffs and talents folded
in) are not stored on the character; the client computes them for the
character menu. The mod reads that menu, so `stats()` fills in after you
have opened the character sheet once per session.

```lua
-- Full sheet, language- and class-agnostic. Each entry is
-- { name = "Vitality", value = 124.0, text = "124" }. `name` is
-- canonical for the five primaries, otherwise the localized label.
for _, s in ipairs(farever.player.stats()) do
    print(s.name, s.value, s.text)
end
```

The typed getters (`vitality()`, `crit_chance()`, ...) give the same
numbers for the primaries and max health. The remaining secondary
getters still report the engine's base value, so prefer `stats()` when
you want what the character sheet shows.

### Equipment slots, bag and currencies

```lua
-- Equipped gear entries also carry slot / slot_name (v1.2.1+).
-- slot_name is one of Weapon1, Weapon2, OffhandWeapon, Head, Neck,
-- Shoulders, Chest, Back, Hands, Waist, Legs, Feet, FingerLeft, Trinket,
-- FingerRight, Pickaxe, Sickle ("" for the unnamed bag / mount slots),
-- so the two rings can finally be told apart.
for _, it in ipairs(farever.player.equipment()) do
    print(it.slot, it.slot_name, it.kind, it.level, it.upgrade)
end

-- Bag inventory (v1.2.1+; stacks and materials since v1.2.3).
-- { kind, level, upgrade, stack }. Holds gear AND stackable consumables
-- and crafting materials, so a drop tracker reads counts straight off it.
for _, it in ipairs(farever.player.inventory()) do
    print(it.kind, it.stack)          -- "LavendulaPetal  10"
end

-- Currencies (v1.2.3+). { kind, amount }. Gold and the like; crafting
-- materials are items and live in inventory() above.
for _, c in ipairs(farever.player.currencies()) do
    print(c.kind, c.amount)           -- "Gold  10433"
end
```

Both lists refresh at about 1 Hz.

### Skills and cooldowns (v1.2.1+ / v1.2.3+)

```lua
-- Arsenal / weapon skills in slot order (v1.2.1+).
-- { slot, kind, icon }  -- icon since v1.2.4, see below.
for _, s in ipairs(farever.player.weapon_skills()) do
    print(s.slot, s.kind)
end

-- Every skill the mod has resolved this session, with cooldowns
-- (v1.2.3+). { kind, cooldown, base_cooldown, charges, icon }
-- `cooldown` is the effective value the game uses (all reductions
-- applied), `base_cooldown` the unmodified one, so
-- 1 - cooldown / base_cooldown is that skill's total reduction.
for _, s in ipairs(farever.player.skills()) do
    print(s.kind, s.cooldown, s.base_cooldown, s.charges)
end

-- Global (gear) cooldown reduction, derived from the above, 0.0 .. 1.0.
farever.player.cooldown_reduction()
```

Skills populate as they resolve, which happens while you fight, so an
empty list right after login is normal.

### Skill icons (v1.2.4+)

The mod ships the game's own icon atlases and can draw a skill's icon
straight into your window.

```lua
-- Draw it. Returns false when the icon has not resolved yet, so you
-- can fall back to text.
if not imgui.icon("Mage_RayOfSpark", 24) then
    imgui.text("Mage_RayOfSpark")
end

-- Or read where it lives and draw it yourself / export it.
local ic = farever.icons.skill("Mage_RayOfSpark")
-- ic = { atlas = "atlas_class_Mage_96PX.png",
--        x = 0, y = 0,          -- cell indices in that atlas
--        size = 96,             -- cell edge in px
--        width = 1, height = 1, -- how many cells the icon spans
--        px = 0, py = 0,        -- the same rectangle in pixels
--        w = 96, h = 96 }
-- nil when the skill has not resolved yet.

-- Raw form, for a cell you already know:
imgui.atlas_icon("atlas_class_Mage_96PX.png", 0, 0, 96, 24)

-- Every kind that currently has an icon, sorted. Use this instead of
-- guessing at a name: these are the game's internal skill ids, which
-- are not always what the tooltip calls the skill.
for _, kind in ipairs(farever.icons.cached()) do
    farever.log.info(kind)
end
```

The atlases sit in `data/atlases/UI/icons/` next to the mod, and the
same `icon` table is attached to every entry of `player.skills()` and
`player.weapon_skills()`.

Icons resolve shortly after your skills are first read, within a few
seconds of the hero loading in. Up to and including v1.2.4 this only
happened while you were fighting, so a skill you had not just hit
something with stayed `nil` indefinitely; that was issue #104 and is
fixed in v1.2.5. A short `nil` window right after login is still
normal, so keep the text fallback.

### Party

```lua
farever.party.is_in_party()      -- true when you are grouped
farever.party.count()            -- number of OTHER members (you are not in it)
-- { name, class, uid, x, y, z, rot_z, health, max_health,
--   attr_ok, hero_valid }
-- attr_ok is false while a member's attributes have not replicated yet,
-- so health / max_health are only meaningful when it is true.
for _, m in ipairs(farever.party.list()) do
    print(m.name, m.class, m.health, m.max_health)
end
```

### Compass

```lua
farever.compass.is_visible()
farever.compass.radius()               -- in metres
farever.compass.cardinals_on()
-- add_marker(x, y [, { z, name, color, icon, ttl }]) -> handle
-- Same color / icon names as the waypoints above.
local h = farever.compass.add_marker(x, y, { name = "Add here", ttl = 30 })
farever.compass.remove_marker(h)
```

### POI list (v0.5.6.1+)

`farever.pois()` returns the full POI table the mod loaded at boot
from `data/pois_<world>.json` (~1224 entries on W1_Siagarta) as a
Lua array of tables. One snapshot per call, plugin authors filter
by `kind` themselves.

```lua
local pois = farever.pois()
for i, p in ipairs(pois) do
    -- p.x, p.y, p.z   world coordinates (float)
    -- p.kind          "chest" / "ore" / "plant" / "red_orb" /
                       "activity" / "dungeon" / "merchant" / ...
    -- p.subkind       optional sub-classification, may be empty
    -- p.name          display label as shown on the minimap
    -- p.id            stable unique id from the prefab tree
end
```

Replaces the pattern of hardcoding POI coordinates in your plugin.
When the mod loads a different world or the json gets updated, your
plugin sees the new data on next read with no code change.

### User waypoints (v1.0.0-beta4+, styling v1.0.0-beta5+)

`farever.waypoints` manages personal waypoints drawn on the built-in
minimap. They persist per world in `data/user_waypoints_<world>.json`
and survive restarts. Players can also add / rename / delete them
in-game by right-clicking the minimap; this API lets a plugin manage
the same set.

```lua
-- Add a waypoint at world (x, y[, z]) with an optional name and an
-- optional opts table for color + icon. Returns the new integer id,
-- or nil if the store is full (256 max).
local id = farever.waypoints.add(farever.player.x(), farever.player.y(),
                                 farever.player.z(), "Farm spot",
                                 { color = "orange", icon = "diamond" })

-- Change a waypoint's color and / or icon later. Each field optional;
-- nil leaves the current value unchanged. Returns true on success,
-- false if no waypoint has that id. Raises on unknown name string.
farever.waypoints.set_style(id, { color = "red", icon = "!" })

-- List all waypoints as an array of tables.
for _, w in ipairs(farever.waypoints.list()) do
    -- w.id            integer, stable for the session
    -- w.name          display label (shown on hover)
    -- w.x, w.y, w.z   world coordinates (float)
    -- w.color         color name string (see palette below)
    -- w.icon          icon name string  (see set below)
end

-- Remove by id. Returns true if a waypoint was removed.
farever.waypoints.remove(id)
```

`z` defaults to 0 and the name defaults to `"Waypoint"` when omitted.
Renaming is still in-game only for now (right-click a waypoint pin).

**Palette** (case-sensitive strings, 8 options, default `"cyan"`):
`cyan`, `red`, `orange`, `yellow`, `green`, `blue`, `magenta`, `white`.

**Icon set** (default `"pin"`):
`pin`, `flag`, `star`, `diamond`, `circle`, `exclamation` (alias `"!"`).

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
        -- data.target   (string, v1.1.8+) the victim's name, the same value
        --                farever.target.name() gives, captured at the hit so
        --                you do not have to read the target separately
        -- data.blocked  (number, v1.1.8+) the blocked portion of the hit
        --                (DamageResult._block); 0 when nothing was blocked

    elseif name == "heal_dealt" then
        -- (v1.1.2+) A heal you dealt (your own heals only).
        -- data.skill    (string)
        -- data.amount   (number, amount healed)
        -- data.is_crit  (boolean)
        -- data.target   (string, v1.1.8+) the heal recipient's name

    elseif name == "shield_applied" then
        -- (v1.1.4+) You gained or refreshed a shield. Fires on every
        -- (re)cast that raises your total active-status absorb, in or
        -- out of combat.
        -- data.skill    (string, the status that granted it, e.g.
        --                "Mage_ShieldOfSpark_Status")
        -- data.amount   (number, your total active shield afterwards)

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

-- The game's own skill icon (v1.2.4+), see "Skill icons" above.
imgui.icon("Mage_RayOfSpark", 24)
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
imgui.draw_triangle_filled(x1, y1, x2, y2, x3, y3, r, g, b, a)
imgui.draw_triangle(x1, y1, x2, y2, x3, y3, r, g, b, a, thickness)
```

A worked example with cast-bar, telegraph circle, blinking alert and
pulsing font size is in [`examples/plugins/animation_demo.lua`](../../examples/plugins/animation_demo.lua).
[`examples/plugins/nav_arrow.lua`](../../examples/plugins/nav_arrow.lua)
shows how to compose the math (player heading + world-space delta to a
target) with `draw_triangle_filled` to build a 3D-looking waypoint
arrow that tilts up or down as the target rises or drops.

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

## Combat logs (v1.1.7+)

The sandbox blocks `io`, so a plugin cannot write arbitrary files. If you
need to emit a log file an external tool can pick up (for example a
FareverLogs.com combat log), use the controlled writer:

```lua
local path, err = farever.write_combatlog(filename, contents)
```

It writes `contents` (a string you build, e.g. JSON) atomically into one
fixed folder, `%LOCALAPPDATA%\farever-minimap\combatlogs\`. The `filename`
must be a single safe component (`A-Za-z0-9._-`, 1..128 chars, no path
separators or `..`), so a plugin can never escape that folder. There is a
4 MiB per-call cap. On success it returns the full path; on failure it
returns `nil` plus an error string.

```lua
local name = string.format("%d-%s.json", os_epoch, run_id)
local path, err = farever.write_combatlog(name, json_text)
if not path then farever.toast("combat log failed: " .. err) end
```

Pair this with the `damage_dealt` / `heal_dealt` events (which carry
`target` and the local player's `farever.player.uid()`) to build a log
keyed to a player and their targets.

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
  `farever.sound()` work, no custom WAV / MP3 paths)

If you find a real-world use case that needs one of these, open an
issue. We can probably expose a safe wrapper for it.

## Example plugins (optional download)

The mod ships empty. Two folders in the repo host ready-to-use
plugins for you to grab as starting points:

### First-party reference plugins (`examples/plugins/`)

- [`hello_world.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/hello_world.lua): render, button, log, event basics.
- [`personal_best.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/personal_best.lua): tracks your best DPS across sessions in the store, fires a toast on new records.
- [`target_probe.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/target_probe.lua): every piece of the `farever.target.*` boss-helper surface in one file.
- [`api_inspector.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/api_inspector.lua): living documentation of every read-surface getter. Drop it in to see what your character / target currently exposes.
- [`damage_planner.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/damage_planner.lua): in-game version of Aragon's PvE damage calculator, with two-build comparison sliders and per-weapon damage memory.
- [`animation_demo.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/animation_demo.lua): showcases the v0.5.6 animation surface (blinking text, pulsing size, custom cast bar, telegraph circle, big-red-alert pattern).
- [`nav_arrow.lua`](https://github.com/ramisotti13-eng/farever-minimap/blob/main/examples/plugins/nav_arrow.lua): 3D-look navigation arrow that points toward an arbitrary world (x, y, z) waypoint and tilts up / down based on vertical offset. Patterns: world-to-player-local rotation via cos/sin of the heading, atan2 for screen angle and vertical tilt, `draw_triangle_filled` for the arrowhead.

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
