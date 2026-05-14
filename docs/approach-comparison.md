# Approach Comparison

Three plausible architectures for adding a minimap. We pick (A) as the
primary path and keep (B) as a fallback.

## (A) External D3D12 Overlay DLL — **chosen**

```
  Farever.exe ── memory read ──► minimap.dll
       │                              │
       └── D3D12 Present hook ────────┘
                       │
                       ▼
            ImGui-DX12 minimap overlay
```

- DLL injected via `dxgi.dll` proxy in the game folder, or via a manual
  injector during development.
- Hooks `IDXGISwapChain::Present` (and `ResizeBuffers`) with MinHook.
- Reads player XYZ + current zone ID from process memory using AOB
  (Array-of-Bytes) pattern scanning.
- Draws an ImGui window: background = pre-extracted zone map texture,
  player arrow + POI icons rendered on top.

**Pros**

- Zero game files modified → safe with Steam, survives game updates
  better, easy to uninstall.
- Tooling is mature: MinHook + ImGui-DX12 are standard.
- Fast iteration: just rebuild and re-inject.

**Cons**

- AOB patterns can break on game patches; need a versioning scheme.
- D3D12 hooks are fiddlier than D3D11 (multiple command queues, frame
  resources) — but well-documented and Kiero handles most of it.
- Steam Overlay also hooks D3D12; order of hooking matters.

## (B) Native HashLink bytecode patch — **fallback**

```
  hlboot.dat ──► hl --dump ──► dump.txt
       ▲                            │
       │                            ▼
  re-pack ◄── patch with Haxe-compiled new minimap class
```

- Decompile `hlboot.dat` to identify the UI/HUD setup function.
- Write minimap logic in Haxe, compile with `haxe --hl out.hl`.
- Splice the new function + a call site into `hlboot.dat`.

**Pros**

- Native integration — uses Heaps directly, no D3D12 reverse work.
- Player position / POIs are already typed Haxe objects, no pattern
  scanning needed.
- Performance is "free" — runs inside the existing render loop.

**Cons**

- Requires Haxe + HashLink toolchain expertise.
- Patching `hlboot.dat` likely violates Steam ToS for distribution
  (modified game files). Personal use only, or distribute as a binary
  diff/patcher that the user runs locally.
- Game updates rewrite `hlboot.dat` → patch must be re-applied each time.

## (C) Custom .hdll injection

- Add native functions in a new `minimap.hdll`, then bytecode-patch
  `hlboot.dat` to call them.
- Combines the worst of (A) and (B): still need bytecode patching AND
  a custom DLL. Skip unless (A) and (B) both fail.

## Decision

**Start with (A).** It's the cleanest from a distribution perspective and
matches how every other Heaps/HashLink mod scene works.

Fall back to (B) only if:

- Memory patterns prove too unstable, or
- Drawing on top of D3D12 turns out to interact badly with the game's
  own UI (e.g., Steam Overlay conflicts).
