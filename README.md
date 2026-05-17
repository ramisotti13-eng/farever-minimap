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

## What's new in 0.5.2

Polish pass on top of the 0.5.1 stability win. Same DCOMP
architecture, no new hooks, the AFK-AV-crash fix from 0.5 is
unchanged. This release is the UX cleanup round after a day of
people actually playing on a stable build.

* **F7 actually hides the overlay now**. In 0.5.1 F7 stopped the
  render loop but left the DCOMP visual mounted, so the last
  rendered frame stayed pinned on screen as a frozen ghost
  overlay. 0.5.2 detaches the DCOMP visual on hide
  (`SetContent(nullptr)`) and re-attaches on show, so F7 actually
  hides for real. Caught by kesmese during the v0.5.1 AFK test.

* **Reset window positions hotkey** (default Home, rebindable).
  If a window ends up somewhere you cannot grab it (different
  monitor, behind chat, off the edge after a resolution change),
  press Home and all four windows (minimap, DPS meter, hotkeys,
  fight detail) snap back to their default positions. Works on
  the stored ImGui state via SetWindowPos by name, so it resets
  windows that are currently closed too. Driven by a user who
  dragged the minimap off-screen and had to reinstall the mod to
  get it back.

* **RMB auto-clickthrough**. While the right mouse button is held
  (camera mode in Farever), the overlay temporarily behaves as if
  clickthrough were on, so camera drag never gets eaten by an
  ImGui window the cursor happens to be over. Releases cleanly on
  RMB up. Cuts down on the "wait my camera is stuck" moments when
  the cursor passes over a hidden bezel button mid-rotate.

* **F7 is a real rebindable keybind now**. v0.5 hard-coded F7 in
  the render thread. 0.5.2 routes it through the normal keybind
  table, so it shows up in the Hotkeys panel and can be rebound
  to whatever you want, persisted to `keybinds.json`.

* **Window opacity slider for the non-compass windows**. Added to
  the bezel filter tablet next to the existing Minimap opacity,
  this one affects the DPS Meter, Hotkeys and Fight Detail
  backgrounds. Range 0.30 to 1.00, persisted to `ui_state.json`.
  Compass has its own opacity already.

* **Log rotation**. Boot rotates `farever-mod.log` to `.log.1`
  (and the previous `.1` to `.2`). If you reproduce a crash and
  restart the game before uploading, the original log is now in
  `farever-mod.log.1` instead of being overwritten.

* **Auto-hide when the hero lock drops**. Server kick, game quit,
  disconnect: the overlay detaches automatically. Re-attaches
  when a fresh hero lock comes back. Fixes kesmese's "stuck on
  character select" report after the 40-minute AFK kick. Note:
  switching characters via the main menu does not drop the lock
  because Farever keeps the old Hero object live in memory, so
  the overlay stays visible there for a moment until the new
  character spawns in the world. Press F7 if you want to hide it
  during the menu sit.

* **Lock stability gate before first attach**. The DCOMP visual
  no longer attaches on the very first hero-lock event, instead
  waits for the lock to stay green for one full second of
  consecutive checks. Stops the brief overlay flicker that
  happened during zone transitions when the lock blinked off and
  back on within a frame or two.

* **Dungeon self-heal**. If the DCOMP attach state and lock state
  get out of sync for a few seconds (visual attached but lock
  gone, or lock present but visual missing), the render thread
  forces a re-attach. Mostly covers the case where re-entering
  the world after a dungeon transition would leave the overlay
  frozen on the old frame.

* **Kill-switch flag reading removed from DllMain**. The
  `no_overlay`, `no_hl_tick`, `anticrash` and `no_d3d12` flags
  were diagnostic levers from the v0.4.13 to v0.5 bisection saga,
  which is done. The setter functions in `overlay.cpp` and
  `hero_state.cpp` remain as no-op shims so a stale `data\*.flag`
  file from an older install is just ignored instead of
  surprising anyone.

### Default hotkeys after 0.5.2

| Key  | Action |
| ---- | ------ |
| F7   | Toggle overlay (show / hide entirely) |
| F8   | Toggle minimap |
| F9   | Reset current DPS pull |
| F10  | Toggle DPS meter |
| F11  | Toggle click-through (mouse passes to game) |
| F12  | Pause DPS tracking |
| Home | Reset window positions |

All rebindable in the Hotkeys window.

### Known compatibility notes

* **Not compatible with RivaTuner Statistics Server (RTSS) on
  the same game**, including MSI Afterburner's OSD which uses
  RTSS underneath. Two overlay injection paths into the same
  D3D12 device race and one of them eventually trips a GPU
  device-removed (DXGI `0x887A0005`). Workaround: in RTSS set
  "Application detection level" to None for `Farever.exe`, or
  use Steam's built-in FPS counter instead, which is much more
  compatible with composition overlays.

* **FPS is capped to your monitor refresh rate while the overlay
  is active.** Side-effect of how DirectComposition mounts on
  the game window: it pulls the game out of independent-flip
  presentation into DWM-composed presentation, which syncs to
  the display refresh. For most players this matches what vsync
  would do anyway (60 Hz = 60 FPS, 144 Hz = 144 FPS, etc).
  If you want uncapped FPS for input latency at the cost of
  the overlay, toggle the mod off with F7.

## What's new in 0.5.1

Five small follow-ups to the 0.5 architectural rewrite, all
addressing UX and compat issues that surfaced once users had a
stable build to play on:

* **Hotkeys window now shows the bound keys again** ([#17](https://github.com/ramisotti13-eng/farever-minimap/issues/17)).
  The labels were empty in 0.5 due to a dangling-pointer bug
  (`key_to_name(vk).c_str()` returned a pointer into a temporary
  std::string that died at the end of the expression). Bound to a
  named local now.

* **Smart click-through** ([#7](https://github.com/ramisotti13-eng/farever-minimap/issues/7)).
  Clicks on the compass mosaic, empty DPS table cells, and other
  decorative overlay pixels now pass through to the game — you can
  attack a mob standing behind your compass. Clicks on actual
  interactive widgets (bezel buttons, title bars, fight-history
  rows) still get captured by the overlay so dragging and toggling
  work as before. Implementation: the wndproc check switched from
  ImGui's coarse `io.WantCaptureMouse` (any window) to
  `IsAnyItemHovered() || IsAnyItemActive()` (only interactive
  items).

* **F7 / F11 split**. 0.5 inadvertently bound F11 to both overlay
  show/hide (in the render thread) and the legacy click-through
  toggle (in the wndproc). Same keypress hit both paths. Split:
  **F7 = overlay show/hide**, **F11 = click-through toggle** (back
  to its pre-0.5 behaviour).

* **Audio no longer chops on game quit** ([#18](https://github.com/ramisotti13-eng/farever-minimap/issues/18) part 2).
  Pre-0.5.1 DllMain DETACH did a minimal shutdown and left the
  overlay-window render thread to be force-killed at process exit,
  which released GPU resources hard and stuttered the system audio
  driver for 5-10 s. 0.5.1 cleanly stops the render thread + frees
  D3D12/DCOMP resources before the process dies.

* **Steam notifications don't glitch any more during character
  select** ([#18](https://github.com/ramisotti13-eng/farever-minimap/issues/18) part 1).
  The DCOMP visual was getting created at game-boot, racing
  Steam's notification animation engine. 0.5.1 waits for the first
  hero lock before bringing the visual up. By then character
  select is over and Steam's notifications have finished their
  lifecycle naturally.

## What's new in 0.5

**This is the release where the recurring `DX12Driver.present line 3306` AV crash is actually fixed**, after a full day of incremental versions and bisection on the affected users' logs (see [#11](https://github.com/ramisotti13-eng/farever-minimap/issues/11) / [#12](https://github.com/ramisotti13-eng/farever-minimap/issues/12) / [#16](https://github.com/ramisotti13-eng/farever-minimap/issues/16)).

### The fix

The overlay no longer renders into the game's swap chain at all. Pre-0.5 we hooked three D3D12 vtable functions on the game's swap chain (`Present`, `ResizeBuffers`, `ExecuteCommandLists`) so we could submit our ImGui content on the game's command queue. The bisection chain in v0.4.13 → v0.4.17 proved those vtable patches are the AV trigger (the patches' *presence* is enough; even when we suppressed all our submissions the patch sites alone destabilised the game over minutes).

0.5 keeps only **one** D3D12 hook: `Present`, used purely as a HashLink-thread tick driver (`damage_tick` + `hero_state_tick` must run on the render thread). It does no rendering, no submission, no queue capture. The overlay instead runs in its own D3D12 device + command queue + composition swap chain, and the swap chain is mounted as a DirectComposition visual onto the **game's** window. DWM composites our visual over the game's render output, so the user still sees a single layered image — but the game's swap chain is never touched for rendering.

### What changed for you

* **F11** toggles the overlay rendering on/off (was the click-through hotkey pre-0.5; we don't need a click-through toggle any more because the overlay doesn't have a window of its own and mouse routing goes naturally through to the game).
* The overlay auto-resizes when the game window changes size (Farever boots in an 800x600 loading window then grows to your monitor's full size, and full-screen toggling now works too).
* The combat-state dot in the DPS header is drawn as a circle instead of the unicode `●` glyph that the Karla font lacked (it rendered as `?` for users).
* If your build was crashing on 0.4.x with that AV, please delete every flag file from `data/` (`no_overlay.flag` / `no_hl_tick.flag` / `anticrash.flag` / `no_d3d12.flag` — they were all opt-in diagnostic levers during the bisection), drop in the new `dinput8.dll`, and play normally. If you weren't crashing, 0.5 should still feel identical except for the auto-resize and the combat-dot fix.

### Known limitations in 0.5

* Exclusive-fullscreen mode disables DWM composition, so the overlay won't appear if you switch the game to true fullscreen (not borderless). Farever's default is borderless windowed.
* The kill-switch flag files (`no_overlay.flag` etc.) still exist in the binary as diagnostic levers but should no longer be needed. They'll likely be removed in 0.6.

## What's new in 0.4.15

v0.4.14's throttling wasn't enough. Retests on the affected hardware
([#11 kesmese](https://github.com/ramisotti13-eng/farever-minimap/issues/11)
12 min alt-tabbed, [#16 Blox](https://github.com/ramisotti13-eng/farever-minimap/issues/16)
3 min in combat) both crashed again with the same `DX12Driver.present`
AV. Critically, neither log shows a SEH trip or our auto-disable line.
That means the AV is **not** inside our code-path (our `__try` would
catch it), it's inside the game's own DX12 path, triggered by the
cumulative overhead of our `hl_alloc_obj` MinHook trampoline itself.
The throttle reduces how much work the dispatcher does on each call,
but the trampoline cost is paid on every game allocation regardless.

The only way out is to actually remove the trampoline once we don't
strictly need it. 0.4.15 adds an opt-in mode for the affected users:

**Anticrash mode**: drop an empty file at `data/anticrash.flag`. On
launch the mod prints `worker: kill switches ... anticrash=ARMED`,
acquires the Hero lock normally, then after 5 seconds of uninterrupted
lock surgically removes the `hl_alloc_obj` hook entirely and stops the
damage tracker. The game's HashLink allocator goes back to running
with zero overhead from us. From that point on:

* The minimap keeps working. `hero_state` switches to polling
  `Player.hero` (the back-reference from the cached Player) every
  ~70 ms (4 frames) to detect zone transitions.
* The DPS meter stops collecting events (the alloc-hook that fed it
  is gone). The window itself still draws but won't update past what
  was already in flight.
* A small amber-then-green diagnostic status box appears top-left:
  amber "anticrash ARMED" while it waits for lock stable, then green
  "anticrash DISARMED, alloc-hook removed" once the hook is gone.
* Log line `hero_state: anticrash trigger at tick N` tells you the
  exact frame the hook came off.

The trade-off is real: you trade DPS tracking for stability. If you
weren't using the DPS meter much, this is a clean win. If you need
DPS and don't hit the AV, leave the flag off and 0.4.15 behaves
exactly like 0.4.14.

To turn it back off, delete the flag file and restart the game.

For everyone else: 0.4.15 is the same as 0.4.14, just with the
opt-in escape hatch added.

## What's new in 0.4.14

The crash-bisection answer arrived.

Two independent test runs on 0.4.13 with the kill switches told
the same story. Blox [#16 boot 1](https://github.com/ramisotti13-eng/farever-minimap/issues/16) with `no_overlay.flag` active
(zero D3D submissions from us) crashed at the usual `DX12Driver.present`
AV after 18 minutes of active combat. kesmese [#11](https://github.com/ramisotti13-eng/farever-minimap/issues/11) with `no_hl_tick.flag`
active (zero HashLink-side reads from us) ran 43 minutes cleanly
before the game's own session timeout took him to character
select. Symmetric. Not the render pipeline, the always-on
HashLink read path is the trigger.

0.4.14 hardens that path in six concrete ways.

* **`hero_state_tick` drain capped at 8 entries per frame** (was
  unbounded). City scenes with many remote players entering range
  used to fire `is_local_hero` checks on dozens of fresh allocs in
  the same frame.
* **`on_hero_alloc` queue capped at 8 entries when already locked**
  (was 256). The pending queue exists to catch zone-transition
  re-locks, not to buffer every remote-player allocation.
* **`hero_state_tick` `publish()` throttled to every 4th frame**
  (15 Hz position update). Still completely smooth on the compass,
  but the per-frame read cost drops about 75 %.
* **`damage_tick` only drains on even frames**. Halves the
  per-frame pressure from the damage pipeline; the DamageDisplay
  retry buffer absorbs the one-frame staggered delay invisibly.
* **Type-tag check on the locked Hero before reads**. The type
  pointer at offset +0 gets compared against the learned `hl_type`
  for `ent.Hero`, and the lock is dropped on mismatch. Cheap guard
  against Boehm GC reusing a dead Hero's memory slot for a
  different class.
* **`__try` / `__except` wrap on both ticks with auto-disable
  after 5 consecutive failures**. Mirrors the overlay's
  auto-disable safety net: if our reads keep tripping access
  violations the module shuts itself down so the game stays alive.
  Log line `auto-disabling module to keep the game alive` tells
  you what happened.

Also: a small **diagnostic status box** (top-left, amber border)
appears whenever a kill switch is engaged so you can visually
confirm the mod is alive. Issue #16's boot 2 retest was abandoned
because the user saw an empty screen and thought the mod had
failed to load. That won't happen any more.

If your build was stable on 0.4.13, install 0.4.14 and use the
mod normally. **No flag files**, **no env vars** — those were
diagnostic only. If you were hitting the recurring
`DX12Driver.present` AV, please remove any `no_overlay.flag` /
`no_hl_tick.flag` from `data/` and retest with the throttling
active. If it still hits, attach the new log; the
`auto-disabling module` line (if present) tells me which side
is still tripping and where to dig next.

## What's new in 0.4.13

Two unrelated things.

**Issue #15**: the minimap's padlock was also locking the DPS /
fight-history window, which made no sense -- you'd lock the
minimap so it stops drifting during combat and then couldn't move
the DPS window any more. The DPS window now has its own lock
flag, toggled by the padlock icon at the start of its status line.
The minimap bezel padlock controls only the minimap. Both states
are persisted independently in `data\ui_state.json`.

**Issues #12 / #16**: the v0.4.12 skeleton-mode log proved the
draw-volume hypothesis wrong. The crash hit at tick 4200 with
`submitted=0` in the heartbeat counter, meaning the AV happened
while our overlay was still paused and we hadn't sent a single
command list to the game queue. The only thing we were still
doing at that moment was the HL-side read pipeline
(`hl_alloc_obj` watcher + `damage_tick` + `hero_state_tick` on
the render thread). v0.4.13 adds two kill switches so affected
users can bisect:

```
no_overlay  -- skip ImGui init + D3D submit. HL reads still run.
no_hl_tick  -- skip damage + hero_state watchers. Overlay still
               runs (will be mostly empty).
```

**To engage a switch**: in the `data` folder next to `dinput8.dll`,
create an empty file with the matching name and the `.flag`
extension. From PowerShell:

```pwsh
New-Item "<your Farever folder>/data/no_overlay.flag" -ItemType File
```

or in Explorer: right-click in the `data` folder, New, Text
Document, then rename to exactly `no_overlay.flag` (with the dot,
no `.txt`). Launch the game from Steam as usual. To turn the
switch off, delete the file. Both flags can exist at once.

The mod log gets a line `worker: kill switches overlay=OFF
hl_tick=ON` right after boot so you can confirm the switch is
engaged, and the overlay emits a `killed @ tick N` heartbeat
every 600 ticks while suppressed so the log proves the Present
hook is firing.

(There are also matching env vars `FAREVER_NO_OVERLAY=1` /
`FAREVER_NO_HL_TICK=1` for users launching `Farever.exe` directly,
but Steam was already running with its own environment block when
you ran your .bat, so `set ... + start steam://run/<appid>` will
**not** propagate the var to the game. Use the file flag with
Steam.)

Useful only for users hitting the recurring `DX12Driver.present`
AV. If your build is stable, do not enable these.

## What's new in 0.4.12

Follow-up for issue #12. The 5 s pause from v0.4.10 delayed the
crash by exactly the extra pause time but the AV still hit roughly
9 s after our overlay actually started submitting. So the trigger
isn't a fixed time after hero lock; it's a fixed time after we
start rendering on top. That points at the draw-call volume on the
affected hardware -- the minimap dumps 1000+ AddImage / AddCircle
calls into ImGui on the first full frame (mosaic + every POI).

0.4.12 adds a **skeleton phase** after the existing pause. For 10
seconds after a hero lock or large position jump, the minimap
renders only the bezel ring + player arrow + buttons -- no mosaic
image, no POI markers. The full minimap snaps in after that.
Total quiet-time after a transition is ~5 s pause + ~10 s skeleton
= 15 s before the heavy draw kicks in. From the user's side this
looks like an empty compass for the first quarter-minute after a
zone change, then the map content arrives.

If the affected user's setup tolerates the skeleton phase, we know
the heavy ImGui draw is the trigger and can fix it properly --
e.g. spread POI rendering across frames or pre-bake the POI layer.
If the crash still hits in skeleton mode, the bug lives deeper
than ImGui geometry and we need a different mitigation.

## What's new in 0.4.11

Bug fix for click-through (issue #14). The v0.4.7 implementation
flipped the flag in `ui_state.json` correctly but ImGui still
received and reacted to mouse buttons through our wndproc -- so
clicks on the minimap zoom buttons, the fight history rows etc.
still fired even with click-through ON.

The wndproc now skips ImGui's mouse handler entirely for button
and wheel messages when click-through is on, so the overlay
genuinely does not react. Mouse moves still flow through so
hover visuals (POI hover scaling, button highlights) continue
working.

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
