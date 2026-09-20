# Architecture

This is the orientation document for anyone picking the mod up. It covers
how the thing is shaped, which parts are load-bearing, and where the sharp
edges are. Read it before changing anything in `src/farever-mod/`.

## The shape of it

Farever is a Haxe game compiled to HashLink bytecode. `Farever.exe` is a
small loader, `libhl.dll` is the VM, and `hlboot.dat` is the bytecode. All
the game state we care about lives as HashLink objects on the VM's heap.

The mod is a single DLL that ships as `dinput8.dll` and sits next to
`Farever.exe`. Windows resolves `dinput8.dll` from the executable's own
directory before it looks in System32, so the game loads us without anything
having to inject us. We then load the real `dinput8.dll` from System32 and
forward every export to it, so input keeps working.

From there the mod reads game state out of the HashLink heap and draws an
overlay. It never writes to game memory, never touches the network, and
never sends input. See [research/anti-cheat.md](../research/anti-cheat.md)
for the scope rules, which are deliberate and should stay.

## Boot order

1. Windows loads us as `dinput8.dll`.
2. `DllMain` ATTACH opens the log, loads the real dinput8 proxy, and spawns
   a worker thread. It does as little as possible: anything slow here
   happens while the game is still starting and shows up as a hang.
3. The worker waits for `libhl.dll` to appear, resolves the exports we need
   (`hl_alloc_obj` above all), registers the module watchers, and installs
   the hooks.
4. Before any of that takes effect, the anti-cheat gate runs. If it trips,
   the mod goes inert instead of unloading.

## Reading the game

Three mechanisms, in rough order of how much they matter.

**Type anchoring.** Every HashLink object starts with a pointer to its
`hl_type`, and every `hl_type` chain leads to the class name as UTF-16.
`type_anchor.cpp` resolves a class name to its canonical `hl_type*` once,
and from then on we can check any pointer by comparing its first 8 bytes.
This is the main defence against the garbage collector handing a freed slot
to something else while we still hold the address. Never identify an object
by the shape of its fields; always anchor on the type.

There is one thing type anchoring cannot do, and it has bitten this code
twice. `hl_alloc_obj` writes the type pointer *before* the Haxe constructor
runs, so an anchored pointer can still be a completely empty object whose
fields hold whatever the previous occupant of that memory left behind. If
you read a field straight out of an alloc watcher, you will sometimes get
convincing garbage. The fix used in `mob_watch.cpp` is to require the same
value on two consecutive passes before believing it, which is independent
of how fast the worker happens to tick.

**Alloc watchers.** `hl_hook.cpp` puts a MinHook trampoline on
`hl_alloc_obj`. A module registers a class name and a callback; the
dispatcher matches the freshly allocated object's type and calls back. This
is how damage events are caught the moment the game creates them, and how
the local Hero is found without scanning.

Two constraints. The callback runs on whatever HashLink thread did the
allocation, so keep it cheap and lock-light. And the watcher matches class
names *exactly*: registering an abstract base class gets you nothing,
because the game only ever allocates concrete subclasses. Check the class
dump before assuming a watcher will fire.

**Field offsets.** Everything else is a read at a fixed byte offset from an
anchored pointer, and this is the part that breaks on game updates. See the
migration section in [CONTRIBUTING.md](../CONTRIBUTING.md).

## The worker

`hl_pump.cpp` runs a plain Win32 thread at 40 Hz that reads state out of the
heap and publishes snapshots the render thread picks up. It is deliberately
invisible to HashLink's garbage collector: no `hl_register_thread`, no
`hl_blocking`. Earlier builds tried both the registered and the unregistered
variants of a GC-participating pump and destabilised the engine; the
GC-invisible model is what the Rust companion mod for the same game does, and
it is what works here.

The trade is that the GC can move or collect objects mid-read. Three things
absorb that: reads go through the SEH-wrapped helpers in `mem_scan.cpp` so
an access violation on a freed page is caught rather than fatal, every
cached pointer is re-checked against its anchored type, and the Hero pointer
comes from an alloc watcher rather than a scan.

Note that the Present hook on the game's swap chain runs on this same worker
thread, not on a render thread. Only alloc-watcher callbacks run on a real
HashLink thread.

## Rendering

There are two backends, chosen once at boot and not switchable within a
session. `render_mode.cpp` owns the choice; it lives in
`data/render_mode.txt` and applies on the next launch, because deciding at
boot avoids a modal racing the game's swapchain queue capture.

**Game swapchain.** `d3d12_hook.cpp` hooks Present on the game's own swap
chain and draws into it. Lower overhead, but it collides with driver-level
frame generation.

**Separate window.** `overlay_window.cpp` creates our own layered window and
swap chain composited with DirectComposition. The game's Present hook stays
installed but only to drive the per-frame ticks; it does no drawing,
no submission, and no queue capture.

ImGui is the widget layer for both. One trap worth knowing: the font atlas
has to be built during init, not lazily inside `NewFrame`, or the DX12
backend and HashLink interact badly.

## The build gate

`anticheat.cpp` runs two checks before the mod does anything. A signature
scan looks for known third-party anti-cheat modules and processes, and
repeats a few times over the first minute in case one loads late. A version
gate hashes `hlboot.dat` and matches it against `data/verified_builds.json`.

That second check has no notion of memory layout. It answers "was this build
tested with this DLL", nothing more. So the file must list only builds whose
layout matches the offsets compiled into the DLL shipped beside it, normally
exactly one. Adding a hash because a build is merely new makes the mod read
wrong values silently, which is worse than refusing to run. Silently wrong
gives the user no signal at all.

Either check tripping sets a latch rather than unloading hooks: the render
and read paths become no-ops and the mod sits there doing nothing.

## Plugins

`plugins.cpp` runs user Lua from `data/plugins/*.lua`, each in its own
sandboxed `lua_State`, on the render thread. The sandbox is built by
removing the dangerous globals (`io`, `os.execute`, `os.remove`, `os.exit`,
`package.loadlib`, `dofile`, `loadfile`, `require`) at state setup, before
any plugin code runs. Plugins get a curated read-only view of player state
and a subset of ImGui; they cannot read game memory directly or reach the
filesystem.

The API is the mod's public contract. Community plugins in the wild depend
on it, so getters get added, not renamed or removed.

## Module map

| Area | Files |
| --- | --- |
| Entry, proxy, logging | `dllmain.cpp`, `dinput8_proxy.cpp`, `log.cpp` |
| HashLink plumbing | `libhl.cpp`, `hl_hook.cpp`, `type_anchor.cpp`, `mem_scan.cpp`, `uid_registry.cpp` |
| Worker | `hl_pump.cpp` |
| Player and party | `hero_state.cpp`, `party_state.cpp`, `camera_state.cpp`, `progress_state.cpp` |
| Combat | `damage.cpp`, `heal.cpp`, `aggregator.cpp`, `skill_resolve.cpp`, `target_state.cpp`, `boss_target.cpp`, `boss_timer.cpp` |
| World | `pois.cpp`, `poi_progress.cpp`, `waypoints.cpp`, `entity_state.cpp`, `mob_watch.cpp`, `codex_state.cpp` |
| Items | `item_capture.cpp`, `loot_recon.cpp`, `cdb_atlas.cpp`, `activity_icons.cpp` |
| Rendering | `overlay.cpp`, `overlay_window.cpp`, `d3d12_hook.cpp`, `textures.cpp`, `render_mode.cpp` |
| Gate | `anticheat.cpp` |
| Scripting | `plugins.cpp` |
| Storage | `user_data.cpp` |

Headers ending in `_parse.h`, `_match.h`, `_key.h` and `waypoints_json.h`
are deliberately free of `<windows.h>` so they can be unit-tested with a
plain `cl` invocation. The tests are in `tools/test_*.cpp`. Keep that
property when you touch them.

## Where the sharp edges are

- Field offsets shift on most game updates. Nothing in the build catches it;
  a wrong offset reads a plausible number.
- A bare numeric literal in a getter survives every migration script
  unnoticed. Offsets belong in named constants.
- Equipment, inventory and currency readers produce no log output, so a
  broken migration there is invisible without the probe plugin.
- Anything read inside an alloc watcher may be an unconstructed object.
- `overlay.cpp` is by far the largest file here and is the first thing worth
  splitting if you plan to stay a while.
