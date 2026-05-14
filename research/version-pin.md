# Version Pin

Hashes of the target build. Every milestone's notes (memory patterns,
class offsets, .pak layout) is implicitly pinned to this build. Re-run
this command on a fresh build and diff before assuming notes still
apply.

## Build captured 2026-05-14

| File           | SHA-256                                                            |
| -------------- | ------------------------------------------------------------------ |
| `hlboot.dat`   | `f6cc06f0bd999e1200cf00cec7eab323590ea01d31ef5a4a43fb3dd91bc917eb` |
| `libhl.dll`    | `44461af07d0b85fa8d11d9634a497c6576613a42c801eec76925c705d5f48224` |
| `heaps.hdll`   | `72da06418e01330f42c7253a2b651e71a8b281f7a28c4326b8320bf4c228a458` |
| `Farever.exe`  | `4c13f4016cb972ede8b63bd5ba522860c0ad044eae0c3c49bc1d3f9e0cb60ef1` |

## Re-pinning command

```pwsh
Get-FileHash `
  "D:\SteamLibrary\steamapps\common\Farever\hlboot.dat", `
  "D:\SteamLibrary\steamapps\common\Farever\libhl.dll", `
  "D:\SteamLibrary\steamapps\common\Farever\heaps.hdll", `
  "D:\SteamLibrary\steamapps\common\Farever\Farever.exe" `
  -Algorithm SHA256 | Format-List
```
