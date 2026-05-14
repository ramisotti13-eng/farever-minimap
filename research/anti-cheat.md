# Anti-Cheat & Online Status

Verified 2026-05-14 (Farever Early Access build, Steam AppID 3672400).

| Check                                         | Result                                                |
| --------------------------------------------- | ----------------------------------------------------- |
| Steam store page mentions an anti-cheat       | No (Steam requires disclosure → none shipped).        |
| AC binaries in game folder (EAC/BattlEye/...) | None.                                                 |
| AC processes spawned with the game            | None observed via Task Manager.                       |
| Steam VAC                                     | Not VAC-secured (single-player + light MMO).          |
| Game type per Steam page                      | **Online multiplayer action RPG (MMO + Co-op).**      |
| Developer / Publisher                         | Shiro Games (Dead Cells, Northgard, Evoland devs).    |
| Network stack                                 | `mysql.hdll`, `ssl.hdll`, `uv.hdll`, `steam.hdll`     |

## What this means for scope

No process-level cheat detection → the technical risk of injecting a
DLL is low. **But the game is online**, so the risk model is
ToS-based, not AC-based:

- ✅ **Safe (read-only overlay):** read own player position, yaw, zone,
      POIs already on the world map. Draw on our own swapchain.
      Same shape as WoW UI addons / PoE loot filters.
- ⚠️ **Grey zone:** ESP-like features (other players' positions,
      seeing mobs through walls). Skip.
- 🚫 **Hard no:** memory writes, packet interception, input automation,
      modified `hlboot.dat` in an online session.

## Procedure for each new build

1. Re-check the Steam store page for an AC disclosure update.
2. Launch the game once, list child processes — bail if anything new.
3. Re-pin `hlboot.dat` SHA-256 in [`version-pin.md`](version-pin.md).
4. Re-run `tools/hlbc_parse.py` to refresh `classes.json` (field
   offsets shift when the studio adds/removes class fields).
