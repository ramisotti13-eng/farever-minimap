"""Live player-position tracker — polls the address we discovered and
writes it to research/live_position.json every 100 ms. The FOW viewer
(when extended) can read that file to render a real-time player marker.

Usage:
    python track_pos.py           # tail mode: prints + saves every 100ms
    python track_pos.py once      # single read
    python track_pos.py find      # auto-rediscover the address if it
                                  # moved (uses the surrounding "tag"
                                  # bytes we recorded the first time)
"""
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, ".")
from probe import attach

# Live PlayerMarker.worldPos vec4 (x, y, z, w=1) discovered via
# map-open snap-diff on 2026-05-14. Address valid only for the current
# game launch + while the world map is open (the PlayerMarker exists
# only then). When the map is closed the object may be freed and this
# address will hold garbage. Re-run discovery after each game restart.
POS_ADDR = 0x000002af9e4aec70
OUT_FILE = Path("../research/live_position.json")


def read_pos(pm, addr=POS_ADDR):
    """Read (x, y, z) as 3 contiguous f32s. The 4th f32 at the address
    is the homogeneous w (always 1.0) and we just skip it."""
    b = pm.read_bytes(addr, 12)
    return struct.unpack("<3f", b)


def cmd_once(pm):
    x, y, z = read_pos(pm)
    print(f"({x:+.3f}, {y:+.3f}, {z:+.3f})")
    OUT_FILE.write_text(json.dumps({"x": x, "y": y, "z": z, "addr": hex(POS_ADDR)}))
    return 0


def cmd_loop(pm):
    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    last_print = 0.0
    last_pos = None
    print("Tracking — move in-game, Ctrl+C to stop. Updates written to "
          f"{OUT_FILE}")
    while True:
        try:
            x, y, z = read_pos(pm)
        except Exception as e:
            print(f"!! read failed: {e}", file=sys.stderr)
            time.sleep(0.5)
            continue
        OUT_FILE.write_text(json.dumps({"x": x, "y": y, "z": z,
                                        "addr": hex(POS_ADDR),
                                        "ts": time.time()}))
        now = time.time()
        # throttle stdout to once every 250 ms; only print when moved or
        # every ~2 seconds at rest
        if now - last_print > 0.25:
            moved = last_pos is None or any(abs(a - b) > 0.05 for a, b in zip((x, y, z), last_pos))
            if moved or now - last_print > 2.0:
                tag = "*" if moved else "."
                print(f"  {tag} ({x:+9.3f}, {y:+9.3f}, {z:+9.3f})")
                last_print = now
                last_pos = (x, y, z)
        time.sleep(0.05)


def main() -> int:
    pm, _ = attach()
    cmd = sys.argv[1] if len(sys.argv) >= 2 else "loop"
    if cmd == "once":
        return cmd_once(pm)
    if cmd == "loop":
        try:
            return cmd_loop(pm)
        except KeyboardInterrupt:
            print("\nstopped.")
            return 0
    print(f"unknown command: {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
