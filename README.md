# Farever Minimap & DPS

![Farever Minimap](minimap.gif)
![Farever DPS meter](dpsmeter.gif)

A drop-in overlay for Farever (Shiro Games) with three tools in one DLL:

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
* **Plugin runtime**: drop your own Lua scripts into `data/plugins/`
  and the mod loads them sandboxed with hot reload on save. Plugin
  authors can read player + target state, HP, the active cast bar,
  react to `target_changed` / `cast_start` / `cast_end` /
  `weapon_changed` events, draw their own ImGui windows, and play
  audible warnings. Enough to build boss-helper plugins, custom
  HUDs, and navigation arrows. See
  [plugin authoring guide](data/plugins/README.md) and
  [`examples/plugins/`](examples/plugins/) for reference plugins.

## Are add-ons allowed?

Yes, for personal use. The Farever developers addressed add-ons on
their official Discord:

> While we won't promote the use of add-ons during the EA (to keep
> players on the intended experience at first), we won't condemn
> personal use of add-ons like minimaps or DPS meter.

![Farever developers on personal add-on use (official Discord)](https://i.imgur.com/FYduqe4.png)

This overlay is read-only personal use exactly along those lines: it
reads your own player state and draws on top of the game, nothing
more (see [Notes](#notes) at the end).

## Status (June 2026)

**v0.6.3 is the current stable build, and the v1.0.0 line is in beta.**
The recurring `DX12Driver.present` access violation from the v0.5.x
series (issues #41, #42, #43) was fixed in v0.6.0: the mod reads game
state from its own background thread that is invisible to the game's
garbage collector, instead of riding the game's render thread, which
removes the class of race that was causing the mid-session crash.

The **v1.0.0 beta** unifies the old DirectComposition and swap-chain
builds into one DLL and renders straight into the game's own Direct3D 12
swap chain by default. That removes the desktop-compositor overhead,
dodges the AMD MPO bug, and is the candidate path for Proton / Steam
Play (Linux). It carries the full feature set (minimap, DPS + HPS meter,
plugins, collectibles, custom minimap waypoints).

If the mod crashes for you, please open an issue with `farever-mod.log`
attached.

## Which release do I download?

**On Linux / Steam Play (Proton), use the v1.0.0 beta.** The stable
v0.6.x build renders through DirectComposition, which does not work
under Proton, so v0.6.3 will not bring up the overlay on Linux. Get
this right before you download.

Two builds on the [Releases page](../../releases):

* **[v0.6.3](../../releases/latest)** — current stable build,
  **Windows only.** The default on Windows. Uses the DirectComposition
  overlay path, which is not Proton-compatible.
* **[v1.0.0 beta](../../releases)** — the next line, in preview, and
  **the build for Linux / Proton.** Grab the latest `v1.0.0-betaN`
  prerelease. It renders into the game's own Direct3D 12 swap chain by
  default (no DirectComposition), so it runs under Proton / vkd3d, has
  no desktop-compositor overhead, and dodges the AMD MPO bug. Same
  feature set as the stable build, plus HPS tracking and custom minimap
  waypoints. Windows users who want the newest features can run it too.

Both carry the full feature set: minimap, DPS meter, plugin runtime,
collectibles, Lua API. If a build does not bring up the overlay on your
machine, open an issue and attach `farever-mod.log` from your Farever
folder.

## How to install

1. Pick a release above and grab the matching `.zip`.
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

| Key  | Action |
| ---- | ------ |
| F7   | Toggle overlay (show / hide entirely) |
| F8   | Toggle the minimap |
| F9   | Reset the current DPS pull |
| F10  | Toggle the DPS meter |
| F11  | Toggle click-through (mouse passes to game) |
| F12  | Pause DPS tracking |
| Home | Reset window positions (snaps everything back to defaults) |

To remap, click the small key icon on the minimap bezel. A
"Hotkeys" window pops up with one row per binding. Click a slot,
press the key you want, done. The binding is written to
`data\keybinds.json` so it persists across restarts. Esc cancels a
rebind in progress.

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

## Plugin runtime

Drop a `.lua` file into `data/plugins/` and the mod loads it
automatically. The folder ships empty. Two folders in the repo host
ready-to-use plugins:

* [`examples/plugins/`](examples/plugins/): first-party reference
  plugins maintained with the mod (hello_world, personal_best,
  target_probe boss-helper, damage_planner, api_inspector, animation
  demo).
* [`community-plugins/`](community-plugins/): submissions from users
  of the mod. Each file has the author's GitHub handle in its
  filename and a header naming the source. See that folder's README
  for the submission process.

Plugins get sandboxed Lua 5.4. They can read your player position,
DPS, in-combat flag, fight events, the equipped weapon and the full
loadout, plus the current target and its cast bar. They can draw
their own ImGui window with text, buttons, sliders, checkboxes,
color pickers, combos, progress bars, and custom shapes for
animated alerts and telegraphs. They can show centered
toast notifications, play system sounds, and persist per-plugin
state to disk. They cannot touch game memory, network, the
filesystem outside their own state, or other plugins' state. A bad
plugin only crashes itself; the mod keeps running.

Full authoring guide is at
[`data/plugins/README.md`](data/plugins/README.md) (also included
inside the zip).

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

If the overlay never appears at all, check `farever-mod.log` for
`all composition swap chain variants failed`. That is the AMD MPO
interaction, and the mod drops three auto-repair files into your
Farever folder when this happens:
`farever-fix-amd-overlay.reg` (double-click, accept the prompt,
reboot), `farever-undo-amd-overlay-fix.reg` (rollback), and
`OVERLAY_NOT_WORKING.txt` (plain-English step-by-step). If the .reg
trick does not bring it up either, try the v1.0.0 beta: it renders
into the game's own swap chain and skips the composition layer
entirely, so the MPO interaction does not apply.

If the game crashes after a while, please zip the `farever-mod.log`
file from your Farever folder and open an issue with it attached.
Since 0.5.2 the previous session's log is kept as
`farever-mod.log.1` after a restart, so you can grab both files
even if you reproduce the crash and relaunch before uploading. The
log records what the mod was doing at the moment of the crash and
is the fastest way to narrow the cause.

## Compatibility notes

* **Not compatible with RivaTuner Statistics Server (RTSS) on the
  same game**, including MSI Afterburner's OSD which uses RTSS
  underneath. Two overlay injection paths into the same D3D12
  device race and one of them eventually trips a GPU device-removed
  (DXGI `0x887A0005`). Workaround: in RTSS set "Application
  detection level" to None for `Farever.exe`, or use Steam's
  built-in FPS counter instead, which is much more compatible with
  composition overlays.

* **Native-resolution exclusive fullscreen bypasses the overlay
  (v0.6.x only).** Side-effect of how DWM presents to the display:
  in exclusive fullscreen the game's swap chain flips straight to
  the GPU scanout and DWM is not in the loop, so our composition
  layer is invisible. Use borderless windowed instead. Most monitor
  + GPU combos run borderless at essentially the same framerate as
  exclusive these days. The v1.0.0 beta doesn't have this problem in
  its default mode because it renders directly into the game's own
  swap chain, no composition layer involved.

* **Holding ALT plus left mouse button on an overlay window can
  trigger auto-attack when ALT is released**
  ([#19](https://github.com/ramisotti13-eng/farever-minimap/issues/19)).
  ImGui captures the LMB-down on the overlay so the game's wndproc
  never sees it. When you release ALT, Farever switches to camera
  mode and polls the physical LMB state via raw input, which still
  reports LMB as held, which in camera mode reads as a continuous
  attack. Workarounds: release LMB before pressing ALT, or hide
  the overlay with F7 while playing actively and toggle it back on
  when you want to read the minimap or DPS numbers.

* **Big monitors (4K and up) need a UI scale bump.**
  Default text size is tuned for 1080p / 1440p. On 4K + large display
  open the filter tablet (funnel button on the minimap bezel) and use
  the "UI scale (text)" slider, 2.0x to 2.5x is usually right for
  a 48 inch 4K display.

* **Alt-tab game crash since the v0.1.5.25921 patch.** Some users
  hit an access violation in the game's own DX12 renderer
  (`h3d.impl.DX12Driver.present`) after a long foreground loss
  followed by returning to the game. Pattern: alt-tab to Discord /
  browser / Steam overlay for several seconds, come back, the game
  crashes within a few frames. The mod is not the cause: when this
  happens the mod's own ticks (`damage`, `hero_state`, `overlay
  alive`) keep firing cleanly while the game's render thread dies.
  On a few setups the timing is still reproducible. Workarounds:
  hide the overlay (default F7) before alt-tabbing, switch to
  borderless windowed if you are in exclusive fullscreen, or try
  the opt-in `data/fg_detach.flag` and `data/cursor_park.flag`
  flags (drop empty files of those names into your Farever folder
  next to `dinput8.dll`, restart). Filed upstream as a game-side
  issue; nothing we can fix from the mod's side directly.

* **Proton / Steam Play (Linux): try the v1.0.0 beta**
  ([#45](https://github.com/ramisotti13-eng/farever-minimap/issues/45)).
  The v0.6.x stable rendering path uses DirectComposition through DXGI,
  the part of the Windows graphics stack Proton's wined3d / vkd3d shims
  cover least completely, so the stable build is unlikely to bring up
  the overlay under Proton. The **v1.0.0 beta line** renders straight
  into the game's own Direct3D 12 swap chain by default and skips the
  DirectComposition layer entirely, which is the path most likely to
  work under Proton / vkd3d. Grab the latest v1.0.0 prerelease (the
  game-swapchain build) from the
  [Releases page](https://github.com/ramisotti13-eng/farever-minimap/releases),
  and whether it works or not please attach `farever-mod.log` to
  [#45](https://github.com/ramisotti13-eng/farever-minimap/issues/45)
  so we can confirm a working setup.

* **Rapidly changing in-game resolution can crash the game.**
  Mitigated since v0.6.1, but cycling resolutions quickly can still
  push the overlay's swap chain into a `DEVICE_REMOVED` state and
  take the game's own render path down with it (an adapter-wide
  driver error). Safest is to quit to the launcher before changing
  resolution, then restart Farever.

## Contributors

Community plugins in [`community-plugins/`](community-plugins/) are
written and shared by players of the mod:

* [@iSkrumpie](https://github.com/iSkrumpie) — POI finder, PvE damage calculator, item finder
* [Ooshraxa](https://github.com/KaareGravesen) — mob codex checker

Thanks for sharing your work. Want your plugin listed here? See the
[community-plugins README](community-plugins/README.md).

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
