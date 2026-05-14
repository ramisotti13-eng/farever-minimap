"""Find live `ent.Hero` instances in a running Farever process by
scanning the GC heap for objects matching the Hero memory signature
(derived from hlbc_parse.py output).

Hero instance layout (1584 bytes, offsets from instance start):
  +  0  ptr     hl_type*                       (any non-null pointer)
  +  8  bool    removed                        (0 or 1)
  + 16  ptr     ownerPlayer : st.Player        (non-null for live)
  + 32  i64     __uid                          (any int)
  + 48  ptr     __host : hxbit.NetworkHost     (non-null for live)
  + 88  ptr     layer : st.GameLayer
  +144  f64     posx                           ★ -2000 .. +2000
  +152  f64     posy                           ★ -2000 .. +2000
  +160  f64     posz                           ★    0  ..  +500
  +168  f64     rotationZ (yaw)                ★  -7  ..   +7
  +176  ptr     position : h3d.VectorImpl      (non-null)
  +208  ptr     obj : h3d.scene.Object         (non-null = visible model)

The 4 doubles at +144..+176 are the strongest signature: very few
unrelated 32-byte spans of memory naturally hold these specific value
ranges.

Usage:
    python find_hero.py            # one-shot scan
    python find_hero.py loop       # poll the local player every 200 ms
    python find_hero.py loop <hex> # poll a specific address (skips scan)
"""
from __future__ import annotations

import json
import math
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, ".")
from probe import attach, iter_committed_regions

PROCESS_NAME = "Farever.exe"
LIVE_FILE = Path("../research/live_position.json")

# Hero field offsets (from hlbc_parse.py for build hash on 2026-05-14)
OFF_POSX        = 144
OFF_POSY        = 152
OFF_POSZ        = 160
OFF_ROTZ        = 168
OFF_POSITION    = 176       # pointer to h3d.VectorImpl
OFF_OBJ         = 208       # pointer to h3d.scene.Object (visible model)
OFF_OWNERPLAYER = 16
OFF_HOST        = 48

# Plausible ranges. Widened 2026-05-15: world W1 spans up to ~6 tiles
# x 1024 world units; outer-zone coords can comfortably exceed 3000.
RX = (-10000.0, 10000.0)
RY = (-10000.0, 10000.0)
RZ = (  -500.0,  1500.0)
RROT = (-math.pi * 2 - 0.5, math.pi * 2 + 0.5)


def is_userland_ptr(v: int) -> bool:
    return 0x0000020000000000 <= v <= 0x00007FFFFFFFFFFF


USERLAND_LO = 0x0000000100000000   # anything above the low 4 GB
USERLAND_HI = 0x00007FFFFFFFFFFF


def _scan_region_numpy(base: int, data: bytes):
    """Vectorised version of candidate_score over one memory region.

    Returns a list of (addr, score, (px, py, pz, rotz)) tuples that pass
    the hard checks. Roughly 50-100x faster than the per-offset python
    loop on a multi-GB heap because numpy does the filtering in C.
    """
    # We need: at byte offset (off + 0..224) various fields, off 8-aligned.
    # Build numpy views of the whole region as float64 / uint64 / uint8;
    # the i-th element of the float64 view starts at byte 8*i, so an
    # 8-aligned candidate at byte offset 8*j has its first hero-field at
    # element j and the 4 position floats at elements j+18..j+21 (since
    # OFF_POSX = 144 = 8*18).
    n = len(data)
    if n < 1600:
        return []
    arr8  = np.frombuffer(data, dtype=np.uint8)
    arr64 = np.frombuffer(data, dtype=np.uint64,  count=n // 8)
    arrf  = np.frombuffer(data, dtype=np.float64, count=n // 8)
    M = arr64.size

    def field_u64(field_byte_off):
        # element index = (8*j + field_byte_off) / 8 = j + field_byte_off/8
        k = field_byte_off // 8
        return arr64[k : k + (M - k)]

    def field_f64(field_byte_off):
        k = field_byte_off // 8
        return arrf[k : k + (M - k)]

    # Truncate everything to the candidates that fit within the region.
    # A hero is 1584 bytes, so the last possible 'off' (in 8-aligned
    # indices j) is (n - 1584) / 8.
    max_j = (n - 1584) // 8
    if max_j <= 0:
        return []

    htype = field_u64(0)[:max_j]
    owner = field_u64(OFF_OWNERPLAYER)[:max_j]
    host  = field_u64(OFF_HOST)[:max_j]
    posp  = field_u64(OFF_POSITION)[:max_j]

    posx = field_f64(OFF_POSX)[:max_j]
    posy = field_f64(OFF_POSY)[:max_j]
    posz = field_f64(OFF_POSZ)[:max_j]
    rotz = field_f64(OFF_ROTZ)[:max_j]

    removed = arr8[0 + 8 :: 8][:max_j]  # byte (8*j + 8) for each j

    # Hard filters — all of these must hold.
    mask = (
        (posx >= RX[0]) & (posx <= RX[1]) &
        (posy >= RY[0]) & (posy <= RY[1]) &
        (posz >= RZ[0]) & (posz <= RZ[1]) &
        (rotz >= RROT[0]) & (rotz <= RROT[1]) &
        ~((np.abs(posx) < 0.01) & (np.abs(posy) < 0.01)) &
        (htype >= USERLAND_LO) & (htype <= USERLAND_HI) &
        (owner >= USERLAND_LO) & (owner <= USERLAND_HI) &
        (host  >= USERLAND_LO) & (host  <= USERLAND_HI) &
        (posp  >= USERLAND_LO) & (posp  <= USERLAND_HI) &
        (removed <= 1)
    )

    # Reject "obviously synthetic" positions: all three coords near
    # exact small integers (canonical/identity transforms, normalised
    # basis vectors, type counters stored as Vec3, ...). Real player
    # world positions are large floats with non-trivial fractional
    # digits.
    EPS = 1e-6
    near_int = lambda a: np.abs(a - np.round(a)) < EPS
    small    = lambda a: np.abs(a) < 10.0
    mask &= ~(near_int(posx) & near_int(posy) & near_int(posz) &
              small(posx) & small(posy) & small(posz))

    js = np.where(mask)[0]
    if js.size == 0:
        return []

    # Score: 1.0 baseline; bonus for non-zero rotation and plausible Z.
    s = np.ones(js.size, dtype=np.float64)
    s += np.where((posz[js] > 0.0) & (posz[js] < 300.0), 0.5, 0.0)
    s += np.where(np.abs(rotz[js]) > 0.001, 0.2, 0.0)

    out = []
    pxs = posx[js]; pys = posy[js]; pzs = posz[js]; rzs = rotz[js]
    addrs = base + (js.astype(np.uint64) * 8)
    for i in range(js.size):
        out.append((int(addrs[i]), float(s[i]),
                    (float(pxs[i]), float(pys[i]),
                     float(pzs[i]), float(rzs[i]))))
    return out


def scan_for_heroes(pm):
    print("scanning heap for Hero candidates (numpy)...", file=sys.stderr)
    t0 = time.time()
    results: list[tuple[int, float, tuple]] = []
    # HashLink's heap base address shifts per process launch (ASLR), so
    # don't hardcode it. Instead include any committed read-write region
    # >= 1 MB. PAGE_READWRITE = 0x04, PAGE_WRITECOPY = 0x08, the
    # _WRITE variants of execute (0x40, 0x80) also count.
    WRITABLE_PROTECT = 0x04 | 0x08 | 0x40 | 0x80
    bytes_scanned = 0
    for base, size, prot in iter_committed_regions(pm):
        if size < 1 << 16:  # < 64 KB — covers small HashLink heap chunks too
            continue
        if not (prot & WRITABLE_PROTECT):
            continue
        try:
            data = pm.read_bytes(base, size)
        except Exception:
            continue
        bytes_scanned += size
        results.extend(_scan_region_numpy(base, data))
    dt = time.time() - t0
    print(f"  {dt:.1f}s, {bytes_scanned/1e9:.2f} GB scanned, "
          f"{len(results)} candidate(s)", file=sys.stderr)
    results.sort(key=lambda r: -r[1])
    return results


def read_position(pm, hero_addr: int):
    b = pm.read_bytes(hero_addr + OFF_POSX, 32)
    return struct.unpack("<4d", b)


def loop(pm, hero_addr: int):
    """Continuously poll the given Hero address and write to live_position.json."""
    LIVE_FILE.parent.mkdir(parents=True, exist_ok=True)
    last = (0.0, 0.0, 0.0)
    print(f"polling Hero @ 0x{hero_addr:016x}  (Ctrl+C to stop)")
    while True:
        try:
            x, y, z, rz = read_position(pm, hero_addr)
        except Exception as e:
            print(f"!! read failed: {e}", file=sys.stderr)
            time.sleep(0.5)
            continue
        # Best-effort atomic write: write to .tmp, then replace. The
        # replace can fail under Windows when a reader (the DLL, tk
        # viewer) has the file open at exactly the wrong moment.
        # Retry a few times before giving up on this tick.
        tmp = LIVE_FILE.with_suffix(".json.tmp")
        payload = json.dumps({
            "x": x, "y": y, "z": z, "rot_z": rz,
            "addr": hex(hero_addr),
            "ts": time.time(),
        })
        tmp.write_text(payload)
        for _ in range(20):
            try:
                tmp.replace(LIVE_FILE)
                break
            except PermissionError:
                time.sleep(0.002)
        else:
            # If even the retry loop loses, fall back to a direct write
            # so we don't drop every update.
            LIVE_FILE.write_text(payload)
        if any(abs(a - b) > 0.05 for a, b in zip((x, y, z), last)):
            print(f"  ({x:+9.3f}, {y:+9.3f}, {z:+9.3f})  rot={rz:+.3f} rad")
            last = (x, y, z)
        time.sleep(0.1)


CANDIDATES_FILE = Path("../research/hero_candidates.json")


def save_candidates(results):
    CANDIDATES_FILE.parent.mkdir(parents=True, exist_ok=True)
    CANDIDATES_FILE.write_text(json.dumps([
        {"addr": hex(a), "score": s,
         "x": p[0], "y": p[1], "z": p[2], "rot_z": p[3]}
        for a, s, p in results
    ], indent=2))


def show_top(results, n=20):
    print(f"\nTop {min(n, len(results))} candidates by score:")
    print(f"  {'addr':<22} {'score':>5}  position                          yaw")
    for addr, sc, (px, py, pz, rz) in results[:n]:
        print(f"  0x{addr:016x}  {sc:>5.2f}  ({px:+9.2f},{py:+9.2f},{pz:+7.2f})  {rz:+.3f}")


def cmd_find_and_show(pm):
    results = scan_for_heroes(pm)
    if not results:
        print("no Hero candidates found — is the game in-world (not main menu)?",
              file=sys.stderr)
        return 1
    show_top(results)
    save_candidates(results)
    print(f"\nsaved all {len(results)} candidates to {CANDIDATES_FILE}")
    return 0


def cmd_motion_scan(pm):
    """Scan, then sample positions for 8 seconds, find the candidate
    with the longest path length (or noisiest yaw)."""
    results = scan_for_heroes(pm)
    if not results:
        print("no candidates from initial scan", file=sys.stderr)
        return 1
    print(f"\n  {len(results)} initial candidates")

    addrs = [r[0] for r in results]
    n_samples = 16
    interval = 0.5
    print(f"\nPLEASE WALK + TURN IN GAME for {n_samples * interval:.0f}s ...")

    # samples[a] = list of (x, y, rz)
    samples = {a: [] for a in addrs}
    failures = 0
    for s in range(n_samples):
        for a in addrs:
            try:
                x, y, z, rz = read_position(pm, a)
                samples[a].append((x, y, rz))
            except Exception:
                failures += 1
        time.sleep(interval)
    print(f"sampled, {failures} read failures")

    moved = []
    for a, seq in samples.items():
        if len(seq) < 2: continue
        # Reject candidates whose values blow up (garbage memory).
        ok = True
        for (sx, sy, srz) in seq:
            if (not math.isfinite(sx) or not math.isfinite(sy)
                or not math.isfinite(srz)
                or abs(sx) > 10000 or abs(sy) > 10000 or abs(srz) > 10):
                ok = False
                break
        if not ok: continue
        path = 0.0
        rmin = rmax = seq[0][2]
        for (px, py, prz), (cx, cy, crz) in zip(seq, seq[1:]):
            step = math.hypot(cx - px, cy - py)
            if step > 50.0:  # teleport — not the player
                ok = False
                break
            path += step
            if crz < rmin: rmin = crz
            if crz > rmax: rmax = crz
        if not ok: continue
        yaw_range = rmax - rmin
        if path > 1.0 or yaw_range > 0.5:
            moved.append((a, path, yaw_range, seq[-1]))
    moved.sort(key=lambda m: -(m[1] + m[2] * 5))
    print(f"\n{len(moved)} candidates with motion:")
    for a, path, yaw_r, last in moved[:25]:
        x, y, rz = last
        print(f"  0x{a:016x}  path={path:6.2f}  yaw_r={yaw_r:5.2f}  last=({x:+9.2f},{y:+9.2f})  rz={rz:+.3f}")
    if moved:
        addr = moved[0][0]
        print(f"\ntop mover: 0x{addr:016x}")
        print(f"start loop with:  python find_hero.py loop 0x{addr:x}")
    return 0


def cmd_verify(pm, hex_addr: str):
    """Read a candidate Hero address several times and print its
    position. Lets the user walk in-game and confirm by motion."""
    addr = int(hex_addr, 0)
    print(f"polling 0x{addr:016x} for 5 seconds (walk in-game to confirm)...")
    last = None
    for _ in range(25):
        try:
            x, y, z, rz = read_position(pm, addr)
        except Exception as e:
            print(f"  read failed: {e}", file=sys.stderr)
            return 1
        delta = ""
        if last is not None:
            dx, dy = x - last[0], y - last[1]
            if dx*dx + dy*dy > 0.01:
                delta = f"  [moved by {math.hypot(dx, dy):.2f}]"
        print(f"  ({x:+9.3f},{y:+9.3f},{z:+7.2f})  rot={rz:+.3f}{delta}")
        last = (x, y, z, rz)
        time.sleep(0.2)
    return 0


def main() -> int:
    pm, _ = attach()
    if len(sys.argv) < 2:
        return cmd_find_and_show(pm)
    cmd = sys.argv[1]
    if cmd == "loop":
        if len(sys.argv) >= 3:
            addr = int(sys.argv[2], 0)
        else:
            results = scan_for_heroes(pm)
            if not results:
                print("no candidates; pass an address explicitly", file=sys.stderr)
                return 1
            save_candidates(results)
            show_top(results, 10)
            addr, _, _ = results[0]
            print(f"\nusing top candidate 0x{addr:016x}")
        try:
            loop(pm, addr)
        except KeyboardInterrupt:
            print("\nstopped.")
        return 0
    if cmd == "motion":
        return cmd_motion_scan(pm)
    if cmd == "verify" and len(sys.argv) >= 3:
        return cmd_verify(pm, sys.argv[2])
    print(f"unknown command: {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
