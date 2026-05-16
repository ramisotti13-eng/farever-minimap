# Farever Minimap & DPS

![Farever Minimap](minimap.gif)
![Farever DPS meter](dpsmeter.gif)

A drop-in overlay for Farever (Shiro Games) that bundles two tools:

* **Minimap** — compass with a heading arrow, the world mosaic
  underneath, and the points of interest the game already tracks
  (obelisks, respawn points, dungeons, world activities, merchants).
* **DPS meter** — per-skill damage table that follows the in-game
  floating damage numbers, so it only counts what the game itself
  shows you.

## How to install

1. Download the latest `.zip` from the [Releases page](../../releases).
2. Extract its contents straight into your Farever folder — the one
   that contains `Farever.exe`, typically
   `C:\Program Files (x86)\Steam\steamapps\common\Farever`.
   You should end up with `dinput8.dll` and a `data\` folder sitting
   next to the game's executable.
3. Launch Farever from Steam as usual.

There is no injector to run anymore: Windows resolves `dinput8.dll`
from the game folder before its own copy, so dropping the file next
to `Farever.exe` is enough. To uninstall, delete `dinput8.dll` and
the `data\` folder.

The overlay comes up a few seconds after the title screen, once your
character has loaded. Closing the game tears everything down cleanly.

## Controls

| Key | Action |
| --- | --- |
| F8  | Show / hide the minimap |
| F10 | Show / hide the DPS meter |
| F9  | Reset the current DPS pull |

The minimap's bezel buttons, going around the rim:

* Pin: hold and drag to move the minimap around the screen
* Square: cycles three sizes (small, medium, large)
* Funnel: opens a filter panel for the icon categories
* Plus and Minus: zoom between roughly 10x and 20x
* Minus on top: shrinks the minimap to a small puck; click the puck
  to expand it again

The DPS window has a normal title-bar collapse arrow on the left so
you can shrink it to just the title row. Drag the title bar to move
the window.

## Troubleshooting

If the overlay starts hitching or you want it gone in a hurry,
F8 and F10 turn the two windows off independently. The mod also
disables itself automatically if it detects the GPU stalling on the
overlay for too long in a row, so you should not need a game
restart to recover from a bad transition.

The DPS meter only counts damage the game would normally display to
you (the floating numbers above mobs). Other party members' damage
and ambient world damage are filtered out at the source — there is
nothing to configure.

## Notes

The mod is read only. It reads your own player state out of the
game process and renders its own swap chain on top of Direct3D 12.
It does not write to game memory, it does not touch the network,
and it does not look at other players' positions or damage.

This is fan made and is not affiliated with Shiro Games. All
Farever assets remain the property of their respective owners; the
release zip bundles a subset of the game's UI textures and map
tiles for runtime display only.
