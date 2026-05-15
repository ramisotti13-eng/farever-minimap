# Farever Minimap

A minimap overlay for Shiro Games' Farever. The base game ships a full
world map screen but no live HUD minimap, so this fills that gap. It
draws a compass-shaped window on top of the running game showing the
world mosaic, your current position with a heading arrow, and the
points of interest (obelisks, respawn points, dungeons, world activities,
merchants) that the game already tracks internally.

## How to use it

Launch Farever and get to your character (anywhere past the title
screen is fine). Then double click `inject.exe` once. A small bezel
with a yellow arrow appears top-left of the game window after a few
seconds of scanning.

The bezel has a handful of buttons around its rim:

* Pin: hold and drag to move the window around
* Square: cycles three sizes (small, medium, large)
* Funnel: opens the filter panel for the icon categories
* Plus and Minus: zoom between roughly 10x and 20x
* Minus on top: shrinks the whole minimap to a small puck so it gets
  out of the way; click the puck to expand it again

When you walk into a dungeon (or any sub level the game streams in)
the mod loses its position lock for a moment, shows a spinner, and
reconnects on its own once you are back in the overworld. The scan
takes a bit because the game's heap is large, but the minimap stays
on screen the whole time.

## What it does and does not touch

Farever is an online game, so the mod is read only on purpose.

It reads your own player state out of the game process and renders its
own swap chain on top of Direct3D 12. It does not write to game memory,
it does not touch the network, it does not look at other players'
state, and it does not modify or redistribute any of the game's data
files. The icons and the tiles drawn on the map are loaded from the
game's existing `.pak` files at runtime; nothing from those files is
bundled here.

## Building from source

You need a recent Windows 11 with the MSVC build tools and CMake.

```pwsh
cmake -B build -S src
cmake --build build --config RelWithDebInfo --target minimap
cmake --build build --config RelWithDebInfo --target inject
```

The build produces `build\minimap-dll\RelWithDebInfo\minimap.dll` and
`build\injector\RelWithDebInfo\inject.exe`.

Asset extraction (run once, after a game update or when you first
clone the repo):

```pwsh
cd tools
python extract_pois.py
python extract_icons.py
```

That pulls the POI list and the icon atlas out of `res.pak` into the
`research\` folder, where the DLL expects to find them.

## Project layout

```
docs\        engine notes, milestones, architectural decisions
research\    mosaics, icons, POI lists, calibration JSON
src\         the C++ side
  injector\  inject.exe
  minimap-dll\  the DLL itself
tools\       Python scripts: pak parsing, bytecode parsing, calibration
```

The DLL's two important runtime files are
`research\minimap_calibration.json` (the affine transform from game
world coordinates to mosaic pixels, hot reloaded every frame) and
`research\pois_W1_Siagarta.json` (the list of points of interest with
their world coordinates).

## Status

The minimap works end to end:

* Compass display in game with player marker and yaw arrow
* In process Hero scan, with auto reconnect across dungeon transitions
* Calibrated affine transform from world coordinates to mosaic
* 161 POIs across the W1 world rendered with the game's own icons
* Filter UI for the POI categories
* Pin, resize, zoom, collapse buttons on the bezel

Still to do is multi world support (right now everything is keyed to
the W1 Siagarta map) and a cleaner installer.

## Credits and licence

Built with [Dear ImGui](https://github.com/ocornut/imgui) and
[MinHook](https://github.com/TsudaKageyu/minhook).

The mod is not affiliated with Shiro Games. All Farever assets remain
the property of their respective owners; this repository contains no
game data, only the code that reads from the user's local copy at
runtime.
