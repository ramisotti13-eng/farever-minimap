"""Extract every minimap tile for a world inside res.map.pak and stitch
them into one big mosaic PNG.

Each tile is a 1024x1024 BC7-compressed DDS (despite the .png filename
extension) at path:

    Level/World/<world>.dat/minimap/<grid_x>_<grid_y>_1024.png

The filename's grid coordinates index a regular 1024-px tile grid; we
arrange them in a Pillow canvas accordingly. Two outputs:

  <out_dir>/<world>.mosaic.png         full-resolution (potentially huge)
  <out_dir>/<world>.preview.png        downscaled to MAX_PREVIEW_PX

Tiles are loaded and pasted one at a time so we never hold more than
one decoded tile in RAM beyond the canvas itself.

Usage:
    python stitch_minimap.py <res.map.pak> <world_name> <out_dir>

Example:
    python stitch_minimap.py res.map.pak W1_Siagarta D:/farevermod/research/maps
"""
from __future__ import annotations

import io
import re
import sys
from pathlib import Path

from PIL import Image

from pak_walk import _R, read_entry  # type: ignore[import-not-found]
from pak_extract import open_pak     # type: ignore[import-not-found]

TILE_PX = 1024
MAX_PREVIEW_PX = 4096

TILE_RX = re.compile(r"minimap/(-?\d+)_(-?\d+)_1024\.png$")


def collect_tiles(entries, world: str):
    """Return list of (grid_x, grid_y, size, dataPos) for the given world."""
    prefix = f"Level/World/{world}.dat/"
    tiles = []
    for path, size, pos in entries:
        if not path.startswith(prefix):
            continue
        m = TILE_RX.search(path)
        if not m:
            continue
        gx, gy = int(m.group(1)), int(m.group(2))
        tiles.append((gx, gy, size, pos))
    return tiles


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    pak = Path(sys.argv[1])
    world = sys.argv[2]
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"opening {pak.name} ...")
    buf, entries, data_start = open_pak(pak)

    tiles = collect_tiles(entries, world)
    if not tiles:
        print(f"no minimap tiles found for world {world!r}", file=sys.stderr)
        return 1

    xs = [t[0] for t in tiles]
    ys = [t[1] for t in tiles]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    cols = max_x - min_x + 1
    rows = max_y - min_y + 1
    print(f"  found {len(tiles)} tiles, grid x:[{min_x}..{max_x}] y:[{min_y}..{max_y}]")
    print(f"  mosaic = {cols * TILE_PX} x {rows * TILE_PX} px")

    # The original guess was that tile filename Y points northward
    # (so we'd flip when placing on a screen-Y-down canvas). User
    # reports the result is wrong — try the un-flipped placement.
    canvas = Image.new("RGBA", (cols * TILE_PX, rows * TILE_PX), (0, 0, 0, 255))

    for i, (gx, gy, size, pos) in enumerate(sorted(tiles), 1):
        data = buf[data_start + pos : data_start + pos + size]
        img = Image.open(io.BytesIO(data))
        img.load()
        px = (gx - min_x) * TILE_PX
        py = (gy - min_y) * TILE_PX
        canvas.paste(img, (px, py))
        if i % 20 == 0 or i == len(tiles):
            print(f"  pasted {i}/{len(tiles)}")

    out_full = out_dir / f"{world}.mosaic.png"
    print(f"writing {out_full} ...")
    canvas.save(out_full, optimize=False, compress_level=1)

    long_side = max(canvas.size)
    if long_side > MAX_PREVIEW_PX:
        scale = MAX_PREVIEW_PX / long_side
        preview_size = (round(canvas.size[0] * scale), round(canvas.size[1] * scale))
        preview = canvas.resize(preview_size, Image.LANCZOS)
        out_prev = out_dir / f"{world}.preview.png"
        preview.save(out_prev)
        print(f"writing {out_prev} ({preview_size[0]}x{preview_size[1]}) ...")

    metadata = out_dir / f"{world}.mosaic.json"
    metadata.write_text(
        '{\n'
        f'  "world": "{world}",\n'
        f'  "tile_px": {TILE_PX},\n'
        f'  "grid_min_x": {min_x},\n'
        f'  "grid_max_x": {max_x},\n'
        f'  "grid_min_y": {min_y},\n'
        f'  "grid_max_y": {max_y},\n'
        f'  "mosaic_cols": {cols},\n'
        f'  "mosaic_rows": {rows},\n'
        f'  "tile_count": {len(tiles)}\n'
        '}\n',
        encoding="utf-8",
    )
    print(f"writing {metadata}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
