# `ent.Hero` Memory Layout — Static Analysis (2026-05-14)

## Critical: pick the LOCAL Hero, not the first scanning hit

Farever is an MMO. A structural scan over the heap will match every
Hero in the client — yours plus every nearby remote player. To pick
your own:

1. For each Hero candidate at address `H`, read `ownerPlayer` (HOBJ)
   at `H + 16` → `st.Player` address `P`.
2. Bidirectional sanity: `*(P + 272)` (`st.Player.hero`) must equal
   `H`. Without this, random heap blocks with byte 1 at +280 pass.
3. `*(P + 280)` is `st.Player.isMe` (HBOOL). `== 1` means the local
   player.

Implementation: `tools/find_me.py`. Never inject and poll a Hero
address picked purely by motion / structural score on an online
server — that reliably tracks a remote player.


Extracted from `hlboot.dat` via `tools/hlbc_parse.py`. The file has
9,125 HOBJ/HSTRUCT classes; this doc captures the layouts that matter
for the minimap mod.

Full dump: `tools/classes.json` (gitignored, ~12 MB).

## `ent.Hero` — type #1375, instance size 1584 bytes

Inherits → `ent.Unit` → `ent.GameObject` → `st.State` (Heaps' networked
state) → ultimately doesn't inherit `h3d.scene.Object` directly. Its
own `obj` field at offset 208 points to the visual `h3d.scene.Object`.

**Fields we care about for the minimap:**

| Offset | Size | Type           | Name        | Notes                                  |
| -----: | ---: | -------------- | ----------- | -------------------------------------- |
|      0 |    8 | ptr            | (hl_type)   | first qword of every instance          |
|      8 |    1 | HBOOL          | removed     | 0 = alive, 1 = pending destroy         |
|     16 |    8 | HOBJ st.Player | ownerPlayer | local player has self-referencing chain |
|     32 |    8 | HI64           | __uid       | unique entity id                       |
|     48 |    8 | HOBJ           | __host      | non-null on live instances             |
|     88 |    8 | HOBJ           | layer       | parent `st.GameLayer`                  |
|  **144** | **8** | **HF64**     | **posx**    | **★ world X (double precision)**      |
|  **152** | **8** | **HF64**     | **posy**    | **★ world Y**                          |
|  **160** | **8** | **HF64**     | **posz**    | **★ world Z (height)**                 |
|  **168** | **8** | **HF64**     | **rotationZ** | **★ yaw, radians**                  |
|    176 |    8 | HOBJ h3d.VectorImpl | position | cached vector copy of posx/y/z   |
|    208 |    8 | HOBJ h3d.scene.Object | obj   | visual model in scene graph     |
|    288 |    8 | HOBJ client.AnimPlayer | anim |                                  |
|    440 |    8 | HENUM          | goState     | `ent.GameObjectState`                  |

The 4 doubles at 144-176 form an extremely specific signature for
identifying live Hero instances in memory (see
[`../tools/find_hero.py`](../tools/find_hero.py)).

## Related classes

| Class                       | Type # | Size  | Use                                          |
| --------------------------- | -----: | ----: | -------------------------------------------- |
| `ent.Unit`                  |   1122 |  1168 | Hero's super                                 |
| `ent.GameObject`            |    ?   |   ~1000 | Unit's super                                |
| `client.PlayerController`   |   2426 |   200 | offset 8 → `unit : ent.Unit` (= the Hero)    |
| `st.Player`                 |   1367 |   352 | local player; offset 200 → `heroData`        |
| `client.GameCamera`         |   1439 |   304 | yaw at h3d.scene.Object inherited x/y/z      |
| `world.World`               |    883 |   392 | the world scene root                         |
| `h3d.scene.Object`          |    468 |   160 | base for all Heaps spatial objects           |

## h3d.scene.Object inherited fields

These appear on Hero indirectly via its `obj` field (offset 208), and
on World, Camera, etc.:

| Offset | Size | Field    | Notes                |
| -----: | ---: | -------- | -------------------- |
|      0 |    8 | hl_type  |                      |
|      8 |    4 | flags    |                      |
|     16 |    8 | currentAnimation |              |
|     24 |    8 | children | `hl.types.ArrayObj`  |
|     32 |    8 | parent   |                      |
|     48 |    8 | x        | HF64 (LOCAL pos)     |
|     56 |    8 | y        | HF64                 |
|     64 |    8 | z        | HF64                 |
|     72 |    8 | scaleX   | HF64                 |

NB: the `obj` (at Hero+208) holds the visual mesh's transform. Its x/y/z
at offset 48/56/64 are the LOCAL pos (parent-relative). For world-space
position, always use Hero.posx/y/z at +144/152/160.

## Why our M1.5 candidate failed

The vec4 we found at `0x000002af9e4aec70` (4 floats: x, y, z, w=1)
was the `worldPos` of an `h2d.col.PointImpl` or similar f32 marker
position attached to the live `PlayerMarker` UI object. It was driven
by `Hero.posx/y/z` (read as doubles, then converted down to floats for
the UI). When the world map closed, the PlayerMarker got GC'd and its
worldPos vec4 was freed. The Hero itself is never GC'd while playing.

The Hero scanner (`find_hero.py`) bypasses the marker entirely and
finds the Hero by its structural fingerprint, so it works regardless
of the map state.

## Next-session workflow

1. Launch the game, get to in-world (not the main menu).
2. `python find_hero.py` — scan for Hero candidates, get a list.
3. Pick the top-scoring one (the local player is typically score ≥ 1.5).
4. `python find_hero.py loop <addr>` — pump the position into
   `live_position.json` every 100 ms.
5. `python fow_viewer.py` — viewer reads the JSON and renders the
   live player marker.
