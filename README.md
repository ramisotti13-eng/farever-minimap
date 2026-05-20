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
* **Plugin runtime** (new in 0.5.3): drop your own Lua scripts into
  `data/plugins/` and the mod loads them sandboxed, with hot reload
  on save. See [plugin authoring guide](data/plugins/README.md).

## Which release do I download?

There are two parallel builds on the [Releases page](../../releases).
Pick once and stick with it.

* **[v0.5.3.2](../../releases/latest)** — the main, actively developed
  build. Use this unless your machine cannot run it.
* **[v0.4.15](../../releases/tag/v0.4.15)** — a frozen legacy build
  for users where v0.5.x cannot get the overlay up. This mostly hits
  older AMD cards with the MPO bug, very old Windows builds, or
  unusual driver configurations. v0.4.x renders directly into the
  game's swap chain and avoids the DirectComposition path entirely,
  which dodges that whole class of problem.

| Feature                              | v0.5.3.2                      | v0.4.15                              |
| ------------------------------------ | ----------------------------- | ------------------------------------ |
| Minimap + DPS meter                  | Yes                           | Yes (older UI, fewer polish passes)  |
| Loot tracker window                  | Yes                           | No                                   |
| UI scale slider for 4K monitors      | Yes                           | No                                   |
| Lua plugin system                    | Yes                           | No                                   |
| Composition overlay (DCOMP)          | Yes                           | No, renders on the game's swap chain |
| Works through AMD MPO / DCOMP bugs   | Sometimes, with .reg fix      | Yes, the path is not used at all     |
| Known long-session access violation  | No                            | Possible after long AFK DPS farming  |

If v0.5.3.2 does not bring up the overlay on your machine, try v0.4.15
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

## Plugin runtime (v0.5.3+)

Drop a `.lua` file into `data/plugins/` and the mod loads it
automatically. The folder ships empty. Two example plugins live in
the repo at [`examples/plugins/`](examples/plugins/) as opt-in
downloads: a minimal "hello world" and a personal-best DPS tracker
that listens for `fight_end` events and persists across sessions.

Plugins get sandboxed Lua 5.4. They can read your player position,
DPS, in-combat flag and fight events. They can draw their own
ImGui window with text, buttons, sliders, checkboxes, color
pickers, combos and progress bars. They can show centered toast
notifications and persist per-plugin state to disk. They cannot
touch game memory, network, the filesystem outside their own
state, or other plugins' state. A bad plugin only crashes itself,
the mod keeps running.

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

If the overlay never appears at all on v0.5.3, check
`farever-mod.log` for `all composition swap chain variants failed`.
That is the AMD MPO interaction, and v0.5.2.3 onwards drops three
auto-repair files into your Farever folder when this happens:
`farever-fix-amd-overlay.reg` (double-click, accept the prompt,
reboot), `farever-undo-amd-overlay-fix.reg` (rollback), and
`OVERLAY_NOT_WORKING.txt` (plain-English step-by-step). If the .reg
trick does not bring it up either, switch to the v0.4.15 legacy
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

## What's new in 0.5.3.2

A bug-fix release. The foe tracker introduced in 0.5.3.1 caused a crash a few seconds after the hero locks on some setups. It is removed in 0.5.3.2 along with the `farever.foes.*` table that depended on it. A new foe tracker will return in a later release once the read path is rebuilt in isolation.

Everything else from 0.5.3.1 stays in:

* **Full Hero attribute surface for plugins**. The Hero exposed only
  position, heading, lock state and the in-combat flag in v0.5.3.
  v0.5.3.1 follows the `Hero.attr` pointer chase and surfaces ~50
  more fields via `farever.player.*`: HP / max HP / energy / shield,
  primary stats (vitality / strength / dexterity / faith / intellect),
  crit / penetration / fervor / dodge / cooldown reduction, armor /
  magic armor / magic reduction, move speed, damage / heal modifiers,
  and the Hero-only class resources (rage / spark / focus / combo
  point / poise / oxygen / glide). Two batched memory reads per frame
  populate one snapshot; the API stays a simple `farever.player.X()`
  getter set.

Plus a handful of defensive fixes that fell out of a 0.5.3.1 code
review: HeroAttributes type-tag check before reading the Hero-only
block, mutex around the snapshot, atomic `farever.store.set` writes
(no more wiped personal_best on crash), bounded event / toast queues,
larger `imgui.input_text` buffer, and a `data/no_plugins.flag`
escape hatch.

Plugin authoring guide at
[`data/plugins/README.md`](data/plugins/README.md).

## What's new in 0.5.3

Three things on top of 0.5.2.3:

* **Lua plugin system**. Drop `.lua` files into `data/plugins/`,
  the mod loads them sandboxed with hot reload. Plugin hooks:
  `on_init`, `on_render` (in an ImGui window the mod opens), and
  `on_event(name, data)` for `hero_locked`, `fight_start`,
  `damage_dealt`, `fight_end`. Read APIs for player position and
  DPS, ImGui widget bindings, persistent per-plugin store, toast
  notifications. Lua 5.4 statically linked, about 300 KB added to
  the DLL. Full authoring guide at
  [`data/plugins/README.md`](data/plugins/README.md).

* **FPS fix for ultrawide and high-resolution overlays**
  ([#30](https://github.com/ramisotti13-eng/farever-minimap/issues/30)).
  Several users on 3440x1440 / 4070 Super class hardware reported
  the game FPS dropping in half while the overlay was visible.
  Root cause was DWM re-compositing a full-game-window DCOMP layer
  on every game-Present. Fixed by computing a tight dirty rect from
  the ImGui draw commands each frame and passing it to
  `IDXGISwapChain1::Present1`, so DWM only re-composites the small
  region where the UI actually lives instead of the full 20 MB
  transparent buffer.

* **HWND re-validation**
  ([#29](https://github.com/ramisotti13-eng/farever-minimap/issues/29),
  [#31](https://github.com/ramisotti13-eng/farever-minimap/issues/31)).
  Two AMD users hit a state where the overlay never came up because
  the game destroyed and recreated its top-level window during the
  hero-lock wait, and the mod was still pointing at the cached
  boot-time HWND. v0.5.3 now validates `IsWindow` + a plausibility-
  bounded `GetClientRect` before DCOMP setup, and re-enumerates the
  game window with retries when the cached handle is stale.

## Changelog

See the [Releases page](../../releases) for the full version
history (0.1 through 0.5.3). Highlights of recent versions are kept
in this README; everything older lives in the per-release notes.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
