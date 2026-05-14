"""Snapshot/diff scanner — find live player coordinates by motion.

Usage flow:
    # 1) player stands still
    python snapdiff.py snap A

    # 2) player walks in a straight line for ~10 seconds
    python snapdiff.py snap B

    # 3) find every 4-byte float that changed by an amount consistent with
    #    player motion, output the most likely candidates
    python snapdiff.py diff A B

    # 4) (optional) take a third snapshot after walking back to the start,
    #    keeping only floats that moved AND came back
    python snapdiff.py diff A B C

The snapshots are written as compact .npz files containing one 1-D array
of (addr, f32_value) pairs per scanned region. We only sample 4-byte
aligned floats whose absolute value falls in a "looks like a position"
band — skipping zeros, tiny epsilons, and huge values. That trims the
candidate set from billions to a few hundred thousand.
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

import numpy as np
import pymem

# Cover the typical Windows user-mode heap range. HashLink's GC chunks
# always land somewhere in this band on x64; the exact base depends on
# the launch (different per game restart).
HEAP_LOW  = 0x0000020000000000
HEAP_HIGH = 0x0000040000000000

# Plausible-position band — Farever's poly coords go up to ~1000, so
# expect player coordinates around 0..3000 in either direction.
MIN_ABS = 1.0
MAX_ABS = 5000.0


def iter_heap_regions(pm):
    from probe import iter_committed_regions
    for base, size, prot in iter_committed_regions(pm):
        if base < HEAP_LOW or base + size > HEAP_HIGH:
            continue
        yield base, size, prot


def take_snapshot(pm) -> tuple[np.ndarray, np.ndarray]:
    """Return parallel arrays (addrs uint64, values float32) of every
    4-byte aligned float in the heap range that's within the position
    band."""
    addrs_chunks: list[np.ndarray] = []
    vals_chunks:  list[np.ndarray] = []
    t0 = time.time()
    total_bytes = 0
    for base, size, _ in iter_heap_regions(pm):
        if size < 16:
            continue
        try:
            data = pm.read_bytes(base, size)
        except Exception:
            continue
        total_bytes += size
        # Round to multiple of 4
        n = len(data) // 4
        if n == 0:
            continue
        arr = np.frombuffer(data[: n * 4], dtype=np.float32)
        absv = np.abs(arr)
        keep = (absv >= MIN_ABS) & (absv <= MAX_ABS) & np.isfinite(arr)
        if not keep.any():
            continue
        idx = np.flatnonzero(keep).astype(np.int64)
        addrs = (base + idx * 4).astype(np.uint64)
        addrs_chunks.append(addrs)
        vals_chunks.append(arr[idx].copy())
    addrs = np.concatenate(addrs_chunks) if addrs_chunks else np.empty(0, np.uint64)
    vals  = np.concatenate(vals_chunks)  if vals_chunks  else np.empty(0, np.float32)
    dt = time.time() - t0
    print(f"# snapshot: {len(addrs):>10,} floats from {total_bytes/(1024**2):.0f} MB in {dt:.1f}s")
    return addrs, vals


def cmd_snap(name: str) -> int:
    pm = pymem.Pymem("Farever.exe")
    addrs, vals = take_snapshot(pm)
    out = Path(f"snapshot_{name}.npz")
    np.savez_compressed(out, addrs=addrs, vals=vals)
    print(f"# wrote {out}")
    return 0


def cmd_diff(*names: str) -> int:
    if len(names) < 2:
        print("need at least two snapshot names", file=sys.stderr)
        return 2
    snaps = []
    for n in names:
        d = np.load(f"snapshot_{n}.npz")
        snaps.append((d["addrs"], d["vals"]))
        print(f"# loaded snapshot_{n}: {len(d['addrs']):,} floats")

    # Use snapshot A's address set as the canonical universe; we only
    # care about positions present in A.
    a_addrs, a_vals = snaps[0]
    print(f"# universe size: {len(a_addrs):,}")

    # For each later snapshot, build a dict address->value and keep
    # only addresses present in A.
    aligned: list[np.ndarray] = [a_vals]
    for s_idx, (b_addrs, b_vals) in enumerate(snaps[1:], 1):
        b_map = dict(zip(b_addrs.tolist(), b_vals.tolist()))
        b_aligned = np.fromiter(
            (b_map.get(int(a), np.nan) for a in a_addrs.tolist()),
            dtype=np.float32, count=len(a_addrs),
        )
        aligned.append(b_aligned)
        present = np.sum(np.isfinite(b_aligned))
        print(f"# snapshot {names[s_idx]}: {present:,} addresses in common with A")

    # Heuristic for a player walking in a straight line for ~10s:
    #   - 1 or 2 of the 3 axes changed by 30..300 units (walking distance)
    #   - the other axis (likely height/Y) stayed within ~10 units
    # We also keep float values in a plausible world-position band so we
    # rule out random small-magnitude scratch floats.
    a = aligned[0]
    b = aligned[1]
    delta_ab = b - a
    abs_d = np.abs(delta_ab)
    moved = np.isfinite(delta_ab) & (abs_d >= 5.0) & (abs_d <= 400.0)
    print(f"# A->B floats that moved by 5..400: {int(moved.sum()):,}")

    if len(aligned) >= 3:
        c = aligned[2]
        # Require: moved at B, but returned to within 10 of A at C
        moved &= np.isfinite(c) & (np.abs(c - a) < 10.0)
        print(f"# A->B moved, then B->C returned (<10): {int(moved.sum()):,}")

    # Now look for triples of 3 consecutive 4-byte slots where:
    #   - ALL THREE addresses are present in both snapshots
    #   - at least one axis moved by 30..300 (a real walking distance)
    #   - the small-displacement axis (likely height) didn't change crazy
    candidates = np.where(moved)[0]
    addrs = a_addrs[candidates]
    order = np.argsort(addrs)
    addrs_sorted = addrs[order]
    cand_sorted = candidates[order]

    triples: list[tuple[int, int, int]] = []
    for i in range(len(addrs_sorted) - 2):
        if (addrs_sorted[i + 1] - addrs_sorted[i] == 4 and
            addrs_sorted[i + 2] - addrs_sorted[i + 1] == 4):
            triples.append((cand_sorted[i], cand_sorted[i + 1], cand_sorted[i + 2]))
    print(f"# 3-in-a-row triples (all moved): {len(triples)}")

    # Stricter: require ONE axis to have moved 30..300 AND the other two
    # to be smaller (the player walked mostly in one direction).
    strict: list[tuple[int, int, int]] = []
    for (i, j, k) in triples:
        dx, dy, dz = abs(delta_ab[i]), abs(delta_ab[j]), abs(delta_ab[k])
        sorted_d = sorted([dx, dy, dz])
        if sorted_d[2] >= 30.0 and sorted_d[2] <= 300.0 and sorted_d[0] < 15.0:
            strict.append((i, j, k))
    print(f"# strict (1 axis big, 1 axis tiny): {len(strict)}")

    # Write a full report to disk in UTF-8 so we don't fight the Windows
    # console's cp1252.
    out = Path(f"diff_{'_'.join(names)}.txt")
    with out.open("w", encoding="utf-8") as f:
        f.write(f"# diff of snapshots {names}\n")
        f.write(f"# strict 3-axis triples: {len(strict)}\n\n")
        for (i, j, k) in strict[:200]:
            addr = int(a_addrs[i])
            ax, ay, az = a[i], a[j], a[k]
            bx, by, bz = b[i], b[j], b[k]
            dxyz = ((bx - ax) ** 2 + (by - ay) ** 2 + (bz - az) ** 2) ** 0.5
            line = (f"0x{addr:016x}  "
                    f"A=({ax:+8.2f},{ay:+8.2f},{az:+8.2f})  "
                    f"B=({bx:+8.2f},{by:+8.2f},{bz:+8.2f})  "
                    f"|d|={dxyz:6.2f}")
            if len(aligned) >= 3:
                cx, cy, cz = c[i], c[j], c[k]
                line += f"  C=({cx:+8.2f},{cy:+8.2f},{cz:+8.2f})"
            f.write(line + "\n")
    print(f"# wrote {out}  (top {min(200, len(strict))})")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "snap":
        return cmd_snap(sys.argv[2] if len(sys.argv) >= 3 else "A")
    if cmd == "diff":
        return cmd_diff(*sys.argv[2:])
    print(f"unknown command: {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
