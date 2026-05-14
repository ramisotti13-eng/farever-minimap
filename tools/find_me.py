"""Find the *local* player's Hero by filtering Hero candidates on
`Hero.ownerPlayer.isMe == true`.

The HashLink type system (hl_type_obj instances) lives in static
arrays inside libhl.dll's read-only sections; following pointer chains
through it doesn't work because they're referenced by index, not
pointer. So we go through st.Player instead.

st.Player layout (verified via hlbc_parse on 2026-05-14 build):
  +272  HOBJ       hero   : ent.Hero
  +280  HBOOL      isMe   : HBOOL    <- 1 for the local player

ent.Hero layout:
  + 16  HOBJ       ownerPlayer : st.Player
  +144  HF64       posx
"""
from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, ".")
from find_hero import scan_for_heroes
from probe import attach

OFF_OWNERPLAYER = 16        # Hero.ownerPlayer
OFF_HERO_IN_PLAYER = 272    # st.Player.hero  (must == this Hero)
OFF_ISME = 280              # st.Player.isMe
OFF_POSX = 144


def main() -> int:
    pm, _ = attach()
    print("scanning for Hero candidates...", file=sys.stderr)
    results = scan_for_heroes(pm)
    if not results:
        print("no Hero candidates", file=sys.stderr)
        return 1
    print(f"  {len(results)} Hero candidates")

    local_heroes = []
    for addr, _score, _pos in results:
        try:
            owner_bytes = pm.read_bytes(addr + OFF_OWNERPLAYER, 8)
            owner = struct.unpack("<Q", owner_bytes)[0]
            if owner == 0:
                continue
            # Bidirectional check: Player.hero must point back to this Hero.
            player_hero = struct.unpack(
                "<Q", pm.read_bytes(owner + OFF_HERO_IN_PLAYER, 8))[0]
            if player_hero != addr:
                continue
            is_me = pm.read_bytes(owner + OFF_ISME, 1)[0]
            if is_me == 1:
                pos = pm.read_bytes(addr + OFF_POSX, 32)
                x, y, z, rz = struct.unpack("<4d", pos)
                local_heroes.append((addr, owner, (x, y, z, rz)))
        except Exception:
            continue

    print(f"\n{len(local_heroes)} candidate(s) with ownerPlayer.isMe == 1:")
    for addr, owner, (x, y, z, rz) in local_heroes:
        print(f"  Hero @ 0x{addr:016x}  owner=0x{owner:016x}  pos=({x:+9.2f},{y:+9.2f},{z:+7.2f})  rz={rz:+.3f}")

    if not local_heroes:
        print("\nno Hero where ownerPlayer.isMe==1 — try walking around for a few seconds and re-run", file=sys.stderr)
        return 2
    if len(local_heroes) > 1:
        # Multiple matches shouldn't happen for a singleton; report all but pick the
        # first as a fallback.
        print("\nwarning: multiple isMe==1 candidates (unusual)", file=sys.stderr)
    addr = local_heroes[0][0]
    print(f"\nlocal Hero: 0x{addr:016x}")
    if "--loop" in sys.argv:
        from find_hero import loop
        try:
            loop(pm, addr)
        except KeyboardInterrupt:
            print("\nstopped.")
    else:
        print(f"start polling with:  python find_hero.py loop 0x{addr:x}")
        print(f"or in one step:      python find_me.py --loop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
