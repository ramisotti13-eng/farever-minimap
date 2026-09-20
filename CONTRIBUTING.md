# Contributing

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) first. This file is the
practical half: how to build it, how to survive a game update, and what the
project will not accept.

## Building

You need Visual Studio 2022 or newer with the C++ workload, and CMake 3.20 or
newer. Everything else is fetched by CMake.

```
cmake -S src -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config RelWithDebInfo --parallel
```

Substitute your own generator name if you are on an older Visual Studio
(`"Visual Studio 17 2022"`). Two artifacts come out:

```
build/farever-mod/RelWithDebInfo/dinput8.dll
build/injector/RelWithDebInfo/inject.exe
```

To run it, copy `dinput8.dll` next to `Farever.exe` in the game folder. The
game loads it on the next launch. The injector is only needed for the older
manual-injection path; the shipped mod does not use it.

The third-party dependencies are pinned to exact revisions in
`src/CMakeLists.txt`, not to branch names. That is deliberate: ImGui changed
its DX12 backend's `Init` signature after v1.92.9b, and an unpinned `docking`
checkout stops compiling against this code. If you bump a pin, rebuild and
smoke-test in the actual game before shipping anything.

## Unit tests

The headers ending in `_parse.h`, `_match.h`, `_key.h`, plus
`waypoints_json.h`, are free of `<windows.h>` on purpose so they compile
standalone. Each has a test under `tools/test_*.cpp` with its exact build
command in the first comment. Keep that property: if a pure header starts
needing Windows, the test dies with it.

## Surviving a game update

This is the main maintenance burden and the reason the project needs someone
watching it. Farever re-lays-out its memory on most updates. Every field the
mod reads is a byte offset, and a stale offset does not crash, it returns a
plausible wrong number.

Work through this in order.

**1. Dump the new class layout.**

```
python tools/hlbc_parse.py "<Steam>/steamapps/common/Farever/hlboot.dat" --out classes_new.json
```

Keep the previous build's dump too; you need both. These files are around
35 MB each and are not committed, since they are derived from the game.

**2. Diff every class, not just the ones you suspect.** Compare field
offsets per class between the two dumps. The useful signal is the pattern:
in the 2026-09-11 update, for instance, every class descending from
`st.DBState` gained one field, so everything below that point shifted by
eight bytes while `ent.*`, `ui.*` and `hxbit.*` did not move at all.

**3. Resolve each constant by field name.** Never apply a blanket delta and
never eyeball the shift. `tools/migrate_v023_v024.py` is the worked example:
for each named constant it looks up which field sits at the current offset
in the old dump, then reports where that same *name* sits in the new one.
Copy it, point it at your two dumps, and update the query list.

**4. Apply with an exactly-one-match assertion.**
`tools/apply_v024_offsets.py` shows the shape. If a constant's old value
appears more than once in a file, stop and fix it by hand rather than
letting a blind replacement run.

**5. Grep for bare numbers.** A literal like `hero + 1272` written inline
instead of as a named constant survives every migration script invisibly and
silently breaks one getter. This has happened. After applying offsets, search
the diff for numeric literals near pointer arithmetic.

**6. Verify against reality.** Some readers announce themselves in
`farever-mod.log` and some do not.

Progress and skill resolution show up in the log, so you can compare bit
indices and resolved names against an older log from a working build. But
equipment, inventory and currencies produce *no log output at all*. The only
way to check those is through the plugin API: drop `tools/offset_check.lua`
into `data/plugins/`, run the game, and read its verdict line. If you skip
this step, a broken gear reader ships and nobody notices for a week.

**7. Update the build whitelist.** Put the new `hlboot.dat` hash into
`data/verified_builds.json` and take the old one **out**. Keep two entries
only when a full per-class diff proves nothing the mod reads has moved. The
gate is a plain hash match with no notion of layout, so a stale entry means
the mod will happily run against a client it was never migrated for.

**8. Bump the version banner and verify it.** The string lives in
`overlay.cpp` (search for `farever-mod v`). After building, search the
compiled DLL for the version bytes and confirm it actually changed. A release
once shipped with the previous version's banner because this was skipped.

## Releasing

`tools/pack_v1_2_8_release.py` is the template. It builds the new zip from
the previous one, swapping the DLL and the whitelist, and then verifies the
result: entry count, the top-level folder name, the DLL hash, and that path
separators are forward slashes. That last one matters, because backslash
separators break extraction for players running through Proton.

Never publish a binary that has not been run in the game first. Not once.

## Scope

The rules in [research/anti-cheat.md](research/anti-cheat.md) are the
project's boundary, and they are the reason it has been tolerated. To be
concrete about what will not be merged:

- memory writes of any kind
- packet reading, interception or injection
- input automation
- anything ESP-like: other players' positions, mobs through walls

The mod reads its own client's state and draws it more usefully. That is the
whole remit.

## Style

Comments explain why a thing is the way it is, especially when it looks
wrong. Most of the odd-looking code here is odd because a simpler version was
tried and broke something; when you fix something subtle, write down what you
ruled out. That is what makes this codebase maintainable by the next person.

The plugin API is a public contract. Community plugins depend on it, so
getters get added, not renamed or removed.
