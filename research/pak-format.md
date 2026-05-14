# Heaps `.pak` Format — Parsed (2026-05-14)

## Status

Solved. Parser at [`../tools/pak_walk.py`](../tools/pak_walk.py),
extractor at [`../tools/pak_extract.py`](../tools/pak_extract.py).

Full directory listing for `res.map.pak` lives in
`res.map.pak.tree.tsv` (2,295 entries, gitignored).

## Binary layout

Source: upstream Heaps' `hxd.fmt.pak.Reader.hx`.

```
Offset    Size  Field        Notes
─────────────────────────────────────────────────────────────────────
0         3     magic        ASCII "PAK"
3         1     version      u8, currently 0 for Farever
4         4     headerSize   i32 LE — absolute file offset where the
                             data section starts. The 4-byte "DATA"
                             marker sits at headerSize - 4.
8         4     dataSize     i32 LE — size of the data section
12        ...   tree         see below
hsize-4   4     "DATA"       marker
hsize     ...   raw blob     all file payloads, concatenated

Tree entry (recursive):
  nameLen   u8
  name      nameLen bytes UTF-8
  flags     u8                     bit 0 = directory, bit 1 = double-precision dataPosition
  if flags & 1 (directory):
    childCount    i32 LE
    children      childCount × tree entry
  else (file):
    dataPos       i32 LE (or f64 LE if flags & 2) — offset INTO the data section
    dataSize      i32 LE
    checksum      i32 LE
```

Absolute file offset of a leaf's payload is `headerSize + dataPos`.

Verified against `res.map.pak`: header_size = 77,824; DATA marker at
77,820; first PNG extracted starts with valid `DDS ` magic.

## What's inside `res.map.pak`

2,295 entries, all under `Level/`. Useful subsets:

| Path glob                                                | Count | Purpose                                        |
| -------------------------------------------------------- | ----: | ---------------------------------------------- |
| `Level/World/*.dat/L0_*/terrain.bin`                     |   ~  | terrain heightmaps, one per chunk              |
| `Level/World/*.dat/L0_*/decor.ddt`                       |   ~  | per-chunk decor (props) data                   |
| `Level/World/*.dat/minimap/<x>_<y>_1024.png`             |  233 | **pre-baked minimap tiles (1024×1024 DDS)**   |
| `Level/World/*.dat/minimap/zones/*.mogshape`             |  125 | **zone polygons** (Haxe-serialized `h2d.col.IPoint` arrays) |
| `Level/World/*.prefab`                                   |    1 | level prefab descriptor                        |

File extensions in use: `bin`, `ddt`, `mogshape`, `png`, `prefab`.

## Pre-baked minimap tiles — the M3 freebie

The `.png` extension is misleading; the actual bytes are **DDS** files
with FourCC `DX10`:

```
magic       = "DDS "
header_size = 124
dimensions  = 1024 × 1024
mip levels  = 11           (full mip chain, 1024→1)
pixel fmt   = DX10 extended header
```

11 mip levels means we get free LODs for zoom — no resampling work.

Tile naming pattern: `<grid_x>_<grid_y>_1024.png`. Observed grid range
for `W1_Siagarta` so far: X ∈ [-4, 6], Y ∈ [-6, 4]. World-to-tile
mapping is presumably `tile_x = floor(world_x / TILE_WORLD_SIZE)`,
`tile_y = floor(world_y / TILE_WORLD_SIZE)`. We can verify
`TILE_WORLD_SIZE` once we have live player coordinates in M2 — stand
at a known landmark and reconcile world-XY against grid coords.

Loading these from our DLL: DX12 supports DDS natively via DirectXTK
(`DirectX::CreateDDSTextureFromMemory`). Zero conversion needed.

## Zone polygon (`.mogshape`) format — Haxe Serializer

Example: `Level/World/W1_Siagarta.dat/minimap/zones/Z1_Region.mogshape`
starts with:

```
aacy14:h2d.col.IPointy1:xi-68y1:yi4gcR0R1i-58R2i4gcR0R1i-51R2i6g
```

This is Haxe's standard `haxe.Serializer` output:

- `a` = array start
- `a` = (nested array? need to confirm; sometimes the outer container)
- `cy14:h2d.col.IPoint` = class with name length 14 = "h2d.col.IPoint"
- `y1:xi-68` = field `x` (length 1) = int -68
- `y1:yi4`   = field `y` (length 1) = int 4
- `g`       = end-of-object
- `cR0R1`   = next instance, reusing class reference 0 and string ref 1
- ...etc

This is a list of integer 2D points (an IPoint polygon). We parse it
with the Haxe serialization grammar — there's a Python port floating
around (`hxserializer` on PyPI) or it's <100 lines to hand-write.

For the minimap, the polygons let us:

- Draw zone names where the player is.
- Highlight a zone outline (matching the world-map screen).
- Drive a poor-man's fog-of-war by tracking "zones the player entered".

## Implication for the plan

**M3 (map tile extraction) is now ~80% done.** What's left for M3:

- Extract all 233 tile DDS files for `W1_Siagarta` (`pak_extract.py`
  with the right glob — 1 command).
- Calibrate `TILE_WORLD_SIZE` once M2 gives us live player XY.
- Optional: extract the rest of the world's zones from the other
  `Level/World/<W>.dat/minimap/` paths if multiple worlds ship.
- Optional: write a Haxe-Serializer parser to load `.mogshape` and
  render zone outlines.
