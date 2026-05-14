# minimap-dll + injector

Native side of the Farever minimap mod. Two CMake targets:

| Target      | Output         | Purpose                                                                             |
| ----------- | -------------- | ----------------------------------------------------------------------------------- |
| `minimap`   | `minimap.dll`  | Loaded into Farever.exe. Hooks D3D12 Present and draws the minimap overlay.         |
| `inject`    | `inject.exe`   | Standalone tool that injects `minimap.dll` into a running Farever process.          |

## Current status (2026-05-14)

- ✅ CMake scaffolding with FetchContent (MinHook + ImGui pulled at configure time)
- ✅ `inject.exe` skeleton: classic CreateRemoteThread(LoadLibraryW)
- ✅ `minimap.dll` skeleton: log file + live_position poll thread
- ❌ D3D12 Present hook — TODO
- ❌ ImGui DX12 backend wiring — TODO
- ❌ Actual minimap render — TODO

The current DLL doesn't draw anything in-game. It just validates that
injection works and that the live-position pipeline (Python →
`research/live_position.json` → DLL background thread) is functional.

## Build

Requires Visual Studio 2022 + CMake 3.20+. From `D:\farevermod`:

```pwsh
cmake -S src -B build -A x64
cmake --build build --config RelWithDebInfo
```

Outputs:
- `build\minimap-dll\RelWithDebInfo\minimap.dll`
- `build\injector\RelWithDebInfo\inject.exe`

## Run

1. Start Farever, get in-world.
2. Start the Python position pump: `python tools/find_hero.py loop`
3. Inject: `build\injector\RelWithDebInfo\inject.exe`
4. Watch `D:\farevermod\minimap.log` for DLL activity.

## Roadmap (left to do)

| Milestone                                             | Where                          |
| ----------------------------------------------------- | ------------------------------ |
| Kiero-style D3D12 vtable hook (Present + ExecuteCmdLists) | `minimap-dll/d3d12_hook.cpp` |
| ImGui DX12 backend init / shutdown                    | `minimap-dll/overlay.cpp`      |
| Load mosaic PNG + zone polygons at startup            | `minimap-dll/assets.cpp`       |
| Project world XY → minimap UV using calibration       | `minimap-dll/transform.cpp`    |
| FOW mask: cell grid OR game-state mirror              | `minimap-dll/fow.cpp`          |
| UI: zoom, north-up vs camera-rot, toggle hotkey       | `minimap-dll/overlay.cpp`      |

## Why injector instead of DXGI proxy

A `dxgi.dll` proxy auto-loads on game start but has to forward
hundreds of DXGI exports (or whitelist the ones the game calls).
An injector is uglier UX but keeps the DLL build clean and lets us
iterate without re-shipping the proxy table on each release.
We can ship a proxy later if it becomes important.
