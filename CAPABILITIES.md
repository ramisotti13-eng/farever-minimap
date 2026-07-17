# farever-mod capabilities

This document describes, in plain terms, what the `dinput8.dll` in this mod does
at the process level: what it reads, what it writes, what it inspects, and what
it does not do. It is written for users who want to understand the binary they
are running rather than take a hash on trust. It describes the current release
(v1.2.2, DLL sha256 `ba3bef131fa6ba13842853aa05729074c28c7b57bd5c316c3e3002711138e585`);
the behaviour below is stable across recent versions.

## What it is

`dinput8.dll` is a DirectInput proxy. You drop it into the Farever game folder,
Windows loads it into the game process on start, and it draws an overlay
(minimap, DPS meter, target and party panels) by reading live values out of the
running game. It is read-only with respect to the game: it reads game memory to
display information and never writes to it.

Because it is named `dinput8.dll`, it also forwards the real DirectInput. On
load it locates the genuine `dinput8.dll` and passes through all five of its
exports (`DirectInput8Create`, `DllCanUnloadNow`, `DllGetClassObject`,
`DllRegisterServer`, `DllUnregisterServer`), so the game's input keeps working
exactly as before.

## What it reads

- The game's own data files it needs to draw the map: map previews, icons,
  icon atlases, and point-of-interest JSON, all from the Farever `data` folder.
- `hlboot.dat` (the game's HashLink bootstrap) to compute a SHA-256 and compare
  it against the mod's list of known-good game builds (`data/verified_builds.json`).
- Live game memory, read-only, to pull the values the overlay shows (player
  position, health, DPS, target, party, and so on).
- Its own configuration and plugin files (see below).

## What it writes

Every file the mod writes lives either inside the Farever game folder or under
`%LOCALAPPDATA%\farever-minimap\`. It writes:

- `ui_state.json`, `keybinds.json`: overlay settings and key bindings.
- `fight_history.json`, per-character boss-timer and point-of-interest progress
  profiles: DPS and progress state.
- `render_mode.txt` and a small marker file: first-run and render-backend state.
- `waypoints.json`: user-placed map waypoints.
- Plugin persistent stores (`data/plugins/<name>.store.lua`) and combat logs
  under `%LOCALAPPDATA%\farever-minimap\combatlogs\`, both written on behalf of
  Lua plugins through a controlled API (filenames are restricted to a single
  safe component, with a size cap).
- `farever-mod.log`: a plain-text diagnostic log in the Farever folder.
- An optional AMD overlay-fix helper: when you ask for it, the mod writes a
  `.reg` file and a readme `.txt` into the Farever folder for you to run
  yourself. The mod does not apply them; double-clicking the `.reg` file (with
  the usual Windows UAC prompt) is what changes anything, and an undo `.reg`
  file is written alongside it.

## What it inspects

The only thing it inspects outside its own process is the list of running
process names, once, to detect whether an anti-cheat is present and show the
safety popup you have seen. It uses `CreateToolhelp32Snapshot` to read process
and module names and `OpenProcess` with `PROCESS_QUERY_LIMITED_INFORMATION`
(the most limited query right). It does not read or write the memory of any
other process.

## What it does not do

- No network connections of its own. The binary imports no networking APIs
  (no WinHTTP, WinINet, or Winsock), so it does not phone home, upload, or
  download anything.
- No registry reads or writes. The binary calls no registry APIs at all. The
  only registry change that can happen is a `.reg` file you choose to run
  yourself (see the AMD fix above).
- No writing to game memory. All game reads are read-only.

## Notes on imported Windows APIs

If you inspect the import table you will see a couple of entries worth
explaining:

- `SHELL32.dll`: used for `SHGetKnownFolderPath` and `SHCreateDirectoryExW`,
  which resolve and create the `%LOCALAPPDATA%\farever-minimap\` folder for
  config and logs.
- `ShellExecuteW`: this import is pulled in by Dear ImGui, the UI library the
  overlay is built on. ImGui's default "open a link in the shell" handler on
  Windows is implemented with `ShellExecuteW`. The mod's own code does not call
  it.

## Verifying the binary yourself

- Every release lists the SHA-256 of both the release zip and the extracted
  `dinput8.dll`. Compare the file you have against the release notes.
- Run the DLL through https://www.virustotal.com/ to see how it scores against
  around 70 engines at once. As an unsigned, not-widely-downloaded native DLL it
  can draw a heuristic false positive from an engine or two; the point of the
  multi-engine view is that you are not relying on any single vendor.

  The v1.2.2 DLL was submitted and came back clean, 0 of 69 engines flagged it
  (checked 2026-07-17):
  https://www.virustotal.com/gui/file/ba3bef131fa6ba13842853aa05729074c28c7b57bd5c316c3e3002711138e585
