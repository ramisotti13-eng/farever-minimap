"""Extract the UI/icons PNGs from res.pak to data/atlases/.

The DLL uses the path string embedded in each CDB gfx record
(e.g. "UI/icons/atlas_class_Mage_96PX.png") to locate the right
atlas at runtime, so we mirror the in-pak directory layout under
the live data folder + the release-staging copy.

It reduces that path to a BASENAME and looks for it directly under
data/atlases/UI/icons/, so everything lands in that one flat folder
regardless of where it sat in the pak.

We used to take only names starting with "atlas_", which is not how the
game names all of its sheets: the four talent atlases are "Mage_talent.png"
and friends, and the world-activity markers are "activities.png". Those
records resolved fine and then drew nothing, because the file had never
been shipped (#105, #107). Take every sheet directly under UI/icons/
instead, and let the gfx records decide which ones matter.

Run:  python tools/extract_all_atlases.py
"""
from __future__ import annotations

import shutil
import os
from pathlib import Path

from pak_extract import open_pak  # type: ignore[import-not-found]

GAME_DIR     = Path(os.environ.get("FAREVER_DIR",
                    r"C:\Program Files (x86)\Steam\steamapps\common\Farever"))
PROJ_DIR     = Path(__file__).resolve().parents[1]
RES_PAK      = GAME_DIR / "res.pak"

# Everything directly under UI/icons/. The subfolders (Classes/,
# Steam_Achievments/) are menu chrome that no gfx record points at, and a flat
# destination cannot represent them anyway.
WANTED_DIR = "UI/icons/"
TARGETS = [
    GAME_DIR / "data" / "atlases",
    PROJ_DIR / "release-staging" / "farever-minimap-dps-0.2" / "data" / "atlases",
]


def main() -> int:
    buf, entries, data_section_start = open_pak(RES_PAK)
    matches = [(p, sz, pos) for (p, sz, pos) in entries
               if p.startswith(WANTED_DIR) and p.endswith(".png")
               and "/" not in p[len(WANTED_DIR):]]
    matches.sort()

    print(f"found {len(matches)} atlas entries")
    payloads: list[tuple[str, bytes]] = []
    for p, sz, pos in matches:
        data = buf[data_section_start + pos : data_section_start + pos + sz]
        payloads.append((p, data))
        print(f"  {p}  {sz:>10,} bytes")

    for root in TARGETS:
        if not root.parent.parent.exists():
            print(f"!! skip {root} (parent missing)")
            continue
        for p, data in payloads:
            dst = root / p
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
        print(f"-> wrote {len(payloads)} files under {root}")

    # The minimap draws its activity markers straight from data/icons/, not
    # from data/atlases/, so that copy has to be refreshed too. It had drifted:
    # the shipped one predated four markers the game added in row 2 (chest,
    # scout eye, purple eye, flame crest), so a marker whose CDB record points
    # there drew nothing at all (#107). Keep it in sync from the same source.
    marker = dict((p, d) for p, d in payloads).get("UI/icons/activities.png")
    if marker is None:
        print("!! UI/icons/activities.png not in the pak - markers not updated")
        return 1
    for dst in (GAME_DIR / "data" / "icons" / "activities.png",
                PROJ_DIR / "release" / "data" / "icons" / "activities.png"):
        if not dst.parent.exists():
            print(f"!! skip {dst} (parent missing)")
            continue
        dst.write_bytes(marker)
        print(f"-> refreshed {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
