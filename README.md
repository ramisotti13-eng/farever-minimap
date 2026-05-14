# Farever Minimap Mod

An on-HUD minimap overlay for **Farever** (Shiro Games, HashLink + Heaps,
Steam AppID 3672400). The base game ships a full world map screen with a
POI system but no live minimap; this mod adds one as a strictly read-only
overlay.

## Status

| Phase   | What's done                                                                 |
| ------- | --------------------------------------------------------------------------- |
| **M0**  | Engine + assets fully reverse-engineered                                    |
| **M1**  | Python pipeline: assets → tools → FOW viewer (offline)                      |
| **M2**  | HashLink bytecode parser; structural memory scan for Hero candidates        |
| **M3.1**| Kiero-style D3D12 vtable hook (Present / ResizeBuffers / ExecuteCommandLists) |
| **M3.2**| ImGui-DX12 overlay window rendering in-game                                 |
| **M3.3**| World mosaic loaded as a D3D12 texture, drawn with `ImGui::Image`           |
| **M3.4**| Live local-player marker, with read-only-safe local-Hero pick via `isMe`    |
| **M3.5**| ↻ calibration of the game ↔ mosaic transform (next session)                 |

End-to-end on the current build:

```pwsh
# 1) Launch Farever and get in-world.
# 2) From an Admin PowerShell:
cd D:\farevermod\tools
python find_me.py --loop
# (~75 s scan, then live polling — writes research\live_position.json)

# 3) In a second window, inject the overlay DLL:
D:\farevermod\build\injector\RelWithDebInfo\inject.exe `
    D:\farevermod\build\minimap-dll\RelWithDebInfo\minimap.dll
```

The "minimap v0.1" ImGui window appears in-game showing the world
mosaic, current world coordinates, and a yellow player marker that
moves with the local character. Marker position on the mosaic is
still using placeholder calibration constants — fitting those to
real landmarks is M3.5.

## Repository layout

```
D:\farevermod\
├── README.md                         this file
├── docs\                             plan + architectural decisions
│   ├── plan.md                       milestones, risks, what's next
│   ├── engine-analysis.md            HashLink + Heaps + D3D12 stack
│   └── approach-comparison.md        DLL overlay vs bytecode patch vs hdll
├── research\                         findings from the reverse-engineering pass
│   ├── hero-layout.md                ★ exact memory offsets for ent.Hero
│   ├── pak-format.md                 Heaps .pak container format
│   ├── types.md                      class graph (mostly superseded by classes.json)
│   ├── anti-cheat.md                 verified: no AC; constraint: read-only only
│   ├── version-pin.md                file SHA-256s for the captured build
│   ├── maps\                         stitched mosaics + zone overlays
│   ├── live_position.json            written every frame by find_hero.py loop
│   ├── fow_state.json                persistent discovered-zones for the viewer
│   └── fow_calibration.json          viewer's manual transform (game ↔ polygon)
├── tools\                            ★ all the Python tools, runnable today
│   ├── hlbc_parse.py                 HashLink 1.15 bytecode parser → classes.json
│   ├── find_me.py                    ★ find the LOCAL Hero (isMe filter, see memory note)
│   ├── find_hero.py                  Hero signature scanner + position pump (debug only)
│   ├── fow_viewer.py                 interactive FOW viewer with calibration
│   ├── stitch_minimap.py             extract + stitch the 107 minimap tiles
│   ├── render_zones.py               render zone polygons standalone / overlaid
│   ├── pak_walk.py, pak_extract.py   Heaps .pak parser + extractor
│   ├── hxserializer.py               Haxe Serializer text-format parser (.mogshape)
│   ├── probe.py                      memory probe (attach + anchor scan + refs/dump)
│   ├── snapdiff.py                   snapshot+diff for hunting changing values
│   ├── verify_stable.py              filter candidates by stable live values
│   ├── extract_strings.py            tiny `strings(1)` replacement
│   └── hashlink\                     official HashLink 1.15 redist (hl.exe)
└── src\                              C++ native side
    ├── CMakeLists.txt                FetchContent: MinHook + ImGui-DX12
    ├── README.md                     build instructions
    ├── injector\                     inject.exe (CreateRemoteThread + LoadLibraryW)
    └── minimap-dll\                  minimap.dll: D3D12 hook + ImGui overlay
                                       (d3d12_hook, overlay, textures, live_position)
```

## Critical constraint: read-only

Farever is an online multiplayer game. The mod is constrained to **read
own player state and render an overlay**. No writes to game memory, no
network packets touched, no other players' positions read, no
modification or distribution of `hlboot.dat`. See
[`research/anti-cheat.md`](research/anti-cheat.md).

## Key findings (the parts most worth remembering)

- **`ent.Hero` is 1584 bytes**, with `posx/posy/posz` at offsets
  `+144/+152/+160` as `HF64` (doubles), and `rotationZ` (yaw) at `+168`.
  See [`research/hero-layout.md`](research/hero-layout.md).
- **The local player is picked via `isMe`.** A raw structural scan
  matches every Hero (yours plus every nearby remote player). The
  read-only-safe way to lock onto your own character is
  `Hero.ownerPlayer (st.Player) .isMe == 1` plus the bidirectional
  check that `Player.hero` points back at the Hero we entered through.
  Implemented in `tools/find_me.py`.
- **The game already ships pre-baked minimap tiles**: 107 × 1024² DDS
  (BC7) under `Level/World/W1_Siagarta.dat/minimap/<x>_<y>_1024.png`
  inside `res.map.pak`. Plus 125 Haxe-serialized zone polygons.
- **`hlbc_parse.py` works on Farever's bytecode** even though
  upstream `hlbc 0.5.0` doesn't (it errors on HPACKED, kind 22).
- **The Hero stays stable across the session**; its address is found
  by structural fingerprint scan rather than by following pointer
  chains.
