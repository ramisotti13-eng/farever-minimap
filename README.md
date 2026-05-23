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
  audible warnings — enough to build boss-helper plugins, custom
  HUDs, and navigation arrows. See
  [plugin authoring guide](data/plugins/README.md) and
  [`examples/plugins/`](examples/plugins/) for reference plugins.

## Status (June 2026)

**v0.6.0 is the stable rewrite.** The recurring `DX12Driver.present`
access violation from the v0.5.x series (issues #41, #42, #43) is
fixed. The mod now reads game state from its own background thread
that is invisible to the game's garbage collector, instead of riding
the game's render thread — which removes the class of race that was
causing the mid-session crash.

Feature parity with v0.5.6.1 is restored: hero lock and minimap, DPS
meter with real skill icons, target tracking, cast bar timing, fight
history, plugin runtime, collectibles. Long-session testing (combat,
zone changes, AFK) reaches clean shutdowns rather than crash-out
exits.

One known limitation: **changing the game's in-game resolution while
the mod is loaded can crash the game**. Quit to the launcher before
changing resolution, then restart Farever. Fix planned for v0.6.1.
See Compatibility notes below.

If v0.6.0 crashes for you, please open an issue with `farever-mod.log`
attached.

## Which release do I download?

There are two parallel builds on the [Releases page](../../releases).
Pick once and stick with it.

* **[v0.6.0](../../releases/latest)** — the default. The stable
  rewrite of v0.5.x. Use this unless your machine cannot run it.
* **[v0.4.16](../../releases/tag/v0.4.16)** — frozen legacy build for
  users where v0.6.x cannot get the overlay up. This mostly hits
  older AMD cards with the MPO bug, very old Windows builds, or
  unusual driver configurations. v0.4.x renders directly into the
  game's swap chain and avoids the DirectComposition path entirely,
  which dodges that whole class of problem. v0.4.16 is a refresh of
  v0.4.15 against game v0.1.5.25921; feature set is much smaller (no
  DPS meter, no plugins, no collectibles).

| Feature                              | v0.6.0                        | v0.4.16                              |
| ------------------------------------ | ----------------------------- | ------------------------------------ |
| Minimap + DPS meter                  | Yes                           | Yes (older UI, fewer polish passes)  |
| Loot tracker window                  | Yes                           | No                                   |
| UI scale slider for 4K monitors      | Yes                           | No                                   |
| Lua plugin system                    | Yes                           | No                                   |
| Square minimap option                | Yes                           | No                                   |
| Composition overlay (DCOMP)          | Yes                           | No, renders on the game's swap chain |
| Works through AMD MPO / DCOMP bugs   | Sometimes, with .reg fix      | Yes, the path is not used at all     |
| Stable on post-patch game            | Yes                           | Yes                                  |

If v0.6.0 does not bring up the overlay on your machine, try v0.4.16
before opening an issue. If neither works, then open the issue and
attach `farever-mod.log` from your Farever folder.

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

* [`examples/plugins/`](examples/plugins/) — first-party reference
  plugins maintained with the mod (hello_world, personal_best,
  target_probe boss-helper, damage_planner, api_inspector, animation
  demo).
* [`community-plugins/`](community-plugins/) — submissions from
  users of the mod. Each file has the author's GitHub handle in its
  filename and a header naming the source. See that folder's
  README for the submission process.

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
trick does not bring it up either, switch to the v0.4.16 legacy
build linked above.

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

* **Native-resolution exclusive fullscreen bypasses the overlay.**
  Side-effect of how DWM presents to the display: in exclusive
  fullscreen the game's swap chain flips straight to the GPU
  scanout and DWM is not in the loop, so our composition layer is
  invisible. Use borderless windowed instead. Most monitor + GPU
  combos run borderless at essentially the same framerate as
  exclusive these days.

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

* **Alt-tab game crash since the v0.1.5.25921 patch.** Some users
  hit an access violation in the game's own DX12 renderer
  (`h3d.impl.DX12Driver.present`) after a long foreground loss
  followed by returning to the game. Pattern: alt-tab to Discord /
  browser / Steam overlay for several seconds, come back, the game
  crashes within a few frames. The mod is not the cause — when this
  happens the mod's own ticks (`damage`, `hero_state`, `overlay
  alive`) keep firing cleanly while the game's render thread dies
  — but on a few setups the timing is reproducible. Workarounds:
  hide the overlay (default F7) before alt-tabbing, switch to
  borderless windowed if you are in exclusive fullscreen, or try
  the opt-in `data/fg_detach.flag` and `data/cursor_park.flag`
  workarounds documented in the v0.5.6 release notes. Filed
  upstream as a game-side issue; nothing we can fix from the mod's
  side directly.

* **Changing in-game resolution crashes the game** (v0.6.0). The
  overlay's swap chain doesn't survive the burst of `WM_SIZE`
  messages Windows fires during a single resolution change; it ends
  up in a `DEVICE_REMOVED` state and the game's own render path
  crashes a fraction of a second later. Workaround: quit to the
  launcher before changing resolution, then restart Farever. Fix
  planned for v0.6.1.

## What's new in 0.6.0

A stability-first rewrite of how the mod reads game state. If you
were on v0.5.6.1 and hitting the recurring `DX12Driver.present`
crash, this release is for you.

The user-visible change is just "it stops crashing mid-session".
Under the hood the read path is structurally different:

* **Background reader thread.** Hero position, target tracking, cast
  timing and damage decoding all run on the mod's own thread at
  20 Hz, not on the game's render thread. That thread is deliberately
  not registered with the game's garbage collector, so the game's
  stop-the-world synchronization never waits on us — which is the
  pattern that was producing the mid-session crash.
* **Own UID registry.** The mod no longer asks the game's network
  serializer to resolve target UIDs at read time. Instead it builds
  its own `uid → entity` map from new-entity allocations, with
  per-pointer type-tag verification so dead entries can't surface a
  stale pointer.
* **Alloc-context icon resolution.** First-sight skill icons are now
  resolved on the game's main thread (during a damage event) and
  cached. The reader thread just looks up the cache, never touches
  the dispatch path that v0.5 was racing on.

Feature behaviour is the same as v0.5.6.1 — same minimap, same DPS
meter, same plugin runtime, same hotkeys. Layout and persisted files
(`farever_layout.ini`, `ui_state.json`, `keybinds.json`,
`poi_done.json`, `data/plugins/`) are untouched.

To revert to the v0.5.6.1 Present-driven path (unstable but legacy),
drop an empty `data/no_worker.flag` next to your `dinput8.dll` and
restart Farever.

Known issue: changing in-game resolution while the mod is loaded can
crash the game (see Compatibility notes). Fix planned for v0.6.1.

## What's new in 0.5.6

A plugin-author release. Big API surface bump that makes boss-helper, gear-inspector, HUD and navigation-arrow plugins buildable from Lua without further DLL work. If you don't write plugins, the only user-visible change is the **mouse-park** below; everything else is API-only and silent for everyone else.

For plugin authors:

* **Equipment + statuses + weapon read surface**. `farever.player.weapon_kind() / .weapon_level() / .weapon_upgrade()` for the currently-equipped weapon, `farever.player.equipment()` for the full loadout array, `farever.player.statuses()` for the active buffs/debuffs list. Plus a `weapon_changed` event that fires on each swap. Enough for per-weapon personal bests, gear inspectors, build-snapshot tooling.
* **Target defense getters**. `farever.target.armor() / .magic_armor() / .magic_reduction()`. Currently base-only because the live final values live in a MapData side-channel we don't decode yet, but the slot is reserved.
* **Animation surface**. `farever.now()` monotonic clock, `imgui.font_scale`, `imgui.cursor_pos`, `imgui.dummy`, and `imgui.draw_rect / draw_rect_filled / draw_circle / draw_circle_filled / draw_line / draw_text / draw_triangle / draw_triangle_filled` for custom HUD pieces (cast bars, telegraph circles, blinking alerts, navigation arrows).
* **Reference plugins** in [`examples/plugins/`](examples/plugins/): `api_inspector.lua` (living doc of every getter), `damage_planner.lua` (in-game Aragon PvE damage calculator with two-build comparison), `animation_demo.lua` (every draw primitive in one panel), `nav_arrow.lua` (tilting 3D-look waypoint arrow).
* **Community-plugins folder** at [`community-plugins/`](community-plugins/) for user-submitted Lua plugins, with attribution headers and a submission process. First entry is `poi_height-by-iskrumpie.lua` from [#36](https://github.com/ramisotti13-eng/farever-minimap/issues/36).

The user-visible change:

* **Mouse-park**. When the game has the cursor invisible (camera mode after an ALT toggle) the overlay used to still register hovers from the invisible cursor drifting over its widgets. v0.5.6 overrides ImGui's notion of the mouse to off-screen while the OS cursor is hidden, so widgets stop pretending you're touching them. Active by default, no flag needed. Does not touch the OS cursor itself — the physical cursor keeps moving normally, only the overlay's hover state is suppressed.

Two **opt-in workaround flags** for users hitting the alt-tab game crash (see Compatibility notes below): drop `data/cursor_park.flag` to clip the OS cursor to a 1-pixel box at center while invisible (heavier hand on the cursor, may interact with the game's wndproc), and `data/fg_detach.flag` to auto-detach our DCOMP visual during long foreground losses. Both are off by default; both hot-reload at ~2 Hz so you can toggle them without restarting.

## Older releases

The per-release notes on the [Releases page](../../releases) carry
the full history. Highlights:

* **0.5.6.1** — `farever.pois()` Lua API for plugin POI access.
* **0.5.5** — game v0.1.5.25921 offset-shift compat + square minimap
  option.
* **0.5.4** — plugin API for target tracking + cast bar
  (`farever.target.*`, `target_changed` / `cast_start` / `cast_end`
  events, `farever.sound()`).
* **0.5.3** — Lua plugin system, Present1 + dirty-rect FPS fix
  ([#30](https://github.com/ramisotti13-eng/farever-minimap/issues/30)),
  HWND re-validation for AMD configurations
  ([#29](https://github.com/ramisotti13-eng/farever-minimap/issues/29),
  [#31](https://github.com/ramisotti13-eng/farever-minimap/issues/31)).
* **0.5** — own-window DCOMP overlay (architectural rewrite from the
  v0.4 in-swap-chain rendering).
* **0.4** — minimap + DPS meter, the original release line. v0.4.16
  remains the legacy fallback for AMD MPO / old-Windows users.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
