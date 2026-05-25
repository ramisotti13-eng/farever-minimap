# Community Plugins

This folder collects Lua plugins contributed by community members. They are
sandboxed Lua files just like the ones in [`examples/plugins/`](../examples/plugins/) —
drop them into `data/plugins/` of your Farever install to use one.

The first-party `examples/plugins/` folder ships reference plugins maintained
together with the mod. This folder ships plugins authored by users of the mod,
kept verbatim with their author's name and the version they were tested against.

## Installation

1. Click any `.lua` file in this folder.
2. Use GitHub's "Raw" button to view the plain Lua source.
3. Save it as `<name>.lua` into your `data/plugins/` folder.
4. The mod hot-loads it within a few seconds.

If a plugin needs a specific minimum mod version, the file header says so.

## Submitting your own plugin

Two ways to share one:

1. **Pull request** — fork the repo, drop your `<name>-by-<your-handle>.lua`
   into this folder, open a PR. Include a one-line description in the PR body.
2. **GitHub issue** — open or comment on an issue and paste the plugin code in
   a Lua code block. We'll pick it up and add it here with attribution.

Either way, please include at the top of your plugin file:

```lua
-- ==============================================================
-- <name>.lua
-- Submitted by @<your-github-handle>  (issue / PR / discord link)
-- Tested against farever-mod v0.5.x
-- License: MIT (or specify your own)
--
-- One-line description of what the plugin does.
-- ==============================================================
```

Authors retain rights to their own work; the default license is MIT unless the
file specifies otherwise.

## Quality bar

Plugins published here should:

- Run cleanly on the current released DLL (no DLL modifications required).
- Use only the documented plugin API
  ([`data/plugins/README.md`](../data/plugins/README.md)).
- Not deliberately spam the log, crash, or freeze the overlay.
- Not try to bypass the sandbox (no `os.execute`, no shelling out, no network).

The maintainer will smoke-test new submissions briefly before merging. After
that the mod owners are not responsible for plugin behaviour; if a plugin
breaks across game updates the original author is the right person to ping.

## Current submissions

| File | Author | Description | Tested against |
| ---- | ------ | ----------- | -------------- |
| [`poi_height-by-iskrumpie.lua`](poi_height-by-iskrumpie.lua) | [@iSkrumpie](https://github.com/ramisotti13-eng/farever-minimap/issues/36) | Lists nearby POIs (chests, ores, plants, activities) within a configurable radius with a per-row up/down/level arrow, XY distance and vertical offset from the player. | v0.5.5 |
| [`damage-calculator-by-iskrumpie.lua`](damage-calculator-by-iskrumpie.lua) | [@iSkrumpie](https://github.com/ramisotti13-eng/farever-minimap/pull/51) | PvE damage calculator using Aragon's verified formula: rating-to-% inputs (fervor, armor pen, crit), enemy armor presets, visual damage bars, dual-attribute scaling and a +20-rating stat-gain analysis. | v0.6.2 |
| [`mob-codex-checker-by-kaaregravesen.lua`](mob-codex-checker-by-kaaregravesen.lua) | [@KaareGravesen](https://github.com/ramisotti13-eng/farever-minimap/issues/44) | Floating widget that shows the codex completion of your current target as a red/yellow/green dot plus X/Y progress, via `farever.player.codex()`. Refreshes on the target_changed event. | v0.6.3 |
