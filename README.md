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
zip the `farever-mod.log` file from your Farever folder and open an
issue with the file attached. Since 0.5.2 the previous session's
log is kept as `farever-mod.log.1` after a restart, so you can grab
both files even if you reproduce the crash and relaunch before
uploading. The log records what the mod was doing at the moment of
the crash and is the fastest way to narrow the cause.

## Compatibility notes

* **Not compatible with RivaTuner Statistics Server (RTSS) on the
  same game**, including MSI Afterburner's OSD which uses RTSS
  underneath. Two overlay injection paths into the same D3D12
  device race and one of them eventually trips a GPU device-removed
  (DXGI `0x887A0005`). Workaround: in RTSS set "Application
  detection level" to None for `Farever.exe`, or use Steam's
  built-in FPS counter instead, which is much more compatible with
  composition overlays.

* **FPS is capped to your monitor refresh rate while the overlay
  is active.** Side-effect of how DirectComposition mounts on the
  game window: it pulls the game out of independent-flip
  presentation into DWM-composed presentation, which syncs to the
  display refresh. For most players this matches what vsync would
  do anyway (60 Hz = 60 FPS, 144 Hz = 144 FPS, etc). If you want
  uncapped FPS for input latency, toggle the mod off with F7.

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

* **Big monitors (4K and up) need a UI scale bump.** Default text
  size is tuned for 1080p / 1440p. On 4K + large display open the
  filter tablet (funnel button on the minimap bezel) and use the
  "UI scale (text)" slider, 2.0x to 2.5x is usually right for
  a 48 inch 4K display.

* **Overlay does not show up on some older GPU driver builds.** If
  the mod loads but no overlay appears, check `farever-mod.log` for
  `CreateSwapChainForComposition` failures. The 0.5.2.1 fallback
  path tries three driver-friendly variants, but very old Win10
  builds (pre-1809) may reject all of them. Updating the GPU
  driver usually fixes it.

## What's new in 0.5.2.1

Small patch release on top of 0.5.2, two user-visible fixes plus a
diagnostic improvement:

* **UI text scale slider**
  ([#22](https://github.com/ramisotti13-eng/farever-minimap/issues/22)).
  EpicTragedy reported that on a 48 inch 4K monitor the overlay text
  and icons are too small to read. Added a "UI scale (text)" slider
  to the filter tablet (the bezel funnel button). Range 0.50x to
  2.50x, default 1.00x, persisted across sessions in
  `ui_state.json`. Hand-drawn bezel icons keep their pixel size by
  design, this only scales rendered text (DPS table, Hotkeys panel,
  Fight Detail, tooltips). 2.0x or 2.5x is the right ballpark for
  4K + large display.

* **DXGI composition swap chain fallback**
  ([#20](https://github.com/ramisotti13-eng/farever-minimap/issues/20),
  [#21](https://github.com/ramisotti13-eng/farever-minimap/issues/21)).
  Two users hit `CreateSwapChainForComposition` failing with
  `0x887A0001` (DXGI_ERROR_INVALID_CALL) on game launch, so the
  overlay never came up even though the game itself ran fine.
  Composition swap chains have stricter format / swap-effect
  requirements than regular DX12 swap chains and some older GPU
  drivers reject specific combos. 0.5.2.1 tries three descriptor
  variants in sequence (BGRA + flip-discard, RGBA + flip-discard,
  BGRA + flip-sequential), the first one the driver accepts wins.
  If all three fail the log says so clearly so the issue is at least
  visible.

* **Adapter info logged at startup**. `farever-mod.log` now records
  the GPU name, vendor / device IDs, and VRAM at the moment the
  overlay's D3D12 device is created. Makes "overlay does not show
  up" issues much faster to diagnose because the GPU + driver combo
  is right there in the log.

## Changelog

See the [Releases page](../../releases) for the full version history (0.1 through 0.5.2.1). Highlights of recent versions are kept in this README; everything older lives in the per-release notes.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
