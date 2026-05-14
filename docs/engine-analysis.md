# Engine Analysis

## Confirmed stack

| Layer        | Component                                    | Evidence                                          |
| ------------ | -------------------------------------------- | ------------------------------------------------- |
| Launcher     | `Farever.exe` (~278 KB)                      | Tiny — just a HashLink loader                     |
| VM           | `libhl.dll` (HashLink runtime)               | File present                                      |
| Bytecode     | `hlboot.dat` (~13 MB)                        | Magic `HLB\x04` → HashLink bytecode format v4     |
| Engine       | `heaps.hdll` (Heaps.io)                      | Open source: <https://github.com/HeapsIO/heaps>   |
| Rendering    | `dx12.hdll` + `D3D12/D3D12Core.dll`          | DirectX 12 backend                                |
| UI binding   | `ui.hdll`                                    | Heaps' UI module                                  |
| Audio        | `fmod.dll` + `hlfmod.hdll` + `hlwwise.hdll`  | FMOD Studio + Wwise                               |
| Input        | `SDL2.dll` + `sdl.hdll`                      | SDL2                                              |
| Networking   | `mysql.hdll`, `ssl.hdll`, `uv.hdll`          | libuv-based net stack                             |
| Steam        | `steam.hdll` + `steam_api64.dll`             | Steamworks SDK                                    |
| Assets       | `res.pak` (5.1 GB), `res.map.pak` (415 MB), `res.levels.pak` (82 MB), `res.light.pak` (5.5 MB) | Heaps custom `.pak` archive |
| Save data    | `save/heropath/*.heropath`                   | Naming: `WorldW1Siagarta_<player>_<timestamp>`, `POIZ1LevelsZ1POIDungeon...` — confirms zone/POI naming scheme |

## What the existence of these settings tells us

From `options.ini`:

```
FilterObelisks = true
FilterRespawnPoints = true
FilterSuggestedActivities = true
FilterDiscoveredActivities = true
FilterCompletedActivities = true
Magnifier = false
MagnifierZoomAmount = 2
```

Implications:

- The game **already has** a world map screen with multiple POI categories.
- Player position, zone identifier, and POI lists must all live in memory
  while playing, since the existing map renders them.
- A "Magnifier" accessibility overlay already exists — proves the engine
  can composite UI elements on top of the gameplay view.

We therefore do **not** need to invent any new game data. The mod is a
**read-and-display** problem, not a "simulate map" problem.

## HashLink + Heaps modding ecosystem

- HashLink is open source (BSD-2): <https://github.com/HaxeFoundation/hashlink>
- The official `hl` binary can disassemble bytecode:
  `hl --dump hlboot.dat > dump.txt` → human-readable opcode listing with
  type info, field names, and string constants.
- Heaps source is on GitHub — we know the public APIs (e.g.
  `h2d.Object`, `h2d.Bitmap`, `h2d.Scene`, the `Res` macros).
- Community precedent: Dead Cells (also HashLink + Heaps) has a thriving
  mod scene that combines bytecode patches and asset replacement.
- Hashlink's `.hdll` files are plain Windows DLLs that export `hl_init`
  + a list of native functions; replacing/wrapping one is straightforward.

## Anti-cheat / online status

Farever appears to be single-player (no multiplayer servers seen in the
binaries, despite `mysql.hdll`/`ssl.hdll`/Steam being present). **Verify
before injecting**:

1. Check the Steam store page for VAC / EAC / BattlEye notices.
2. Run a no-mod session and look for a process named `BEService.exe`,
   `EasyAntiCheat.exe`, etc.
3. If clean, single-player injection is low-risk. If not — abort and
   switch to an offline-only/snapshot-based approach.
