"""Re-read each candidate address LIVE and keep only those that still
hold the same A/C value (within 1.0 units). These are the candidates
that survived GC = sit in a long-lived object like the player's Hero.

Usage:
    python verify_stable.py
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, ".")
from probe import attach

REPORT = Path("diff_A_B_C.txt")
LINE_RX = re.compile(
    r"0x([0-9a-fA-F]+)\s+A=\(\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*\)\s+"
    r"B=\(\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*\)"
    r".*?C=\(\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*,\s*([-+0-9.]+)\s*\)"
)


def main() -> int:
    pm, _ = attach()
    candidates = []
    for ln in REPORT.read_text().splitlines():
        m = LINE_RX.match(ln.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        ax, ay, az = float(m.group(2)), float(m.group(3)), float(m.group(4))
        bx, by, bz = float(m.group(5)), float(m.group(6)), float(m.group(7))
        cx, cy, cz = float(m.group(8)), float(m.group(9)), float(m.group(10))
        candidates.append((addr, (ax, ay, az), (bx, by, bz), (cx, cy, cz)))

    print(f"# loaded {len(candidates)} candidates from {REPORT}")

    survivors = []
    for addr, (ax, ay, az), bxyz, (cx, cy, cz) in candidates:
        try:
            data = pm.read_bytes(addr, 12)
        except Exception:
            continue
        if len(data) != 12:
            continue
        try:
            lx, ly, lz = struct.unpack("<3f", data)
        except struct.error:
            continue
        # Stable if live values ~= A and ~= C (within 1 unit)
        if (abs(lx - ax) < 1.0 and abs(ly - ay) < 1.0 and abs(lz - az) < 1.0 and
            abs(lx - cx) < 1.0 and abs(ly - cy) < 1.0 and abs(lz - cz) < 1.0):
            # Also: require the live values to look like real positions
            # (not tiny integers / not absurdly large)
            mags = [abs(lx), abs(ly), abs(lz)]
            if 0.5 < max(mags) < 5000 and not all(int(m) == m for m in mags):
                survivors.append((addr, (lx, ly, lz), (ax, ay, az), bxyz))

    print(f"# survivors (live value ~= A & C, plausible magnitude): {len(survivors)}")
    out = Path("verify_stable.txt")
    with out.open("w", encoding="utf-8") as f:
        f.write(f"# {len(survivors)} stable position-like triples\n\n")
        for addr, (lx, ly, lz), (ax, ay, az), (bx, by, bz) in survivors:
            f.write(
                f"0x{addr:016x}  live=({lx:+9.2f},{ly:+9.2f},{lz:+9.2f})  "
                f"A=({ax:+9.2f},{ay:+9.2f},{az:+9.2f})  "
                f"B=({bx:+9.2f},{by:+9.2f},{bz:+9.2f})\n"
            )
    print(f"# wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
