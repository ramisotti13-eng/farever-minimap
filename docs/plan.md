# Farever Minimap Mod — Plan

## Context

Farever (Shiro Games, AppID 3672400) ships without an on-HUD minimap.
Navigation in open zones is painful and the game already tracks every
data point we'd need — player position, current zone, POI lists for
obelisks, respawn points, activities. The user also explicitly wants
the minimap **gated by discovery** (only reveal areas they've already
been in), matching the world-map screen's behavior.

## Threat model

Farever is an **online MMO**, not single-player as initially assumed.
Steam AC field shows no anti-cheat; no AC processes appear at runtime.
Mod scope is strictly read-only:

- ✅ Read own player position, yaw, current zone
- ✅ Read POI lists already shown on the world-map screen
- ✅ Render an overlay over our own swapchain
- 🚫 Write game memory, intercept network, automate input
- 🚫 Read other players' positions (ESP grey zone)
- 🚫 Modify or redistribute `hlboot.dat`

See [`../research/anti-cheat.md`](../research/anti-cheat.md).

## Architecture

External `minimap.dll` injected into the game process:

1. **Position pump** (Python): scans memory for the Hero by structural
   signature → writes `live_position.json` every 100 ms.
2. **DLL** (C++): hooks D3D12 Present, reads the JSON, renders ImGui
   minimap with mosaic + zone polygons + player marker + FOW.

See [`approach-comparison.md`](approach-comparison.md) for the
"why-not-a-bytecode-patch" rationale.

## Milestones

### M0 — Reconnaissance ✅

Engine: HashLink + Heaps + D3D12. Bytecode `hlboot.dat` is 13 MB,
HLB version 4. Three `.pak` files: `res.pak` (5.1 GB) + `res.map.pak`
(435 MB) + `res.levels.pak` (82 MB). No anti-cheat. Save files reveal
zone naming scheme (`WorldW1Siagarta_*`).

### M1 — Pipeline end-to-end (Python) ✅

| Sub | Result                                                                       |
| --- | ---------------------------------------------------------------------------- |
| M1.0 | All 107 W1_Siagarta tiles extracted + stitched (`W1_Siagarta.mosaic.png`, 11264²) |
| M1.1 | All 125 zone polygons parsed (`hxserializer.py` for the `.mogshape` format) |
| M1.2 | `probe.py` attaches to the live game, finds anchor strings reliably         |
| M1.3 | C++ DLL skeleton builds (`inject.exe` + `minimap.dll`) — no render yet       |
| M1.4 | Tkinter FOW viewer with mouse-walk + zone-reveal + persistent state          |
| M1.5 | Live player position read from memory (vec4 marker), updates every frame    |

### M2 — Memory access ✅

| Sub | Result                                                                                   |
| --- | ---------------------------------------------------------------------------------------- |
| M2.0 | `hlbc_parse.py` — own HashLink bytecode parser. Dumps 9 125 classes with field offsets. |
| M2.1 | `find_hero.py` — scans heap for the 1584-byte `ent.Hero` signature. Position at +144.   |

### M3 — In-game D3D12 overlay

| Sub  | Result                                                                      |
| ---- | --------------------------------------------------------------------------- |
| M3.1 | ✅ Kiero-style D3D12 vtable hook on Present (slot 8), ResizeBuffers (13), ExecuteCommandLists (10). Hook install on background thread (loader-lock safety). |
| M3.2 | ✅ ImGui-DX12 backend rendering an overlay window inside the game. Font atlas pre-built at init (lazy build inside `ImGui::NewFrame` reliably stack-overflows HashLink's main thread, see memory note). |
| M3.3 | ✅ World mosaic loaded as an R8G8B8A8 D3D12 texture (WIC decode, UPLOAD→DEFAULT heap, transient queue), drawn with `ImGui::Image`. SRV slot 0 = ImGui font, slot 1 = mosaic. |
| M3.4 | ✅ Live local-player marker. `tools/find_me.py` locks onto the local Hero via `Hero.ownerPlayer.isMe` plus bidirectional `Player.hero == Hero` check — required to avoid reading remote players (Farever is an MMO; structural scan finds every nearby player). DLL polls `live_position.json` and draws a yellow dot + yaw line. |
| M3.5 | ↻ Calibrate the game ↔ mosaic affine transform from real landmarks. Current `g_calib` in `overlay.cpp` is placeholder. |
| M3.6 | ↻ Re-enable ImGui-Win32 backend so the overlay accepts input (toggle, drag, zoom). Was disabled while debugging the font-atlas crash. |
| M3.7 | ↻ Configuration UI (toggle, opacity, zoom, north-up). |
| M3.8 | ↻ Steam Overlay interop check (Steam also hooks D3D12). |
| M3.9 | ↻ Distribution / installer. |

### M4 — POIs

Read the in-memory POI lists (game already tracks
`FilterObelisks`, `FilterRespawnPoints`, etc. — see options.ini) and
render them as marker sprites on top of the minimap.

## Risks

| Risk                                                              | Likelihood | Mitigation                                                                              |
| ----------------------------------------------------------------- | ---------- | --------------------------------------------------------------------------------------- |
| Server-side ToS enforcement against client overlays               | Medium     | Stay strictly read-only. No state mutation, no network touching, no ESP.                |
| Steam Overlay + our hook conflict over D3D12                      | Medium     | Hook after Steam, detect-and-chain. Worst case: ship a Steam-Overlay-off mode.          |
| Game update changes `ent.Hero` field offsets                      | High over time | Externalize layout to `hero-layout.json`; ship updates with a re-run of `hlbc_parse.py`. |
| Coordinate calibration drifts between zones                       | Low        | Game uses a single world coord system per zone; verify per-region in M3.                |

## Verification

End-to-end on each milestone:

1. Drop dll + injector + tools into the Farever folder.
2. Launch from Steam → in-world.
3. `python tools/find_hero.py loop` — see live position update.
4. (M3+) `inject.exe` → confirm overlay shows + no crash.
5. Walk a known path, confirm the player arrow follows.
6. Open the world map, confirm POIs match in placement and filter state.
7. Alt-tab + return, confirm overlay survives device reset.
8. Repeat with Steam Overlay both on and off.
