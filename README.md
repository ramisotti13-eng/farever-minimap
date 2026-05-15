# Farever Minimap

A compass-shaped minimap overlay for Farever (Shiro Games). Shows your
position with a heading arrow, the world mosaic underneath, and the
points of interest the game already tracks (obelisks, respawn points,
dungeons, world activities, merchants).

## How to run

1. Download the latest `.zip` from the [Releases page](../../releases).
2. Extract it anywhere on your disk.
3. Launch Farever from Steam and get past the title screen so your
   character is loaded.
4. Double click `inject.exe` in the extracted folder.

A small bezel appears at the top left of the game window after a few
seconds of scanning. Closing the game tears the overlay down.

## Controls

The bezel buttons, going around the rim:

* Pin: hold and drag to move the minimap around the screen
* Square: cycles three sizes (small, medium, large)
* Funnel: opens a filter panel for the icon categories
* Plus and Minus: zoom between roughly 10x and 20x
* Minus on top: shrinks the minimap to a small puck; click the puck
  to expand it again

When you walk into a dungeon, the overlay shows a spinner while it
reconnects to your character. The minimap stays on the whole time.

## Notes

The mod is read only. It reads your own player state out of the game
process and renders its own swap chain on top of Direct3D 12. It does
not write to game memory, it does not touch the network, and it does
not look at other players.

This is fan made and is not affiliated with Shiro Games. All Farever
assets remain the property of their respective owners; the release
zip bundles a subset of the game's UI textures and map tiles for
runtime display only.
