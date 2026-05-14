"""Render zone polygons from `.mogshape` files into a couple of PNGs:

1. zones.png             — standalone, polygons in their own space,
                           color-coded per region (Z1, Z2, Z3, …).
2. zones_on_mosaic.png   — same polygons overlaid on the stitched
                           mosaic, using a guessed world-to-pixel
                           transform. Inspect this visually to refine
                           the transform once live player coords are
                           available.

Usage:
    python render_zones.py <res.map.pak> <world_name> <mosaic_dir>
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

from hxserializer import unserialize, HxClass  # type: ignore[import-not-found]
from pak_extract import open_pak               # type: ignore[import-not-found]

# Region prefix → display color (RGBA)
REGION_COLORS: dict[str, tuple[int, int, int, int]] = {
    "Z1":           (244, 113, 116, 200),
    "Z2":           (132, 200, 132, 200),
    "Z3":           (110, 173, 233, 200),
    "Bel":          (227, 184, 100, 200),  # Bel_Etir
    "CrimsonIsland":(208,  96, 200, 200),
}
DEFAULT_COLOR = (200, 200, 200, 180)


def region_color(zone_name: str) -> tuple[int, int, int, int]:
    for k, v in REGION_COLORS.items():
        if zone_name.startswith(k):
            return v
    return DEFAULT_COLOR


def load_zones(pak_path: Path, world: str) -> list[tuple[str, list[list[tuple[float, float]]]]]:
    buf, entries, data_start = open_pak(pak_path)
    prefix = f"Level/World/{world}.dat/minimap/zones/"
    zones: list[tuple[str, list[list[tuple[float, float]]]]] = []
    for path, sz, pos in entries:
        if not path.startswith(prefix) or not path.endswith(".mogshape"):
            continue
        data = buf[data_start + pos : data_start + pos + sz].decode("latin-1")
        obj = unserialize(data)
        polys: list[list[tuple[float, float]]] = []
        for poly in obj:
            pts = [(p.fields["x"], p.fields["y"]) for p in poly if isinstance(p, HxClass)]
            if pts:
                polys.append(pts)
        name = path[len(prefix):-len(".mogshape")]
        zones.append((name, polys))
    return zones


def standalone(zones, out_path: Path, max_px: int = 4096) -> None:
    xs = [p[0] for _, polys in zones for poly in polys for p in poly]
    ys = [p[1] for _, polys in zones for poly in polys for p in poly]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    span_x = max(1, max_x - min_x)
    span_y = max(1, max_y - min_y)
    scale = min(max_px / span_x, max_px / span_y)
    margin = 16
    w = int(span_x * scale) + 2 * margin
    h = int(span_y * scale) + 2 * margin

    img = Image.new("RGBA", (w, h), (24, 26, 36, 255))
    drw = ImageDraw.Draw(img, "RGBA")

    def proj(x, y):
        # flip y so positive-y goes up (image y goes down)
        return margin + (x - min_x) * scale, margin + (max_y - y) * scale

    for name, polys in zones:
        col = region_color(name)
        outline = (col[0], col[1], col[2], 255)
        for poly in polys:
            if len(poly) < 3:
                continue
            pts = [proj(x, y) for x, y in poly]
            drw.polygon(pts, fill=col, outline=outline)

    img.save(out_path)
    print(f"  wrote {out_path}  ({w}x{h})")


def overlay(zones, mosaic_path: Path, meta_path: Path, out_path: Path,
            meters_per_tile: float = 256.0, max_px: int = 4096) -> None:
    """Overlay polygons on top of the mosaic.

    Assumes: polygon coords are in world units (call them 'm') and each
    minimap tile spans `meters_per_tile` world units. Origin (0,0) in
    polygon space sits at the inner corner of tile (0,0). Y in polygon
    space increases northward (image Y decreases).
    """
    mosaic = Image.open(mosaic_path).convert("RGBA")
    meta = json.loads(meta_path.read_text())
    tile_px = meta["tile_px"]
    min_gx = meta["grid_min_x"]
    max_gy = meta["grid_max_y"]
    px_per_m = tile_px / meters_per_tile

    overlay = Image.new("RGBA", mosaic.size, (0, 0, 0, 0))
    drw = ImageDraw.Draw(overlay, "RGBA")

    def proj(x, y):
        px = (x - min_gx * meters_per_tile) * px_per_m
        py = (max_gy * meters_per_tile + meters_per_tile - y) * px_per_m
        return px, py

    for name, polys in zones:
        col = region_color(name)
        edge = (col[0], col[1], col[2], 255)
        fill = (col[0], col[1], col[2], 90)
        for poly in polys:
            if len(poly) < 3:
                continue
            pts = [proj(x, y) for x, y in poly]
            drw.polygon(pts, fill=fill, outline=edge)

    combined = Image.alpha_composite(mosaic, overlay)
    long_side = max(combined.size)
    if long_side > max_px:
        scale = max_px / long_side
        new = (round(combined.size[0] * scale), round(combined.size[1] * scale))
        combined = combined.resize(new, Image.LANCZOS)
    combined.save(out_path)
    print(f"  wrote {out_path}")


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    pak = Path(sys.argv[1])
    world = sys.argv[2]
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"loading zones for {world} ...")
    zones = load_zones(pak, world)
    total = sum(len(p) for _, p in zones)
    print(f"  {len(zones)} zones, {total} polygons")

    standalone(zones, out_dir / f"{world}.zones.png")

    mosaic = out_dir / f"{world}.mosaic.png"
    meta = out_dir / f"{world}.mosaic.json"
    if mosaic.exists() and meta.exists():
        for mpt in (128, 192, 256, 384):
            overlay(zones, mosaic, meta,
                    out_dir / f"{world}.zones_on_mosaic.mpt{mpt}.png",
                    meters_per_tile=mpt)
    else:
        print("  (mosaic + metadata not found, skipped overlay)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
