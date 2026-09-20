"""Extract UI icon assets from res.pak into research/icons/.

The game ships its POI / map icons under UI/icons/ and UI/Editor/. We
pull the ones we render onto the minimap, plus the player arrow for a
future hero-marker option.

Output: research/icons/<basename>.png (gitignored - the game owns them).
"""
from __future__ import annotations

import os
from pathlib import Path

from pak_extract import open_pak       # type: ignore[import-not-found]

GAME_DIR  = Path(os.environ.get("FAREVER_DIR",
                 r"C:\Program Files (x86)\Steam\steamapps\common\Farever"))
PROJ_DIR  = Path(__file__).resolve().parents[1]
RES_PAK   = GAME_DIR / "res.pak"
OUT_DIR   = PROJ_DIR / "research" / "icons"

WANTED = [
    "UI/icons/activities.png",
    "UI/icons/icon_mapInformation_atlas_128PX.png",
    "UI/Editor/activityMarker.png",
    "UI/Placeholder/PlayerMapArrow.png",
]


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    buf, entries, data_section_start = open_pak(RES_PAK)
    by_path = {p: (sz, pos) for (p, sz, pos) in entries}
    for w in WANTED:
        if w not in by_path:
            print(f"!! missing: {w}")
            continue
        sz, pos = by_path[w]
        data = buf[data_section_start + pos : data_section_start + pos + sz]
        dst = OUT_DIR / Path(w).name
        dst.write_bytes(data)
        print(f"  -> {dst.relative_to(PROJ_DIR)} ({sz:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
