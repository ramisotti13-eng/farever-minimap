"""Minimap calibration tool with zoom + pan.

Loads the W1_Siagarta mosaic, polls research/live_position.json for the
current local-player world coordinates, draws a yellow dot where the
current calibration places you. You click on the mosaic where you
ACTUALLY are in-game. After 2+ points the script fits an affine
transform (scale_x, scale_y, offset_x, offset_y; flip_y locked) and
writes research/minimap_calibration.json — the DLL hot-reloads on file
change.

Workflow:
  1. Game running + find_me.py --loop pumping live_position.json
  2. python tools/calibrate.py
  3. Mouse-wheel to zoom into a region you recognize.
  4. Walk to a known spot in-game, then left-click on the mosaic where
     your character actually is.
  5. Repeat 2-4 more times at well-separated spots.
  6. Press F to fit + save. Yellow dot snaps to the new prediction.

Controls:
  Left-click          add a calibration point (live coord ↔ click px)
  Mouse-wheel         zoom in/out at cursor
  Right-drag / Mid-d  pan
  +  /  -             zoom in / out (keyboard)
  0                   reset view (zoom 1, centered)
  F                   fit affine + save research/minimap_calibration.json
  D                   delete last point
  R                   reset collected points
  Q / Esc             quit
"""
from __future__ import annotations

import json
import tkinter as tk
from pathlib import Path

from PIL import Image, ImageTk

PROJ_DIR    = Path(r"D:\farevermod")
MAP_DIR     = PROJ_DIR / "research" / "maps"
LIVE_FILE   = PROJ_DIR / "research" / "live_position.json"
CALIB_FILE  = PROJ_DIR / "research" / "minimap_calibration.json"
MOSAIC_META = MAP_DIR / "W1_Siagarta.mosaic.json"
MOSAIC_PNG  = MAP_DIR / "W1_Siagarta.preview.png"   # 4096²

DISPLAY_PX   = 900
LIVE_POLL_MS = 100
ZOOM_STEP    = 1.25
ZOOM_MIN     = 0.5
ZOOM_MAX     = 20.0

DEFAULT_CALIB = {
    "scale_x":  4.0,
    "scale_y":  4.0,
    "offset_x": 4096.0,
    "offset_y": 6144.0,
    "flip_y":   True,
}


def load_calib() -> dict:
    if CALIB_FILE.exists():
        try:
            d = json.loads(CALIB_FILE.read_text())
            d.pop("_comment", None)
            return {**DEFAULT_CALIB, **d}
        except Exception:
            pass
    return dict(DEFAULT_CALIB)


def save_calib(c: dict) -> None:
    out = {"_comment": "Auto-fitted by tools/calibrate.py. "
                       "DLL hot-reloads on mtime change."}
    out.update(c)
    CALIB_FILE.write_text(json.dumps(out, indent=2))


def world_to_full(world_x: float, world_y: float, c: dict
                  ) -> tuple[float, float]:
    fx = world_x * c["scale_x"] + c["offset_x"]
    fy = world_y * c["scale_y"] + c["offset_y"]
    if c["flip_y"]:
        fy = 11264.0 - fy
    return fx, fy


def fit_affine(pairs: list[tuple[float, float, float, float]],
               flip_y: bool, prior: dict) -> tuple[dict, str]:
    """Pairs: (world_x, world_y, full_x, full_y).

    full_x        = sx * world_x + ox
    full_y_eff    = sy * world_y + oy   where
    full_y_eff    = (11264 - full_y) if flip_y else full_y

    Fits each axis independently via least-squares. If an axis is
    degenerate (only 1 unique world coord, e.g. user didn't move
    between clicks), that axis falls back to the prior scale and only
    the offset is solved from the mean. Returns (calib, note).
    """
    if not pairs:
        raise ValueError("no points")

    def fit(xs: list[float], ys: list[float], prior_scale: float
            ) -> tuple[float, float, bool]:
        n = len(xs)
        sx, sy = sum(xs), sum(ys)
        sxx = sum(x * x for x in xs)
        sxy = sum(x * y for x, y in zip(xs, ys))
        denom = n * sxx - sx * sx
        if n >= 2 and abs(denom) >= 1e-6:
            s = (n * sxy - sx * sy) / denom
            o = (sy - s * sx) / n
            return s, o, False
        # Degenerate (n=1 or all xs equal): keep prior scale, fit offset.
        s = prior_scale
        o = (sy - s * sx) / n
        return s, o, True

    wx = [p[0] for p in pairs]
    wy = [p[1] for p in pairs]
    fx = [p[2] for p in pairs]
    fy_eff = [(11264.0 - p[3]) if flip_y else p[3] for p in pairs]

    sx, ox, dx = fit(wx, fx, prior["scale_x"])
    sy, oy, dy = fit(wy, fy_eff, prior["scale_y"])
    note_parts = []
    if dx: note_parts.append("scale_x kept from prior (need movement in X)")
    if dy: note_parts.append("scale_y kept from prior (need movement in Y)")
    note = "; ".join(note_parts) if note_parts else "full fit"
    return {
        "scale_x":  sx,
        "scale_y":  sy,
        "offset_x": ox,
        "offset_y": oy,
        "flip_y":   flip_y,
    }, note


class App:
    def __init__(self) -> None:
        meta = json.loads(MOSAIC_META.read_text())
        self.tile_px     = meta["tile_px"]
        self.mosaic_full = meta["mosaic_cols"] * self.tile_px   # 11264
        Image.MAX_IMAGE_PIXELS = None
        # Source image is the 4096² preview. We use it for all sampling
        # and resize per-frame based on current zoom + pan.
        self.src_img  = Image.open(MOSAIC_PNG).convert("RGB")
        self.src_size = self.src_img.size[0]   # 4096

        self.calib  = load_calib()
        self.pairs: list[tuple[float, float, float, float]] = []
        self.live_x = self.live_y = 0.0
        self.live_valid = False
        self.live_ts    = 0.0

        # View transform in FULL-mosaic coords. cx/cy = center of view,
        # zoom = mosaic-pixels-per-display-pixel-reciprocal (zoom 1 fits
        # full mosaic into DISPLAY_PX).
        self.zoom = 1.0
        self.cx   = self.mosaic_full / 2.0
        self.cy   = self.mosaic_full / 2.0

        self._pan_anchor: tuple[int, int] | None = None
        self._pan_origin: tuple[float, float] = (0.0, 0.0)

        self.root = tk.Tk()
        self.root.title("Farever minimap — calibrate")
        self.root.geometry(f"{DISPLAY_PX}x{DISPLAY_PX + 80}")
        self.canvas = tk.Canvas(self.root, width=DISPLAY_PX,
                                height=DISPLAY_PX, bg="#101218",
                                highlightthickness=0, cursor="crosshair")
        self.canvas.pack()

        btn_row = tk.Frame(self.root, bg="#101218")
        btn_row.pack(fill=tk.X, pady=2)
        tk.Button(btn_row, text="Fit & Save (F)",
                  command=self.fit_and_save,
                  bg="#3f7", fg="#000",
                  font=("Consolas", 10, "bold")
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_row, text="Undo last (D)",
                  command=self.del_last
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_row, text="Reset points (R)",
                  command=self.reset_points
                  ).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_row, text="Reset view (0)",
                  command=self.reset_view
                  ).pack(side=tk.LEFT, padx=4)

        self.status1 = tk.Label(self.root, anchor="w", font=("Consolas", 9),
                                bg="#101218", fg="#dddddd")
        self.status1.pack(fill=tk.X)
        self.status2 = tk.Label(self.root, anchor="w", font=("Consolas", 9),
                                bg="#181b25", fg="#9eb4ff")
        self.status2.pack(fill=tk.X)

        self.tk_img: ImageTk.PhotoImage | None = None
        self.canvas.bind("<Button-1>", self.on_click)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        # Right-click and middle-click drag both pan.
        for btn in ("<Button-3>", "<Button-2>"):
            self.canvas.bind(btn, self.on_pan_start)
        for btn in ("<B3-Motion>", "<B2-Motion>"):
            self.canvas.bind(btn, self.on_pan_move)
        self.canvas.bind("<Motion>", self.on_motion)

        self.root.bind("+",        lambda _: self.zoom_at(DISPLAY_PX // 2,
                                                          DISPLAY_PX // 2,
                                                          ZOOM_STEP))
        self.root.bind("<KP_Add>", lambda _: self.zoom_at(DISPLAY_PX // 2,
                                                          DISPLAY_PX // 2,
                                                          ZOOM_STEP))
        self.root.bind("-",        lambda _: self.zoom_at(DISPLAY_PX // 2,
                                                          DISPLAY_PX // 2,
                                                          1 / ZOOM_STEP))
        self.root.bind("<KP_Subtract>",
                       lambda _: self.zoom_at(DISPLAY_PX // 2,
                                              DISPLAY_PX // 2,
                                              1 / ZOOM_STEP))
        # bind_all so key events fire regardless of which widget has focus.
        self.root.bind_all("0", lambda _: self.reset_view())
        self.root.bind_all("f", lambda _: self.fit_and_save())
        self.root.bind_all("F", lambda _: self.fit_and_save())
        self.root.bind_all("r", lambda _: self.reset_points())
        self.root.bind_all("R", lambda _: self.reset_points())
        self.root.bind_all("d", lambda _: self.del_last())
        self.root.bind_all("D", lambda _: self.del_last())
        self.root.bind_all("q",       lambda _: self.root.destroy())
        self.root.bind_all("<Escape>", lambda _: self.root.destroy())

        self.hover_full = (0.0, 0.0)
        self.refresh()
        self.root.after(LIVE_POLL_MS, self.poll_live)

    # -- view coord transforms (full mosaic px <-> display px) ---------
    def viewport_full(self) -> float:
        return self.mosaic_full / self.zoom

    def full_to_disp(self, fx: float, fy: float) -> tuple[float, float]:
        v = self.viewport_full()
        left = self.cx - v / 2.0
        top  = self.cy - v / 2.0
        return (fx - left) / v * DISPLAY_PX, (fy - top) / v * DISPLAY_PX

    def disp_to_full(self, dx: float, dy: float) -> tuple[float, float]:
        v = self.viewport_full()
        left = self.cx - v / 2.0
        top  = self.cy - v / 2.0
        return left + dx / DISPLAY_PX * v, top + dy / DISPLAY_PX * v

    # -- input ---------------------------------------------------------
    def on_click(self, ev) -> None:
        if not self.live_valid:
            print("[calibrate] click ignored: no live position yet",
                  flush=True)
            return
        fx, fy = self.disp_to_full(ev.x, ev.y)
        self.pairs.append((self.live_x, self.live_y, fx, fy))
        print(f"[calibrate] added pt{len(self.pairs)-1}: "
              f"world=({self.live_x:.1f}, {self.live_y:.1f})  "
              f"full=({fx:.1f}, {fy:.1f})", flush=True)
        self.refresh()

    def on_wheel(self, ev) -> None:
        factor = ZOOM_STEP if ev.delta > 0 else 1 / ZOOM_STEP
        self.zoom_at(ev.x, ev.y, factor)

    def zoom_at(self, dx: int, dy: int, factor: float) -> None:
        # Keep the full-mosaic point under the cursor stationary.
        fx, fy = self.disp_to_full(dx, dy)
        new_zoom = max(ZOOM_MIN, min(ZOOM_MAX, self.zoom * factor))
        if abs(new_zoom - self.zoom) < 1e-6:
            return
        self.zoom = new_zoom
        # Re-anchor so cursor stays on same world spot.
        v = self.viewport_full()
        self.cx = fx - (dx / DISPLAY_PX - 0.5) * v
        self.cy = fy - (dy / DISPLAY_PX - 0.5) * v
        self.clamp_center()
        self.refresh()

    def on_pan_start(self, ev) -> None:
        self._pan_anchor = (ev.x, ev.y)
        self._pan_origin = (self.cx, self.cy)

    def on_pan_move(self, ev) -> None:
        if not self._pan_anchor:
            return
        ax, ay = self._pan_anchor
        ox, oy = self._pan_origin
        v = self.viewport_full()
        self.cx = ox - (ev.x - ax) / DISPLAY_PX * v
        self.cy = oy - (ev.y - ay) / DISPLAY_PX * v
        self.clamp_center()
        self.refresh()

    def on_motion(self, ev) -> None:
        self.hover_full = self.disp_to_full(ev.x, ev.y)
        self.refresh_status()

    def clamp_center(self) -> None:
        v = self.viewport_full()
        half = v / 2.0
        lo, hi = -half * 0.25, self.mosaic_full + half * 0.25
        self.cx = min(max(self.cx, lo + half), hi - half)
        self.cy = min(max(self.cy, lo + half), hi - half)

    def reset_view(self) -> None:
        self.zoom = 1.0
        self.cx = self.cy = self.mosaic_full / 2.0
        self.refresh()

    def del_last(self) -> None:
        if self.pairs:
            self.pairs.pop()
            self.refresh()

    def reset_points(self) -> None:
        self.pairs.clear()
        self.refresh()

    def fit_and_save(self) -> None:
        if not self.pairs:
            self.refresh(extra="  no points — left-click on the map first")
            return
        try:
            fit, note = fit_affine(self.pairs,
                                   flip_y=self.calib["flip_y"],
                                   prior=self.calib)
        except ValueError as e:
            self.refresh(extra=f"  fit failed: {e}")
            return
        print(f"[calibrate] fit ({len(self.pairs)} pts): {fit}  ({note})",
              flush=True)
        for i, (wx, wy, fx, fy) in enumerate(self.pairs):
            print(f"           pt{i}: world=({wx:.1f}, {wy:.1f})  "
                  f"full=({fx:.1f}, {fy:.1f})", flush=True)
        self.calib = fit
        save_calib(self.calib)
        self.refresh(extra=f"  saved -> {CALIB_FILE.name}  ({note})")

    # -- live poll -----------------------------------------------------
    def poll_live(self) -> None:
        try:
            data = json.loads(LIVE_FILE.read_text())
            ts = data.get("ts", 0)
            if ts != self.live_ts:
                self.live_ts    = ts
                self.live_x     = float(data["x"])
                self.live_y     = float(data["y"])
                self.live_valid = True
                self.refresh()
        except Exception:
            pass
        self.root.after(LIVE_POLL_MS, self.poll_live)

    # -- render --------------------------------------------------------
    def render_image(self) -> Image.Image:
        # Crop the source preview to the current viewport (in full coords,
        # mapped down by src/full ratio) and resize to DISPLAY_PX.
        ratio = self.src_size / self.mosaic_full   # 4096 / 11264
        v = self.viewport_full()
        left = self.cx - v / 2.0
        top  = self.cy - v / 2.0
        sx0 = int(left * ratio)
        sy0 = int(top * ratio)
        sx1 = int((left + v) * ratio)
        sy1 = int((top + v) * ratio)
        # Clamp + pad outside the source with black background.
        bg = Image.new("RGB", (max(1, sx1 - sx0), max(1, sy1 - sy0)),
                       (10, 12, 18))
        cx0 = max(0, sx0)
        cy0 = max(0, sy0)
        cx1 = min(self.src_size, sx1)
        cy1 = min(self.src_size, sy1)
        if cx1 > cx0 and cy1 > cy0:
            crop = self.src_img.crop((cx0, cy0, cx1, cy1))
            bg.paste(crop, (cx0 - sx0, cy0 - sy0))
        return bg.resize((DISPLAY_PX, DISPLAY_PX),
                         Image.LANCZOS if self.zoom <= 2.0 else Image.NEAREST)

    def refresh(self, extra: str = "") -> None:
        img = self.render_image()
        self.tk_img = ImageTk.PhotoImage(img)
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.tk_img, anchor="nw")

        # Tile-boundary grid every 1024 full px.
        for i in range(0, 12):
            f = i * 1024.0
            dx, _ = self.full_to_disp(f, 0)
            _, dy = self.full_to_disp(0, f)
            if 0 <= dx <= DISPLAY_PX:
                self.canvas.create_line(dx, 0, dx, DISPLAY_PX,
                                        fill="#234", width=1)
            if 0 <= dy <= DISPLAY_PX:
                self.canvas.create_line(0, dy, DISPLAY_PX, dy,
                                        fill="#234", width=1)

        # Collected pairs (green = where YOU said you were) + dashed line
        # to current-calib prediction.
        for (wx, wy, fx, fy) in self.pairs:
            dx, dy = self.full_to_disp(fx, fy)
            self.canvas.create_oval(dx - 5, dy - 5, dx + 5, dy + 5,
                                    outline="#000", width=2, fill="#3f7")
            pfx, pfy = world_to_full(wx, wy, self.calib)
            pdx, pdy = self.full_to_disp(pfx, pfy)
            self.canvas.create_line(dx, dy, pdx, pdy,
                                    fill="#3f7", width=1, dash=(3, 2))

        # Current-calib prediction for live coords (yellow).
        if self.live_valid:
            fx, fy = world_to_full(self.live_x, self.live_y, self.calib)
            dx, dy = self.full_to_disp(fx, fy)
            self.canvas.create_oval(dx - 6, dy - 6, dx + 6, dy + 6,
                                    outline="#000", width=2,
                                    fill="#ffc832")

        self.refresh_status(extra)

    def refresh_status(self, extra: str = "") -> None:
        c = self.calib
        live = (f"live ({self.live_x:+.1f}, {self.live_y:+.1f})"
                if self.live_valid else "live: waiting...")
        hx, hy = self.hover_full
        self.status1.config(
            text=(f" {live}   points: {len(self.pairs)}   "
                  f"hover full=({hx:.0f},{hy:.0f}) zoom={self.zoom:.2f}x   "
                  f"[click=add  wheel=zoom  rdrag=pan  F=fit  D=undo  R=reset]"
                  f"{extra}")
        )
        self.status2.config(
            text=(f" calib: scale=({c['scale_x']:.4f}, {c['scale_y']:.4f})  "
                  f"offset=({c['offset_x']:+.1f}, {c['offset_y']:+.1f})  "
                  f"flip_y={c['flip_y']}")
        )

    def run(self) -> None:
        self.root.mainloop()


if __name__ == "__main__":
    App().run()
