"""Interactive prototype viewer for the Farever minimap mod's
fog-of-war (FOW) experience.

What it does:
- Shows the stitched W1_Siagarta mosaic as a scaled-down image.
- Loads all 125 zone polygons from .mogshape files.
- Lets you "walk" a synthetic player around with the mouse. Hold left
  to drag-walk; right-click teleports.
- Whenever the player enters a zone, that zone becomes "discovered"
  and reveals its painted area on the minimap. Unrevealed zones are
  dimmed to ~25 %.
- Persists discovered-zones to a JSON file so you can stop / restart.

Keyboard:
  Space  toggle FOW on/off (debug)
  M      toggle mosaic background (off = abstract colored zones only)
  R      reveal everything
  X      reset (clear discovery)
  S      save FOW state
  Q      quit / close (saves on exit)

Calibration (move polygons relative to mosaic to find the right transform):
  Arrows         pan polygons +/- 5 world units
  Shift+Arrows   pan polygons +/- 50 world units
  + / -          scale polygons by 1.02 / 0.98
  F              flip Y axis (Heaps Y up vs. down)
  0              reset transform to defaults
  C              save current calibration to research/fow_calibration.json
"""
from __future__ import annotations

import json
import tkinter as tk
from pathlib import Path

from PIL import Image, ImageDraw, ImageTk

from hxserializer import unserialize, HxClass    # type: ignore[import-not-found]
from pak_extract import open_pak                 # type: ignore[import-not-found]

# ---------------------------------------------------------------- config
GAME_DIR     = Path(r"D:\SteamLibrary\steamapps\common\Farever")
PROJ_DIR     = Path(r"D:\farevermod")
MAP_DIR      = PROJ_DIR / "research" / "maps"
STATE_FILE   = PROJ_DIR / "research" / "fow_state.json"
CALIB_FILE   = PROJ_DIR / "research" / "fow_calibration.json"
LIVE_FILE    = PROJ_DIR / "research" / "live_position.json"
WORLD        = "W1_Siagarta"
LIVE_POLL_MS = 100

TILE_PX            = 1024
METERS_PER_TILE    = 256.0
DISPLAY_PX         = 900           # final canvas size (square-ish)
UNREVEALED_ALPHA   = 0.22          # how dim the un-discovered terrain looks
DISCOVERY_RADIUS   = 18.0          # world units of "reveal radius" around player
PLAYER_DOT_RADIUS  = 5
EDGE_BLUR_PX       = 6             # soft FOW edge

# Default calibration (user can override + save)
DEFAULT_CALIB = {
    "offset_x": 0.0,    # world units added to polygon x before projection
    "offset_y": 0.0,    # world units added to polygon y before projection
    "scale":    1.0,    # polygon coords multiplied by this before projection
    "flip_y":   True,   # if True, polygon y is mirrored (Heaps Y-up convention)
}


# ---------------------------------------------------------------- data
def load_zones(pak_path: Path, world: str):
    buf, entries, data_start = open_pak(pak_path)
    prefix = f"Level/World/{world}.dat/minimap/zones/"
    zones = []
    for path, sz, pos in entries:
        if not path.startswith(prefix) or not path.endswith(".mogshape"):
            continue
        text = buf[data_start + pos : data_start + pos + sz].decode("latin-1")
        obj = unserialize(text)
        polys = []
        for poly in obj:
            pts = [(p.fields["x"], p.fields["y"]) for p in poly if isinstance(p, HxClass)]
            if len(pts) >= 3:
                polys.append(pts)
        if polys:
            zones.append({"name": path[len(prefix):-len(".mogshape")], "polys": polys})
    return zones


def load_state() -> set[str]:
    if STATE_FILE.exists():
        try:
            return set(json.loads(STATE_FILE.read_text())["revealed"])
        except Exception:
            pass
    return set()


def save_state(revealed: set[str]) -> None:
    STATE_FILE.write_text(json.dumps({"revealed": sorted(revealed)}, indent=2))


def load_calib() -> dict:
    if CALIB_FILE.exists():
        try:
            d = json.loads(CALIB_FILE.read_text())
            return {**DEFAULT_CALIB, **d}
        except Exception:
            pass
    return dict(DEFAULT_CALIB)


def save_calib(c: dict) -> None:
    CALIB_FILE.write_text(json.dumps(c, indent=2))


def poly_area(poly: list[tuple[float, float]]) -> float:
    """Absolute polygon area via the shoelace formula."""
    n = len(poly)
    if n < 3:
        return 0.0
    s = 0.0
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        s += x1 * y2 - x2 * y1
    return abs(s) * 0.5


# ---------------------------------------------------------------- coords
class World2Pixel:
    """Transform from polygon-world coords to mosaic pixels, then to
    display pixels. The polygon→world step has a runtime-adjustable
    calibration (offset/scale/flip) so we can nudge the polygons over
    the mosaic until they line up. Once the game gives us live player
    coordinates we'll lock the right values in."""

    def __init__(self, mosaic_meta: dict, calib: dict):
        self.min_gx = mosaic_meta["grid_min_x"]
        self.max_gy = mosaic_meta["grid_max_y"]
        self.tile_px = mosaic_meta["tile_px"]
        self.mosaic_w = mosaic_meta["mosaic_cols"] * self.tile_px
        self.mosaic_h = mosaic_meta["mosaic_rows"] * self.tile_px
        self.px_per_m = self.tile_px / METERS_PER_TILE

        self.disp_scale = DISPLAY_PX / max(self.mosaic_w, self.mosaic_h)
        self.disp_w = int(self.mosaic_w * self.disp_scale)
        self.disp_h = int(self.mosaic_h * self.disp_scale)

        self.calib = dict(calib)

    # ----- polygon-space → world ----- (the calibration step)
    def p2w(self, px: float, py: float) -> tuple[float, float]:
        c = self.calib
        wx = (px + c["offset_x"]) * c["scale"]
        wy = (py + c["offset_y"]) * c["scale"]
        return wx, wy

    def w2p(self, wx: float, wy: float) -> tuple[float, float]:
        c = self.calib
        return wx / c["scale"] - c["offset_x"], wy / c["scale"] - c["offset_y"]

    # ----- world → mosaic pixel ----- (the geometric step)
    def w2m(self, x: float, y: float) -> tuple[float, float]:
        mx = (x - self.min_gx * METERS_PER_TILE) * self.px_per_m
        if self.calib["flip_y"]:
            my = (self.max_gy * METERS_PER_TILE + METERS_PER_TILE - y) * self.px_per_m
        else:
            my = (y - (self.max_gy - 11) * METERS_PER_TILE) * self.px_per_m
        return mx, my

    def m2w(self, mx: float, my: float) -> tuple[float, float]:
        wx = mx / self.px_per_m + self.min_gx * METERS_PER_TILE
        if self.calib["flip_y"]:
            wy = self.max_gy * METERS_PER_TILE + METERS_PER_TILE - my / self.px_per_m
        else:
            wy = my / self.px_per_m + (self.max_gy - 11) * METERS_PER_TILE
        return wx, wy

    # ----- mosaic ↔ display -----
    def m2d(self, mx: float, my: float) -> tuple[float, float]:
        return mx * self.disp_scale, my * self.disp_scale

    def d2m(self, dx: float, dy: float) -> tuple[float, float]:
        return dx / self.disp_scale, dy / self.disp_scale

    # ----- end-to-end: polygon coords → display pixel -----
    def p2d(self, px: float, py: float) -> tuple[float, float]:
        return self.m2d(*self.w2m(*self.p2w(px, py)))

    # ----- end-to-end: display click → polygon coords -----
    def d2p(self, dx: float, dy: float) -> tuple[float, float]:
        return self.w2p(*self.m2w(*self.d2m(dx, dy)))


# ---------------------------------------------------------------- geometry
def point_in_polygon(x: float, y: float, poly: list[tuple[float, float]]) -> bool:
    """Standard ray-casting point-in-polygon."""
    n = len(poly)
    inside = False
    j = n - 1
    for i in range(n):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi):
            inside = not inside
        j = i
    return inside


# ---------------------------------------------------------------- render
class Renderer:
    def __init__(self, mosaic_full: Image.Image, zones: list, xf: World2Pixel):
        self.bg = mosaic_full.resize((xf.disp_w, xf.disp_h), Image.LANCZOS).convert("RGBA")
        from PIL import ImageEnhance
        self.bg_dim = ImageEnhance.Brightness(self.bg).enhance(UNREVEALED_ALPHA)
        self.bg_abstract = Image.new("RGBA", (xf.disp_w, xf.disp_h), (24, 26, 36, 255))
        self.xf = xf
        self.zones = zones
        # Cached masks keyed by calibration signature so we re-rasterize
        # only when the user nudges.
        self._mask_cache: dict[str, dict[str, Image.Image]] = {}
        self._abstract_cache: dict[str, Image.Image] = {}

    def _calib_key(self) -> str:
        c = self.xf.calib
        return f"{c['offset_x']:.3f},{c['offset_y']:.3f},{c['scale']:.5f},{int(c['flip_y'])}"

    def _zone_masks(self) -> dict[str, Image.Image]:
        key = self._calib_key()
        cached = self._mask_cache.get(key)
        if cached is not None:
            return cached
        masks: dict[str, Image.Image] = {}
        for z in self.zones:
            mask = Image.new("L", (self.xf.disp_w, self.xf.disp_h), 0)
            md = ImageDraw.Draw(mask)
            for poly in z["polys"]:
                pts = [self.xf.p2d(x, y) for x, y in poly]
                md.polygon(pts, fill=255)
            masks[z["name"]] = mask
        self._mask_cache.clear()
        self._mask_cache[key] = masks
        return masks

    def _abstract_bg(self, revealed: set[str]) -> Image.Image:
        """Pretty background built from sorted-by-area filled polygons —
        used when mosaic is toggled off so we can validate alignment."""
        img = Image.new("RGBA", (self.xf.disp_w, self.xf.disp_h), (24, 26, 36, 255))
        drw = ImageDraw.Draw(img, "RGBA")
        # Largest first so smaller zones sit on top
        for z in self.zones:
            if z["name"] not in revealed:
                continue
            c = z["color"]
            for poly in z["polys"]:
                if len(poly) < 3:
                    continue
                pts = [self.xf.p2d(x, y) for x, y in poly]
                drw.polygon(pts, fill=(c[0], c[1], c[2], 230),
                            outline=(c[0], c[1], c[2], 255))
        return img

    def compose(self, revealed: set[str], player_xy_world: tuple[float, float] | None,
                fow_enabled: bool = True, show_mosaic: bool = True) -> Image.Image:
        if not show_mosaic:
            img = self._abstract_bg(revealed if fow_enabled else
                                    {z["name"] for z in self.zones})
        elif not fow_enabled:
            img = self.bg.copy()
        else:
            masks = self._zone_masks()
            mask = Image.new("L", (self.xf.disp_w, self.xf.disp_h), 0)
            for name in revealed:
                m = masks.get(name)
                if m is None:
                    continue
                mask.paste(255, mask=m)
            if revealed:
                from PIL import ImageFilter
                mask = mask.filter(ImageFilter.GaussianBlur(EDGE_BLUR_PX))
            img = Image.composite(self.bg, self.bg_dim, mask)

        if player_xy_world is not None:
            dx, dy = self.xf.m2d(*self.xf.w2m(*player_xy_world))
            drw = ImageDraw.Draw(img, "RGBA")
            r = PLAYER_DOT_RADIUS
            drw.ellipse([dx - r - 2, dy - r - 2, dx + r + 2, dy + r + 2],
                        fill=(0, 0, 0, 200))
            drw.ellipse([dx - r, dy - r, dx + r, dy + r],
                        fill=(255, 60, 60, 255))
        return img


# ---------------------------------------------------------------- app
class App:
    def __init__(self, mosaic_full: Image.Image, mosaic_meta: dict, zones: list) -> None:
        self.calib = load_calib()
        self.xf = World2Pixel(mosaic_meta, self.calib)
        self.zones = zones
        self.renderer = Renderer(mosaic_full, zones, self.xf)
        self.revealed: set[str] = load_state()
        self.fow_enabled = True
        self.show_mosaic = True
        # Spawn player at polygon origin (whatever that maps to)
        self.player_px, self.player_py = (0.0, 0.0)
        self.live_enabled = LIVE_FILE.exists()
        self.live_last_ts = 0.0

        self.root = tk.Tk()
        self.root.title(f"Farever Minimap FOW prototype — {WORLD}")
        self.root.geometry(f"{self.xf.disp_w}x{self.xf.disp_h + 56}")
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.canvas = tk.Canvas(self.root, width=self.xf.disp_w, height=self.xf.disp_h,
                                bg="#101218", highlightthickness=0)
        self.canvas.pack()
        self.status1 = tk.Label(self.root, anchor="w", font=("Consolas", 9),
                                bg="#101218", fg="#dddddd")
        self.status1.pack(fill=tk.X)
        self.status2 = tk.Label(self.root, anchor="w", font=("Consolas", 9),
                                bg="#181b25", fg="#9eb4ff")
        self.status2.pack(fill=tk.X)

        self.tk_img: ImageTk.PhotoImage | None = None
        self.canvas.bind("<Button-1>", self.on_drag)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<Button-3>", self.on_teleport)
        self.canvas.bind("<Motion>", self.on_hover)
        # state hotkeys
        self.root.bind("<space>", lambda _: self.toggle_fow())
        self.root.bind("m",       lambda _: self.toggle_mosaic())
        self.root.bind("r",       lambda _: self.reveal_all())
        self.root.bind("x",       lambda _: self.reset_fow())
        self.root.bind("s",       lambda _: self.save())
        self.root.bind("q",       lambda _: self.on_close())
        # calibration hotkeys
        self.root.bind("<Left>",        lambda e: self.pan(-5, 0, e))
        self.root.bind("<Right>",       lambda e: self.pan(+5, 0, e))
        self.root.bind("<Up>",          lambda e: self.pan(0, +5, e))
        self.root.bind("<Down>",        lambda e: self.pan(0, -5, e))
        self.root.bind("<Shift-Left>",  lambda e: self.pan(-50, 0, e))
        self.root.bind("<Shift-Right>", lambda e: self.pan(+50, 0, e))
        self.root.bind("<Shift-Up>",    lambda e: self.pan(0, +50, e))
        self.root.bind("<Shift-Down>",  lambda e: self.pan(0, -50, e))
        self.root.bind("+",        lambda _: self.zoom(1.02))
        self.root.bind("<KP_Add>", lambda _: self.zoom(1.02))
        self.root.bind("-",        lambda _: self.zoom(1/1.02))
        self.root.bind("<KP_Subtract>", lambda _: self.zoom(1/1.02))
        self.root.bind("f",        lambda _: self.flip_y())
        self.root.bind("0",        lambda _: self.reset_calib())
        self.root.bind("c",        lambda _: self.save_calib())
        self.root.bind("l",        lambda _: self.toggle_live())

        self.hover_zone = ""
        self.refresh()
        if self.live_enabled:
            self.root.after(LIVE_POLL_MS, self._poll_live)

    def _poll_live(self) -> None:
        """Read live_position.json (written by track_pos.py loop) and
        update the player marker. Treats the game's (x, y) world coords
        as polygon-space input; the calibration step in World2Pixel
        re-maps them. Trigger a refresh + auto-discover if position
        actually changed."""
        try:
            data = json.loads(LIVE_FILE.read_text())
            ts = data.get("ts", 0)
            if ts != self.live_last_ts:
                self.live_last_ts = ts
                self.player_px = float(data["x"])
                self.player_py = float(data["y"])
                self.discover_at(self.player_px, self.player_py)
                self.refresh()
        except Exception:
            pass
        if self.live_enabled:
            self.root.after(LIVE_POLL_MS, self._poll_live)

    def toggle_live(self) -> None:
        self.live_enabled = not self.live_enabled
        if self.live_enabled:
            self.root.after(LIVE_POLL_MS, self._poll_live)
        self.refresh_status()

    # ---- input handlers --------------------------------------------
    def on_drag(self, ev) -> None:
        px, py = self.xf.d2p(ev.x, ev.y)
        self.player_px, self.player_py = px, py
        self.discover_at(px, py)
        self.refresh()

    def on_teleport(self, ev) -> None:
        self.on_drag(ev)

    def on_hover(self, ev) -> None:
        px, py = self.xf.d2p(ev.x, ev.y)
        name = self.zone_at(px, py)
        if name != self.hover_zone:
            self.hover_zone = name
            self.refresh_status()

    # ---- discovery -------------------------------------------------
    def zone_at(self, px: float, py: float) -> str:
        """Smallest-area containing zone wins, so sub-zones beat parents."""
        best: tuple[float, str] | None = None
        for z in self.zones:
            for poly in z["polys"]:
                if point_in_polygon(px, py, poly):
                    a = z["area"]
                    if best is None or a < best[0]:
                        best = (a, z["name"])
                    break
        return best[1] if best else ""

    def discover_at(self, px: float, py: float) -> None:
        # Reveal every zone whose polygon currently contains the player,
        # plus zones whose nearest vertex is within DISCOVERY_RADIUS.
        # Coords are in polygon space here (NOT world meters), so the
        # radius is in the same units as the polygons.
        for z in self.zones:
            if z["name"] in self.revealed:
                continue
            for poly in z["polys"]:
                if point_in_polygon(px, py, poly):
                    self.revealed.add(z["name"])
                    break
                for (vx, vy) in poly:
                    if abs(vx - px) < DISCOVERY_RADIUS and abs(vy - py) < DISCOVERY_RADIUS:
                        if (vx - px) ** 2 + (vy - py) ** 2 < DISCOVERY_RADIUS ** 2:
                            self.revealed.add(z["name"])
                            break
                if z["name"] in self.revealed:
                    break

    # ---- calibration -----------------------------------------------
    def pan(self, dx: float, dy: float, ev=None) -> None:
        self.calib["offset_x"] += dx
        self.calib["offset_y"] += dy
        self.xf.calib = self.calib
        self.refresh()

    def zoom(self, factor: float) -> None:
        self.calib["scale"] *= factor
        self.xf.calib = self.calib
        self.refresh()

    def flip_y(self) -> None:
        self.calib["flip_y"] = not self.calib["flip_y"]
        self.xf.calib = self.calib
        self.refresh()

    def reset_calib(self) -> None:
        self.calib = dict(DEFAULT_CALIB)
        self.xf.calib = self.calib
        self.refresh()

    def save_calib(self) -> None:
        save_calib(self.calib)
        self.refresh_status(extra=f"  (calib saved → {CALIB_FILE.name})")

    # ---- actions ---------------------------------------------------
    def toggle_fow(self) -> None:
        self.fow_enabled = not self.fow_enabled
        self.refresh()

    def toggle_mosaic(self) -> None:
        self.show_mosaic = not self.show_mosaic
        self.refresh()

    def reveal_all(self) -> None:
        self.revealed = {z["name"] for z in self.zones}
        self.refresh()

    def reset_fow(self) -> None:
        self.revealed.clear()
        self.refresh()

    def save(self) -> None:
        save_state(self.revealed)
        self.refresh_status(extra="  (saved)")

    def on_close(self) -> None:
        save_state(self.revealed)
        self.root.destroy()

    # ---- render ----------------------------------------------------
    def refresh(self) -> None:
        img = self.renderer.compose(self.revealed,
                                    (self.player_px, self.player_py),
                                    fow_enabled=self.fow_enabled,
                                    show_mosaic=self.show_mosaic)
        self.tk_img = ImageTk.PhotoImage(img)
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.tk_img, anchor="nw")
        self.refresh_status()

    def refresh_status(self, extra: str = "") -> None:
        n = len(self.revealed)
        total = len(self.zones)
        fow = "ON " if self.fow_enabled else "OFF"
        bg  = "mosaic" if self.show_mosaic else "abstract"
        live = "LIVE" if self.live_enabled else "off "
        cur = self.hover_zone or "—"
        c   = self.calib
        self.status1.config(
            text=(f" FOW: {fow}  bg: {bg:8}  live: {live}  revealed: {n}/{total}  "
                  f"player: ({self.player_px:+.1f}, {self.player_py:+.1f})  "
                  f"hover: {cur}{extra}")
        )
        self.status2.config(
            text=(f" calib: offset=({c['offset_x']:+.1f}, {c['offset_y']:+.1f}) "
                  f"scale={c['scale']:.4f}  flip_y={c['flip_y']}    "
                  f"[arrows pan  shift+arrows pan10x  +/- zoom  F flip  "
                  f"0 reset  C save calib  M toggle bg]")
        )

    def run(self) -> None:
        self.root.mainloop()


def _annotate_zones(zones: list) -> list:
    """Attach a per-zone 'color' and 'area' so the renderer can sort
    largest-first (small zones render on top) and color-code by region.
    Mutates and returns the list."""
    from collections import defaultdict
    region_to_color = {
        "Z1":            (244, 113, 116),
        "Z2":            (132, 200, 132),
        "Z3":            (110, 173, 233),
        "Bel":           (227, 184, 100),
        "CrimsonIsland": (208,  96, 200),
    }
    for z in zones:
        c = (200, 200, 200)
        for k, v in region_to_color.items():
            if z["name"].startswith(k):
                c = v
                break
        z["color"] = c
        z["area"] = sum(poly_area(p) for p in z["polys"])
    # Sort largest first so small sub-zones land on top of their parents
    zones.sort(key=lambda z: z["area"], reverse=True)
    return zones


def main() -> int:
    pak = GAME_DIR / "res.map.pak"
    mosaic_full = Image.open(MAP_DIR / f"{WORLD}.mosaic.png")
    meta = json.loads((MAP_DIR / f"{WORLD}.mosaic.json").read_text())
    print(f"loading zones from {pak.name} ...")
    zones = load_zones(pak, WORLD)
    _annotate_zones(zones)
    print(f"  {len(zones)} zones, {sum(len(p['polys']) for p in zones)} polygons "
          f"(sorted largest-first)")
    print("opening viewer ...")
    Image.MAX_IMAGE_PIXELS = None   # allow our 11264x11264 mosaic
    App(mosaic_full, meta, zones).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
