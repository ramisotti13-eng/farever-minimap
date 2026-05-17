# Farever Minimap & DPS

![Farever Minimap](minimap.gif)
![Farever DPS meter](dpsmeter.gif)

A drop-in overlay for Farever (Shiro Games) with two tools in one DLL:

* **Minimap**: compass with a heading arrow, world mosaic underneath,
  the POIs the game already tracks (obelisks, respawn points,
  dungeons, world activities, merchants), and 800+ optional
  collectible markers (chests, red orbs, plants, ores) you can
  toggle in.
* **DPS meter**: per-skill damage table with real in-game icons,
  fight history, and combat-state tracking. Only counts the damage
  numbers the game itself shows above mobs, so other party members,
  ambient world damage and bleeds on you are out of the picture by
  construction.

## How to install

1. Grab the latest `.zip` from the [Releases page](../../releases).
2. Extract straight into your Farever folder, the one that contains
   `Farever.exe`. Typical Steam path:
   `C:\Program Files (x86)\Steam\steamapps\common\Farever`.
   You should end up with `dinput8.dll` and a `data\` folder sitting
   next to the game's executable.
3. Launch Farever from Steam.

There is no injector. Windows resolves `dinput8.dll` from the game
folder before its own copy, so dropping the file next to
`Farever.exe` is enough. To uninstall, delete `dinput8.dll` and the
`data\` folder.

The overlay shows up a few seconds after the title screen, once
your character is loaded. It hides itself during loading screens
and zone transitions, and comes back as soon as the next player
Hero is spawned in the world.

## Controls

Default hotkeys (all rebindable in game, see below):

| Key | Action |
| --- | --- |
| F8  | Toggle the minimap |
| F10 | Toggle the DPS meter |
| F9  | Reset the current DPS pull |

To remap, click the small key icon on the minimap bezel. A
"Hotkeys" window pops up with three slots. Click a slot, press the
key you want, done. The binding is written to `data\keybinds.json`
so it persists across restarts. Esc cancels a rebind in progress.

The minimap bezel buttons (and yes, you can drag them around the
rim to wherever you like, see Layout below):

* Pin: hold and drag to move the minimap around the screen
* Square: cycles three sizes (small, medium, large)
* Funnel: opens the POI filter panel
* Padlock: locks / unlocks the whole overlay layout
* Key: opens the hotkey rebind window
* Chest: toggles all four collectible categories at once
* Plus and Minus: zoom between roughly 10x and 20x
* Minus on top: shrinks the minimap to a small puck; click the puck
  to expand it again

The DPS window has a custom padlock toggle at the very start of the
status line that does the same job as the bezel padlock. Clicking
either locks every overlay window in place. The standard ImGui
title-bar collapse arrow on the left also shrinks the window to one
row when you want it out of the way. Drag the title bar to move.

## Collectibles

The minimap also knows about 821 collectible spawn points across
the W1 world: 147 chests, 199 Red Orbs (the secret-world ones),
311 plants and 264 ore nodes. They are off by default. Toggle them
all at once with the chest button on the bezel, or pick categories
individually in the filter panel.

To mark a chest or red orb as "done", **right-click it on the
minimap**. The icon stays visible but dims, and the filter panel
shows a "12 / 147" counter so completionists know how many are
left. The done set is written to `data\poi_done.json` and persists
across sessions. Plants and ores respawn, so they have no counter
and can't be marked done.

POI icons grow about 80% on mouse hover, which makes hitting the
right one much easier when several are stacked.

## DPS meter

* **Real skill icons** for every class skill, weapon skill and
  basic attack. At the first damage event for each skill the mod
  reads the icon's atlas, cell coordinates and size out of the
  game's own data, then loads that atlas on demand. No per-class
  configuration needed.

* **Combat state** indicator next to the elapsed time. Red dot
  while in combat, grey dot when idle. The mod uses the game's own
  `isInCombat` flag, so dodging or blocking without dealing damage
  keeps the fight open instead of prematurely sealing it.

* **Bleeds and incoming damage are filtered out**. Every floating
  damage number whose target is your own character (mob hits,
  ground AoE, DOTs on you) is dropped before it reaches the meter,
  so the totals are exclusively your outgoing damage.

* **Fight history**: a collapsible block under the live table shows
  the last 10 sealed fights, with time, duration, total damage and
  DPS for each. Click a row to open a detail window with the full
  per-skill breakdown of that fight.

* **Responsive layout**: as you shrink the DPS window, columns
  drop progressively (Crit%, Max, Hits, Total, %). At the narrowest
  size you get just the icon and the DPS column.

## Layout

Click the padlock (either on the minimap bezel or in the DPS-meter
status line) to lock the overlay. While locked, nothing moves:
title-bar drags, the minimap pin, and the bezel button right-drag
described next are all disabled. Click the padlock again to unlock.

While unlocked, **right-click and drag** on any bezel button to
slide it around the minimap rim. Useful if you want the most-used
buttons in a different spot than the defaults. The layout is saved
to `data\ui_state.json` together with the lock flag. Delete that
file to reset to defaults.

Window positions for the DPS meter, the minimap, the hotkeys window
and the fight-detail view are saved to `data\farever_layout.ini`.
Drag your windows wherever once; they come back there next time.

## Troubleshooting

If the overlay starts hitching, F8 and F10 turn the two windows off
independently. The mod also auto-disables itself if it detects the
GPU stalling on the overlay for too long in a row, so you should
not need a game restart to recover from a bad transition.

If the minimap or DPS meter does not come back right after a
dungeon load, give it ten or fifteen seconds. The mod waits for
the new player Hero to spawn on the client and locks onto it as
soon as the allocation comes through, which on slow loads can take
a bit longer than the loading screen itself.

If the game still crashes after a while with this version, please
zip the `farever-mod.log` file from your Farever folder (close the
game first, do not restart it - the log gets overwritten on launch)
and open an issue with the file attached. The log records what the
mod was doing at the moment of the crash and is the fastest way to
narrow the cause.

## What's new in 0.4.10

Two changes informed by v0.4.9's new heartbeat counters.

* **Post-transition pause extended from 2 s to 5 s** (issue #12).
  The v0.4.9 log from the affected user showed the pause fired
  correctly at hero lock, our overlay resumed cleanly 2 s later,
  ran 1441 frames without any guard tripping, and the AV still
  hit ~5 s after lock. So whatever game-side event tips over has
  a fixed delay relative to lock, not to our submission --- the
  fix is to stay paused through that whole window. 120 frames
  becomes 300 frames.
* **Pause DPS tracking hotkey** (issue #13). New default `F12`,
  rebindable in the Hotkeys panel. When on, the DamageDisplay
  alloc-hook callback returns immediately and damage_tick
  short-circuits before any work, so even in dense mob farming
  the per-event overhead drops to just the MinHook trampoline
  itself. Toggling F10 only hides the window; F12 actually pauses
  the backend. State is persisted to `ui_state.json`. A live
  status line in the Hotkeys window shows whether tracking is
  active or paused.

## What's new in 0.4.9

Diagnostic build. v0.4.8's post-transition pause shipped working
heuristics for the teleport / first-spawn crashes (confirmed in
issue #11), but a third crash class still surfaced on a long
AFK + alt-tab session. The existing alt-tab guards may or may not
have fired in that window -- the previous heartbeat only said
"alive" without distinguishing submitted vs skipped frames.

0.4.9 changes the overlay heartbeat to a per-guard counter so a
post-crash log shows exactly what happened in the last ~10 s
before the freeze:

```
overlay: alive @ tick N submitted=X skip(no_hero=A pause=B
  iconic=C hidden=D fg=E fence=F) auto-disabled=0
```

If the next AFK + alt-tab crash log shows `fg=600` for the
windows around the crash, the alt-tab guard was doing its job
and the bug is somewhere outside our overlay submission. If
it shows `submitted=600`, the guard wasn't catching the
alt-tab and we have a concrete bug to fix.

No behaviour changes other than the heartbeat format -- the
v0.4.8 post-transition pause is still active.

## What's new in 0.4.8

Targets the `DX12Driver.present` AV pattern that keeps surfacing
on zone transitions / fresh hero spawns (issues #11 + #12). Logs
show the crash hits right after the game's render pipeline starts
streaming a new chunk while our overlay is still submitting a
command list on top.

Both crash logs share the same shape:

* Issue #11: cross-zone teleport (Mayda → Azuram), crash ~31 s
  later while walking around the new zone.
* Issue #12: first hero spawn after the title screen, crash ~5 s
  after the LOCKED event when the minimap first starts drawing
  the mosaic + 1082 POIs.

0.4.8 adds a **post-transition overlay pause**. Two heuristics
catch both scenarios:

1. Hero pointer changes (fresh lock, zone transition, recovery
   after a re-validation failure).
2. Hero position jumps more than 500 game units between frames
   (in-place teleport where the existing Hero just gets a new
   position rather than being replaced).

When either fires, overlay submission is skipped for the next
120 frames (~2 s @ 60 fps) so the game's own DX12 streaming
finishes before we add another command list on top. The pause
is logged so it shows up in any future crash log.

DPS-meter ticking, hero tracking and the alt-tab guard continue
running during the pause -- only the overlay's GPU submission is
held off. From the user's side this looks like the overlay
disappearing for a couple of seconds right after a teleport,
then snapping back.

## What's new in 0.4.7

Round two of the user-reported issue sweep.

* **Click-through toggle** (issues #4 and #7). New default hotkey
  `F11`. When click-through is ON, the mouse passes straight through
  the overlay to the game -- attacks and camera rotation aren't
  intercepted any more, even when the cursor happens to land on the
  DPS meter or the minimap. When it's OFF, the overlay reacts to
  clicks normally (drag windows, click bezel buttons, rebind keys).
  State is persisted to `ui_state.json` and shown in the Hotkeys
  window. Rebindable in the same panel.
* **Minimap opacity slider** (issue #2). Filter panel now has a
  slider below the collectible toggles, range 30% to 100%. Affects
  the mosaic and the bezel ring; player arrow, POI markers and
  bezel buttons stay full-alpha so they don't fade away. Persisted
  to `ui_state.json`.
* **Foreground-window guard** (continuation of issues #9 / #10).
  v0.4.6 caught `IsIconic` / `!IsWindowVisible`, but alt-tab to a
  non-fullscreen window (Discord overlay, browser tab) doesn't
  trigger either of those -- the game stays visible, just loses
  focus. v0.4.7 also skips overlay submission when
  `GetForegroundWindow() != game_hwnd`, which is the case for that
  scenario. Should land the remaining alt-tab AV path reported in
  issue #11.

## What's new in 0.4.6

User-reported issue sweep:

* **Alt-tab crash fix** (issues #9, #10). When the game window is
  iconic or hidden, we now skip overlay submission entirely. DXGI's
  Present can return `DXGI_STATUS_OCCLUDED` in that state and our
  back-buffer references aren't guaranteed valid, which matches the
  alt-tab crash signature in the recent log uploads.
* **Off-screen window rescue** (issue #1). If the minimap, DPS
  meter, hotkeys window or fight-detail window load with a saved
  position that's outside the current viewport, they snap back to a
  visible spot the next frame. Used to require deleting
  `farever_layout.ini` by hand to recover.
* **Minimap size now persists across launches** (issue #6). The
  compass size cycle (small / medium / large) is saved to
  `ui_state.json` alongside the lock flag and bezel angles.
* **Lock also blocks resize now** (issue #4 partial). Previously
  locking the UI only stopped window moves, the resize corner was
  still grabbable. With v0.4.6 the lock pins both. The full
  click-through-on-mouselook fix (issues #4, #7) is the next
  iteration.
* **Fight-detail window default position** (issue #5). Used to open
  at (80, 80) directly under the DPS meter, which made it look like
  part of it. Bumped to (240, 240) so it's clearly a separate,
  draggable window. (It was always movable; the default position
  just hid that fact.)
* **Quit-error fix** (issue #8). Stopped doing a full MinHook
  uninstall on process detach. The process is dying anyway; the
  uninstall raced the game's own DX12 / DXGI tear-down and produced
  the exit error dialog. Lean shutdown now.

## What's new in 0.4.5

The 0.4.4 logs showed the crash pattern: combat starts, a sudden
burst of damage events arrives (one Mage_Conduit_Projectile
channel produced ~65 `DamageDisplay` allocs in 11 seconds), and a
few seconds later Present trips. 0.4.5 spreads that burst out
instead of letting damage_tick process the whole queue in one
render frame.

Changes:

* `damage_tick` now processes at most 4 events per render frame.
  A burst of 65 events drains over ~16 frames instead of in one
  ~10 ms tick. DPS numbers update at the same rate they always
  did because the display is sampled, not realtime.
* The pending-damage queue is now capped at 256 entries. If a
  combat burst overruns that, the oldest entries get dropped
  instead of growing the queue without bound. You lose a tiny bit
  of DPS-tracking accuracy in the extreme case and gain a hard
  ceiling on render-thread work.
* Added an overlay-render heartbeat that logs "overlay: alive @
  tick N" every 600 frames. Combined with the existing damage
  heartbeat this lets a post-crash log pinpoint whether the
  freeze hit before or after our overlay submission.

If the crash still happens on 0.4.5, please attach the
`farever-mod.log` to the issue -- the new heartbeat lines tell
me which sub-stage of the frame died.

## What's new in 0.4.4

Continued stability work after 0.4.3. Disabled the entity tracker
(chests / gather nodes / obelisks) entirely. The 0.4.3 logs showed
it was still slipping past the type-anchor guard and reading
random strings out of the engine's string pool (shader uniform
names, FMOD event paths, prefab paths) on long sessions — the
same pattern that correlates with the recurring
`DX12Driver.present` crash. Cutting it out removes that read
path completely.

What you lose: the minimap no longer auto-dims chests and red
orbs you've already looted. Use the right-click toggle on the
minimap to mark them done manually (still persists to
`data\poi_done.json` across sessions).

What you keep: everything else. Minimap, POI filter, hotkeys,
DPS meter, fight history, the lot.

## What's new in 0.4.3

Removed the experimental Atlas window. It was an item / boss /
dungeon catalog with runtime item-icon captures, but the icon
pipeline never landed cleanly and the extra alloc hooks (every
chest, every `st.item.*` subclass, every inventory slot UI
element) were adding more crash surface than the feature returned
value. Stripping it out keeps the mod focused on what it does
well: minimap + DPS meter.

If you have leftover files from 0.4.1 / 0.4.2, you can safely
delete:

* `data\cdb_atlas*.tsv`
* `data\item_captures.tsv`
* `data\portraits\` (the whole folder; ~70 MB)

## What's new in 0.4.2

A further stability pass on top of 0.4.1, plus Atlas multi-language.

* Throttled the entity tracker (chests / gather nodes / obelisks)
  to run every fourth render frame instead of every frame. The
  per-entity memory probes added up to a few thousand reads per
  second on long sessions; the discovered set is only sampled by
  the minimap, so quartering the rate is invisible to the user but
  meaningfully relieves the render-thread budget.
* Removed the 30-second verbose "snapshot" log dump from the entity
  tracker. It burst 100+ synchronous `fflush` calls into a single
  render frame, which under load could race the game's own Present
  submission. Per-entity transition and first-sight logs are kept.
* Atlas now ships nine language TSVs (de / es / fr / ja / ko / pl /
  pt-BR / ru / zh) plus a synthetic en fallback. The Atlas window
  has a language picker at the top; selection is process-local for
  now. UI chrome (tab labels, buttons, hints) was translated to
  English.

## What's new in 0.4.1

Stability-focused update on top of 0.4.

* Hardened every long-lived pointer cache against the Boehm GC's
  slot-reuse pattern, which was responsible for the dungeon-entry
  access violations a few users reported. Every entity, chest and
  item the mod tracks is now compared against its expected HashLink
  type before any field is read; mismatches drop the pointer silently
  instead of walking a freed slot.
* Bounded all GPU fence waits on the texture loader to 2 seconds
  (previously unbounded), so a stalled atlas load can never freeze
  the game's Present.
* Added the Atlas window as a prototype.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
