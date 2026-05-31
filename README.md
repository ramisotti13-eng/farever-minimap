# Farever Minimap & DPS

![Farever Minimap](minimap.gif)
![Farever DPS meter](dpsmeter.gif)

> ## Please read this first
>
> This is an unofficial **mod**. It can crash, and when it does the trigger is
> usually the game plus your GPU driver settings, not the mod reading state.
>
> **For the most stable experience, run Farever with default ("vanilla") GPU
> driver settings:**
>
> * **No frame generation of any kind** for Farever: NVIDIA Smooth Motion,
>   DLSS Frame Generation, AMD AFMF / Fluid Motion Frames, Lossless Scaling.
>   Farever has no built-in frame generation, so a driver-injected one fights
>   the overlay and can crash the game (`DXGI_ERROR_DEVICE_REMOVED`). If you
>   want to keep frame gen on, pick **Compatibility** mode in the dialog on
>   first launch (see [Which release](#which-release-do-i-download)), but
>   turning it off is the cleaner fix.
> * **No DSR / DLDSR** (dynamic super resolution), no forced VSync, no
>   driver-level sharpening, anti-lag or "ultra low latency" overrides for
>   Farever. Reset Farever to the global defaults in your NVIDIA / AMD control
>   panel if in doubt.
> * **Older Windows versions and older hardware can also crash.** The mod
>   draws through Direct3D 12; very old GPUs or unpatched OS installs are more
>   fragile here.
>
> If the mod or the game crashes, please open an issue and attach
> `farever-mod.log` from your Farever folder. That log is the fastest way to
> narrow the cause.

A drop-in overlay for Farever (Shiro Games) that bundles several tools into a
single DLL. Everything below is in one download, nothing extra to install.

## Features at a glance

* **Minimap** with a heading compass, the world map underneath, every POI the
  game already tracks (obelisks, respawn points, dungeons, world activities,
  merchants), and 800+ optional collectible markers (chests, red orbs, plants,
  ores) with per-character "collected" tracking.
* **Camera compass strip** across the top of the screen, Skyrim style, with
  cardinal directions and markers for POIs and party members. It follows your
  camera by default (toggleable to follow your character instead).
* **Custom waypoints**: right-click anywhere on the minimap to drop your own
  marker, rename or delete it, and it is saved per world.
* **Party display**: in a group, your party members appear on the minimap and
  the compass with their live position.
* **DPS / HPS meter**: a per-skill damage table with real in-game icons,
  healing tracking, combat-state detection, and a persistent fight history.
* **Boss speedrun timer** (opt-in): times your dungeon and arena boss fights and
  keeps your best, average and recent clear times per character.
* **Plugin runtime**: drop your own sandboxed Lua scripts into `data/plugins/`,
  hot-reloaded on save, with a rich read-only API and ImGui drawing.
* **Two render paths** (Fast and Compatibility) chosen on first launch, with
  Proton / Linux auto-detected so it just works there.
* **Your settings and progress survive a reinstall** (since v1.1.1), saved in
  your Windows user folder instead of next to the game.
* **Fully movable and lockable UI** with rebindable hotkeys.
* **Anti-cheat fail-safe**: if the mod ever detects an anti-cheat running, it
  refuses to inject at all.

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

## Status

**v1.1.1 is the current stable build.** It carries the full feature set in a
single DLL with a render-mode chooser: minimap, camera compass, custom
waypoints, party display, DPS + HPS meter, the boss speedrun timer, and the
plugin runtime.

Recent changes:

* **v1.1.1** moved all of your saved settings and progress out of the game
  folder and into your Windows user folder, so reinstalling the mod or
  verifying the game in Steam no longer wipes them (see
  [Where your settings live](#where-your-settings-live)). The options panel is
  now its own draggable window too.
* **v1.1.0** made the compass follow your camera, added the opt-in boss
  speedrun timer, and made the mod auto-detect Proton / Wine on Linux so the
  render-mode dialog is skipped there (the dialog does not draw correctly under
  Proton anyway).
* **v1.0.0** unified the older DirectComposition and swap-chain builds into one
  DLL with the render-mode chooser.

The `DX12Driver.present` access violation from the old v0.5.x series (issues
#41, #42, #43) was fixed back in v0.6.0 by reading game state from a background
thread that is invisible to the game's garbage collector, instead of riding the
game's render thread.

If the mod crashes for you, please open an issue with `farever-mod.log`
attached.

## Which release do I download?

Get **[v1.1.1](../../releases/latest)**, the current stable build, for
**Windows and Linux / Steam Play (Proton)**.

On **Windows**, the first launch asks how the overlay should draw. You choose
once (changeable later in the overlay settings, restart to apply):

* **Fast** (recommended): renders into the game's own Direct3D 12 swap
  chain. Best performance, and dodges the AMD MPO bug. Pick this unless you
  have problems.
* **Compatibility**: renders in a separate DirectComposition layer over the
  game. Use this if you run frame generation (see the disclaimer at the top)
  or if the game crashes in Fast mode.

On **Linux / Proton**, the mod detects that it is running under Proton/Wine and
picks **Fast** automatically (the only mode that works under vkd3d/DXVK), so
there is no dialog and no restart, the overlay just comes up.

If a build does not bring up the overlay on your machine, open an issue and
attach `farever-mod.log` from your Farever folder.

## How to install

1. Grab `farever-minimap-dps-1.1.1.zip` from the
   [Releases page](../../releases/latest).
2. Extract straight into your Farever folder, the one that contains
   `Farever.exe`. Typical Steam path:
   `C:\Program Files (x86)\Steam\steamapps\common\Farever`.
   You should end up with `dinput8.dll` and a `data\` folder sitting
   next to the game's executable.
3. Launch Farever from Steam. On Windows you pick the render mode on the first
   launch and restart once for it to take effect; on Linux / Proton it starts
   straight away.

There is no injector. Windows resolves `dinput8.dll` from the game
folder before its own copy, so dropping the file next to
`Farever.exe` is enough.

**Updating** is just swapping `dinput8.dll` for the new one. Your settings and
progress live elsewhere (see below) and are not touched. To **uninstall**,
delete `dinput8.dll` and the `data\` folder; to also wipe your saved settings,
delete the `farever-minimap` folder described in the next section.

The overlay shows up a few seconds after the title screen, once
your character is loaded. It hides itself during loading screens
and zone transitions, and comes back as soon as the next player
Hero is spawned in the world.

## Where your settings live

Everything the mod saves for you (hotkeys, window positions, collected orbs and
chests, waypoints, boss times, fight history, your render-mode choice) is stored
in your Windows user folder:

```
%LOCALAPPDATA%\farever-minimap
```

Paste that into the Explorer address bar to open it. On **Linux / Proton** it is
inside the Proton prefix, typically:

```
steamapps/compatdata/3672400/pfx/drive_c/users/steamuser/AppData/Local/farever-minimap
```

Because this folder lives outside the game directory, reinstalling the mod,
updating it, or letting Steam verify the game files no longer wipes your
progress. The first time you launch v1.1.1 it copies any existing settings from
the old `data\` location over automatically and leaves the originals in place as
a backup, so you keep what you already had.

That folder is also where you back up or move your setup: copy it to another PC
and your whole configuration comes with it.

The shipped read-only files (map tiles, icons, POI lists, plugins) and the
optional diagnostic `.flag` files still live in the game's `data\` folder.

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
press the key you want, done. The binding is saved to
`keybinds.json` in your settings folder so it persists across restarts.
Esc cancels a rebind in progress.

The minimap bezel buttons (and yes, you can drag them around the
rim to wherever you like, see Layout below):

* Pin: hold and drag to move the minimap around the screen
* Square: cycles three sizes (small, medium, large)
* Funnel: opens the options / POI filter panel (compass, party and
  render-mode toggles live here too); the panel is its own draggable window
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

## Camera compass

A horizontal compass strip sits across the top of the screen, Skyrim style. It
shows the cardinal directions (N, E, S, W and the diagonals) plus markers for
the POIs and party members around you, so you can keep your bearings without
looking down at the minimap.

By default it follows your **camera**, which makes turning with the mouse feel
natural. If you prefer it to follow your **character's facing** instead, flip the
"Compass follows camera" toggle in the options panel (funnel button on the
minimap bezel).

## Collectibles

The minimap also knows about 821 collectible spawn points across
the W1 world: 147 chests, 199 Red Orbs (the secret-world ones),
311 plants and 264 ore nodes. They are off by default. Toggle them
all at once with the chest button on the bezel, or pick categories
individually in the options panel.

To mark a chest or red orb as "done", **right-click it on the
minimap**. The icon stays visible but dims, and the panel shows a
"12 / 147" counter so completionists know how many are left. Plants
and ores respawn, so they have no counter and can't be marked done.

The "done" set is saved **per character**: each character gets its own
`poi_done__<name>.json` in your settings folder, so a brand-new character
starts with a fresh map instead of inheriting your main's collected state. When
you update from an older build, your existing global progress carries over to
the first character you log in on, and other characters start fresh from there.

POI icons grow about 80% on mouse hover, which makes hitting the
right one much easier when several are stacked.

## Custom waypoints

You can drop your own waypoints on the minimap. **Right-click an empty spot on
the minimap** and pick "Add waypoint here"; a pin appears at that world
position. **Right-click an existing pin** to rename or delete it. Waypoints are
shown on the minimap when they are in range and saved per world, so they come
back next time you log in.

Plugin authors can manage waypoints too through the `farever.waypoints` Lua API
(add / list / remove).

## Party

When you are in a group, the other members show up as markers on the minimap
and on the compass strip, with their live position updating as they move.
A member only shows once their character is actually loaded on your client
(in your zone / nearby); a party member in another zone has no position to
draw until they are close again, because the game does not stream their
character to you before then. Toggle the party display with the "Show party"
checkbox in the options panel.

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
  so the totals are exclusively your outgoing damage. Only the damage
  numbers the game itself shows above mobs are counted, so other party
  members' damage is out of the picture by construction.

* **HPS / healing**: the meter also tracks your healing output, with a
  DMG / HEAL toggle and an HPS figure in the header. Fight history keeps
  both per fight.

* **Fight history**: a collapsible block under the live table shows
  the last 10 sealed fights, with time, duration, total damage and
  DPS for each. Click a row to open a detail window with the full
  per-skill breakdown of that fight. History persists across restarts.

* **Responsive layout**: as you shrink the DPS window, columns
  drop progressively (Crit%, Max, Hits, Total, %). At the narrowest
  size you get just the icon and the DPS column.

## Boss speedrun timer

An opt-in timer for dungeon and arena boss fights. Turn it on in the options
panel. When you engage one of the named dungeon/arena bosses it starts a live
timer, and on the kill it records your clear time.

It keeps, **per character**, your best (personal best), average, last and worst
clear time for each boss, plus how many times you have killed and wiped to it,
and a small session summary. Beating your best is highlighted briefly. The
records are saved to `boss_times__<name>.json` in your settings folder.

Open-world bosses are deliberately not tracked, only the instanced dungeon and
arena bosses, so a roaming world boss does not start a run.

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
loadout, the current target and its cast bar, the compass, your
waypoints, and your party. They can draw their own ImGui window with
text, buttons, sliders, checkboxes, color pickers, combos, progress
bars, and custom shapes for animated alerts and telegraphs. They can
show centered toast notifications, play system sounds, and persist
per-plugin state to disk. They cannot touch game memory, network, the
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
buttons in a different spot than the defaults. The options panel is
its own window you can drag anywhere as well. The layout is saved
to `ui_state.json` in your settings folder together with the lock
flag. Delete that file to reset to defaults.

Window positions for the DPS meter, the minimap, the hotkeys window,
the options panel and the fight-detail view are saved to
`farever_layout.ini` in your settings folder. Drag your windows
wherever once; they come back there next time.

## Troubleshooting

**First, the cheap fixes:** if the game crashes, re-read the disclaimer at
the top and make sure no frame generation / DSR / driver overrides are active
for Farever. If the game crashes right at startup in Fast mode, set the render
mode to **Compatibility** (overlay settings, or drop an empty
`data\force_dcomp.flag` file next to `dinput8.dll` to force it), then restart.

If the overlay starts hitching, F8 and F10 turn the two windows off
independently. The mod also auto-disables itself if it detects the
GPU stalling on the overlay for too long in a row, so you should
not need a game restart to recover from a bad transition.

If the minimap or DPS meter does not come back right after a
dungeon load, give it ten or fifteen seconds. The mod waits for
the new player Hero to spawn on the client and locks onto it as
soon as the allocation comes through, which on slow loads can take
a bit longer than the loading screen itself.

In **Compatibility** mode, if the overlay never appears at all, check
`farever-mod.log` for `all composition swap chain variants failed`. That is
the AMD MPO interaction, and the mod drops three auto-repair files into your
Farever folder when this happens: `farever-fix-amd-overlay.reg` (double-click,
accept the prompt, reboot), `farever-undo-amd-overlay-fix.reg` (rollback), and
`OVERLAY_NOT_WORKING.txt` (plain-English step-by-step). Switching to **Fast**
mode skips the composition layer entirely, so the MPO interaction does not
apply there.

If the game crashes after a while, please zip the `farever-mod.log`
file from your Farever folder and open an issue with it attached.
Since 0.5.2 the previous session's log is kept as
`farever-mod.log.1` after a restart, so you can grab both files
even if you reproduce the crash and relaunch before uploading.

## Compatibility notes

* **Frame generation crashes Fast mode.** If you force NVIDIA Smooth Motion,
  DLSS Frame Generation, AMD AFMF, or Lossless Scaling onto Farever, the
  driver inserts its own frame between the game's rendered frame and present,
  which collides with the overlay drawing into the same swap chain and removes
  the GPU device (`DXGI_ERROR_DEVICE_REMOVED`). Use **Compatibility** mode if
  you keep frame gen on, or, better, turn frame gen off for Farever (it has no
  native support for it anyway).

* **Not compatible with RivaTuner Statistics Server (RTSS) on the
  same game**, including MSI Afterburner's OSD which uses RTSS
  underneath. Two overlay injection paths into the same D3D12
  device race and one of them eventually trips a GPU device-removed
  (DXGI `0x887A0005`). Workaround: in RTSS set "Application
  detection level" to None for `Farever.exe`, or use Steam's
  built-in FPS counter instead.

* **Native-resolution exclusive fullscreen bypasses the overlay
  (Compatibility mode only).** In exclusive fullscreen the game's swap chain
  flips straight to the GPU scanout and DWM is not in the loop, so the
  composition layer is invisible. Use borderless windowed instead, or switch
  to **Fast** mode, which renders into the game's own swap chain and does not
  have this problem.

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
  open the options panel (funnel button on the minimap bezel) and use
  the "UI scale (text)" slider, 2.0x to 2.5x is usually right for
  a 48 inch 4K display.

* **Alt-tab game crash since the v0.1.5.25921 patch.** Some users
  hit an access violation in the game's own DX12 renderer
  (`h3d.impl.DX12Driver.present`) after a long foreground loss
  followed by returning to the game. The mod is not the cause: when this
  happens the mod's own ticks keep firing cleanly while the game's render
  thread dies. Workarounds: hide the overlay (default F7) before
  alt-tabbing, switch to borderless windowed if you are in exclusive
  fullscreen, or try the opt-in `data/fg_detach.flag` and
  `data/cursor_park.flag` flags (drop empty files of those names into your
  Farever folder, restart). Filed upstream as a game-side issue.

* **Rapidly changing in-game resolution can crash the game.**
  Mitigated since v0.6.1, but cycling resolutions quickly can still
  push the swap chain into a `DEVICE_REMOVED` state and take the
  game's own render path down with it (an adapter-wide driver error).
  Safest is to quit to the launcher before changing resolution, then
  restart Farever.

## Contributors

Community plugins in [`community-plugins/`](community-plugins/) are
written and shared by players of the mod:

* [@iSkrumpie](https://github.com/iSkrumpie) - POI finder, PvE damage calculator, item finder
* [Ooshraxa](https://github.com/KaareGravesen) - mob codex checker
* [@Mupki](https://github.com/Mupki) - combat logger

Thanks for sharing your work. Want your plugin listed here? See the
[community-plugins README](community-plugins/README.md).

## Notes

The mod is read only. It reads your own player state out of the
game process and renders on top of Direct3D 12. It does not write to
game memory, it does not touch the network, and other players'
positions are read only for your own party display, nothing else. As an
extra safeguard, if the mod ever detects an anti-cheat running it refuses
to inject at all.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
