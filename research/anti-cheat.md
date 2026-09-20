# Anti-Cheat & Online Status

Verified 2026-05-14 (Farever Early Access build, Steam AppID 3672400).

| Check                                         | Result                                                |
| --------------------------------------------- | ----------------------------------------------------- |
| Steam store page mentions an anti-cheat       | No (Steam requires disclosure → none shipped).        |
| AC binaries in game folder (EAC/BattlEye/...) | None.                                                 |
| AC processes spawned with the game            | None observed via Task Manager.                       |
| Steam VAC                                     | Not VAC-secured (single-player + light MMO).          |
| Game type per Steam page                      | **Online multiplayer action RPG (MMO + Co-op).**      |
| Developer / Publisher                         | Shiro Games (Northgard, Evoland, Wartales).           |
| Network stack                                 | `mysql.hdll`, `ssl.hdll`, `uv.hdll`, `steam.hdll`     |

## What this means for scope

No process-level cheat detection → the technical risk of injecting a
DLL is low. **But the game is online**, so the risk model is
ToS-based, not AC-based. That gives three tiers, and the mod stays
inside the first one.

**Safe, and all this mod does.** Read your own player position, yaw and
zone, plus the POIs the game already shows you on the world map. Draw
the result on our own swapchain. Same shape as a WoW UI addon or a PoE
loot filter: the information was already on your screen, we only put it
somewhere more useful.

**Grey zone, deliberately skipped.** Anything ESP-like, meaning other
players' positions or seeing mobs through walls. Technically reachable
from the same memory we already read, and left alone on purpose.

**Hard no.** Memory writes, packet interception, input automation, or a
modified `hlboot.dat` in an online session. None of these exist in the
code and none of them should be added. A pull request that adds one
will not be merged.

## Procedure for each new build

1. Re-check the Steam store page for an AC disclosure update.
2. Launch the game once, list child processes - bail if anything new.
3. Re-pin `hlboot.dat` SHA-256 in [`version-pin.md`](version-pin.md).
4. Re-run `tools/hlbc_parse.py` to refresh the class dump (field
   offsets shift when the studio adds/removes class fields).
