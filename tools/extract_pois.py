"""Extract per-world POIs (obelisks, respawn points, merchants,
activities) from Farever's gameplayData prefabs.

Each tile prefab in `Level/World/<World>.dat/gameplayData/L0_<col>_<row>.prefab`
is plain JSON: a tree of `children`, where each child has an `x/y/z`
giving its world-space position. Children we treat as POIs:

  - reference with name == "Obelisk"             -> kind = obelisk
  - reference with name in {"RespawnPoint", "RespawnZone"} -> kind = respawn
  - reference with name == "WanderingMerchant"   -> kind = merchant
  - object with $cdbtype == "activity"           -> kind = activity
      sub-category = `inherit` value (FightStone, WorldElite, ...)
      label = texts.name

Output: research/pois_<World>.json, an array of
    {"kind": str, "subkind": str?, "name": str?, "id": str?,
     "x": float, "y": float, "z": float, "source_tile": "L0_+X_+Y"}

Usage:
    python tools/extract_pois.py [--world W1_Siagarta]
"""
from __future__ import annotations

import argparse
import json
import re
import os
from pathlib import Path

from pak_extract import open_pak    # type: ignore[import-not-found]
from hbson import loads_any         # type: ignore[import-not-found]

GAME_DIR = Path(os.environ.get("FAREVER_DIR",
                r"C:\Program Files (x86)\Steam\steamapps\common\Farever"))
PROJ_DIR = Path(__file__).resolve().parents[1]
MAP_PAK  = GAME_DIR / "res.map.pak"

# Match reference children by the SOURCE prefab they reference (the
# `name` field is often unique per instance like "RespawnPoint_7"; the
# `source` always points at a fixed prefab path).
REFERENCE_KINDS = [
    ("Gameplay/Elements/Obelisk.prefab",                "obelisk"),
    ("Gameplay/Elements/RespawnPoint.prefab",           "respawn"),
    ("Gameplay/Elements/RespawnZone.prefab",            "respawn_zone"),
    ("/WanderingMerchant.prefab",                       "merchant"),
    ("Gameplay/Prefabs/GameplayBase/InstanceOrb.prefab", "dungeon"),
    # Completionist collectibles + farming nodes (Phase 1: static).
    ("/RedOrb_World.prefab",                            "red_orb"),
    ("Gameplay/Elements/WorldChest.prefab",             "chest"),
    ("Gameplay/Elements/Recipe_Chest.prefab",           "chest"),
    ("Elements/Activities/OrbChest.prefab",             "chest"),
    ("Elements/Activities/VaultChest.prefab",           "chest"),
    ("Elements/Activities/CampChest.prefab",            "chest"),
    ("/Plants/",                                        "plant"),
    ("/Ores/",                                          "ore"),
]


def reference_kind(source: str) -> str | None:
    for needle, kind in REFERENCE_KINDS:
        if needle in source:
            return kind
    return None

TILE_RE = re.compile(r"L\d+_[+-]\d+_[+-]\d+\.prefab$")


def collect_from_node(node: dict, tile: str, out: list[dict]) -> None:
    """Walk one prefab tree, append POI rows for each interesting node."""
    if not isinstance(node, dict):
        return

    node_type = node.get("type")
    name = node.get("name")
    src  = node.get("source", "")
    x    = node.get("x")
    y    = node.get("y")
    z    = node.get("z")

    # Reference-typed children (Obelisk, RespawnPoint, ...).
    if node_type == "reference" and x is not None and y is not None:
        kind = reference_kind(src)
        if kind is not None:
            # Dungeons carry their target instance under props.props.
            ref_props = (node.get("props") or {}).get("props") or {}
            target = ref_props.get("targetActivity")
            row_name = target or name
            row_id   = (node.get("props") or {}).get("id")
            out.append({
                "kind":     kind,
                "subkind":  None,
                "name":     row_name,
                "id":       row_id,
                "zone":     (node.get("props") or {}).get("zoneBaked"),
                "x":        float(x),
                "y":        float(y),
                "z":        float(z) if z is not None else 0.0,
                "source":   src,
                "source_tile": tile,
            })

    # Object-typed children flagged as $cdbtype activity.
    # For "object" nodes, gameplay metadata lives one level deeper in
    # the `props` field (props.inherit, props.id, props.texts.name).
    if node_type == "object":
        props = node.get("props") or {}
        if props.get("$cdbtype") == "activity":
            texts = props.get("texts") or {}
            out.append({
                "kind":     "activity",
                "subkind":  props.get("inherit"),
                "name":     texts.get("name") or name,
                "id":       props.get("id"),
                "zone":     props.get("zoneBaked"),
                "x":        float(x) if x is not None else 0.0,
                "y":        float(y) if y is not None else 0.0,
                "z":        float(z) if z is not None else 0.0,
                "source":   src,
                "source_tile": tile,
            })

    # Recurse: prefab trees nest under "children".
    children = node.get("children")
    if isinstance(children, list):
        for c in children:
            collect_from_node(c, tile, out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--world", default="W1_Siagarta")
    args = ap.parse_args()

    world = args.world
    out_path = PROJ_DIR / "research" / f"pois_{world}.json"

    prefix = f"Level/World/{world}.dat/gameplayData/"

    buf, entries, data_start = open_pak(MAP_PAK)
    pois: list[dict] = []
    n_prefabs = 0
    for path, sz, pos in entries:
        if not path.startswith(prefix) or not path.endswith(".prefab"):
            continue
        m = TILE_RE.search(path)
        tile = m.group(0)[:-len(".prefab")] if m else path
        raw = buf[data_start + pos : data_start + pos + sz]
        try:
            # Prefabs were plain JSON up to the 2026-05 builds and are
            # HBSON (binary) since 2026-07; loads_any handles both.
            tree = loads_any(raw)
        except Exception as e:
            print(f"!! {tile}: {e}")
            continue
        collect_from_node(tree, tile, pois)
        n_prefabs += 1

    # Summarize.
    by_kind: dict[str, int] = {}
    for p in pois:
        k = p["kind"] + (f"/{p['subkind']}" if p.get("subkind") else "")
        by_kind[k] = by_kind.get(k, 0) + 1
    print(f"scanned {n_prefabs} prefabs, found {len(pois)} POIs:")
    for k in sorted(by_kind):
        print(f"  {by_kind[k]:4d}  {k}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(pois, indent=2, ensure_ascii=False))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
