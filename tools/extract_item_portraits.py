"""Extract every UI/Portraits/Items/<Category>/*.prefab.png from
res.pak. These are the pre-rendered 256x256 PNGs the game uses for
inventory / vendor / chest-preview icons -- the same image you see
when you hover over an item in the Heaps engine UI.

The captured ``gfx.file`` string we read off an Item record is the
basename of one of these files (e.g.
"Character_Weapon_DualAxes_DA_Human_KoboldBone.prefab.png"), so we
flatten the per-category subfolders into one ``data/portraits/``
directory. We verified at extract time that the 831 portrait
basenames are unique across categories.

Run:  python tools/extract_item_portraits.py
"""
from __future__ import annotations

import os
from pathlib import Path

from pak_extract import open_pak  # type: ignore[import-not-found]

GAME_DIR = Path(os.environ.get("FAREVER_DIR",
                r"C:\Program Files (x86)\Steam\steamapps\common\Farever"))
PROJ_DIR = Path(__file__).resolve().parents[1]
RES_PAK  = GAME_DIR / "res.pak"

WANTED_PREFIX = "UI/Portraits/Items/"

TARGETS = [
    GAME_DIR / "data" / "portraits",
    PROJ_DIR / "release-staging" / "farever-minimap-dps-0.4" / "data" / "portraits",
    PROJ_DIR / "release-staging" / "farever-minimap-dps-0.4.1" / "data" / "portraits",
    PROJ_DIR / "release-staging" / "farever-minimap-dps-0.4.2" / "data" / "portraits",
]


def main() -> int:
    buf, entries, data_section_start = open_pak(RES_PAK)
    matches = [(p, sz, pos) for (p, sz, pos) in entries
               if p.startswith(WANTED_PREFIX) and p.endswith(".png")]
    matches.sort()

    print(f"found {len(matches)} portrait entries")

    # Collision check -- we promised unique basenames upstream.
    seen: dict[str, str] = {}
    for p, _sz, _pos in matches:
        base = p.rsplit("/", 1)[-1]
        if base in seen:
            print(f"!! basename collision: {base}\n   {seen[base]}\n   {p}")
        seen[base] = p

    payloads: list[tuple[str, bytes]] = []
    for p, sz, pos in matches:
        data = buf[data_section_start + pos : data_section_start + pos + sz]
        base = p.rsplit("/", 1)[-1]
        payloads.append((base, data))

    total_bytes = sum(len(d) for _, d in payloads)
    print(f"total payload: {total_bytes / (1024 * 1024):.1f} MB")

    for root in TARGETS:
        if not root.parent.parent.exists():
            print(f"!! skip {root} (parent missing)")
            continue
        root.mkdir(parents=True, exist_ok=True)
        for base, data in payloads:
            (root / base).write_bytes(data)
        print(f"-> wrote {len(payloads)} files under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
