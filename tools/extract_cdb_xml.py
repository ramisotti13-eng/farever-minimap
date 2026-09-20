"""Extract the Farever CDB (items, bosses, dungeons, chests, ...) from
the localized export XMLs that ship in res.pak.

Each ``lang/export_<lang>.xml`` is Castle DB's localization dump. Its
structure is a flat list of sheets, one per CDB type:

    <cdb>
      <sheet name="item">
        <RecordId>
          <texts.name>...</texts.name>
          <texts.desc>...</texts.desc>
          <texts.flavorDesc>...</texts.flavorDesc>
        </RecordId>
        ...
      </sheet>
      <sheet name="unit"> ... </sheet>
      ...
    </cdb>

That's every entity in the game with its display strings. The structural
side (which LootTable references which Item with what probability) is
still only in hlboot.dat's bytecode init -- this script does NOT touch
that. We produce a flat catalog instead, sufficient for an item / boss
viewer in the mod.

Output: one ``data/cdb_atlas_<lang>.tsv`` per available localization
plus a synthetic ``data/cdb_atlas_en.tsv`` derived from CDB IDs (since
English is the source language and isn't shipped as a separate XML).
The in-game atlas picker shows whichever TSVs are present at load.

TSV format (one row per record, tab-separated, no escaping --- we
strip tabs/newlines from values up front)::

    sheet<TAB>id<TAB>name<TAB>desc<TAB>flavor<TAB>category<TAB>slot
"""
from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
import os
from pathlib import Path

from pak_extract import open_pak  # type: ignore[import-not-found]

GAME_DIR = Path(os.environ.get("FAREVER_DIR",
                r"C:\Program Files (x86)\Steam\steamapps\common\Farever"))
PROJ_DIR = Path(__file__).resolve().parents[1]
RES_PAK  = GAME_DIR / "res.pak"

# The 9 localizations Farever ships in res.pak. English is the source
# language and lives in hlboot.dat literals, not as a separate XML --
# we synthesize an English TSV from CDB IDs in a separate pass below.
ALL_LANGS = ["de", "es", "fr", "ja", "ko", "pl", "pt-BR", "ru", "zh"]

# Sheets we care about for an item / boss atlas. Everything else in
# the XML (icon, gameTerm, notify, option, ...) is UI scaffolding and
# not useful for the atlas viewer.
ATLAS_SHEETS = {
    "item", "itemType",
    "unit", "unitType",
    "element",
    "activity",
    "affinity", "affix", "aptitude", "rarity", "attribute",
    "skill", "statusType",
    "gatherable",
    "zone",
}

# Per-record fields we extract. Castle DB nests these under <texts.*>;
# the XML elements are literally named with dots in them.
FIELDS = {
    "name":   "texts.name",
    "desc":   "texts.desc",
    "flavor": "texts.flavorDesc",
}

# Classify an item ID into a coarse category. Drives the equipment tab
# in the in-game atlas. The slot (right column) is what the WoW-style
# UI shows: "Head", "Legs", "1H Weapon", "Off-hand", etc.
WEAPON_PREFIXES_1H = {
    "Sword", "Mace", "Axe", "Daggers", "Dagger", "Fists",
    "Scepter", "Wand", "Crescent", "Halos",
}
WEAPON_PREFIXES_2H = {
    "GS", "GA", "GM", "Staff", "Spear", "Bow", "Pike",
    "Thrown", "Book",
}
WEAPON_PREFIXES_DUAL = {"DS", "DA", "DM"}
OFFHAND_PREFIXES = {"Shield"}
ARMOR_PREFIXES = {
    "Head", "Shoulders", "Chest", "Hands", "Legs", "Feet",
    "Waist", "Back",
}
ACCESSORY_PREFIXES = {"Finger", "Necklace", "Trinket"}
GLIDER_PREFIXES = {"Glider"}
MOUNT_PREFIXES = {"Mount"}


def classify(item_id: str) -> tuple[str, str]:
    """Returns (category, slot). Category drives tab choice; slot is
    the human-readable equipment slot or item type. Slot strings are
    intentionally English -- the same value is used regardless of the
    chosen display language, since the mod UI is English-only."""
    prefix = item_id.split("_", 1)[0]
    if prefix in WEAPON_PREFIXES_1H:    return ("weapon", f"{prefix} (1H)")
    if prefix in WEAPON_PREFIXES_2H:    return ("weapon", f"{prefix} (2H)")
    if prefix in WEAPON_PREFIXES_DUAL:  return ("weapon", f"{prefix} (dual)")
    if prefix in OFFHAND_PREFIXES:      return ("weapon", "Shield")
    if prefix in ARMOR_PREFIXES:        return ("armor", prefix)
    if prefix in ACCESSORY_PREFIXES:    return ("accessory", prefix)
    if prefix in GLIDER_PREFIXES:       return ("glider", "Glider")
    if prefix in MOUNT_PREFIXES:        return ("mount", "Mount")
    return ("other", prefix)


def extract_lang_xml(buf: bytes, entries, data_start: int, lang: str
                     ) -> bytes | None:
    target = f"lang/export_{lang}.xml"
    for path, size, pos in entries:
        if path == target:
            return buf[data_start + pos : data_start + pos + size]
    return None


def parse_sheet(sheet_elem) -> dict:
    """Each direct child of <sheet> is one record. Field names contain
    dots, which is valid XML."""
    out = {}
    for rec in sheet_elem:
        rec_id = rec.tag
        entry: dict[str, str | None] = {k: None for k in FIELDS}
        for key, xml_name in FIELDS.items():
            el = rec.find(xml_name)
            if el is not None and el.text:
                entry[key] = el.text.strip()
        out[rec_id] = entry
    return out


def parse_one_lang(buf: bytes, entries, data_start: int, lang: str
                   ) -> dict[str, dict] | None:
    """Parse a single language's XML into ``{sheet: {id: {name, desc,
    flavor}}}``. Returns None if the XML isn't packaged for ``lang``."""
    xml_bytes = extract_lang_xml(buf, entries, data_start, lang)
    if xml_bytes is None:
        return None
    root = ET.fromstring(xml_bytes)
    sheets: dict[str, dict] = {}
    for sheet in root.findall("sheet"):
        name = sheet.attrib.get("name") or ""
        if name not in ATLAS_SHEETS:
            continue
        sheets[name] = parse_sheet(sheet)
    return sheets


def synthesize_english_from_ids(any_lang_sheets: dict[str, dict]
                                ) -> dict[str, dict]:
    """Build a synthetic English sheet by pretty-printing CDB IDs.
    'Sword_Boomerang' -> 'Sword Boomerang'; CamelCase gets a space
    inserted. Better than nothing for users who want to use the atlas
    in English; the canonical names live in hlboot.dat and would need a
    bytecode opcode interpreter to recover properly."""
    out: dict[str, dict] = {}
    cam_split = re.compile(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])")
    for sheet_name, records in any_lang_sheets.items():
        bucket: dict[str, dict] = {}
        for rec_id in records:
            pretty = rec_id.replace("_", " ")
            pretty = cam_split.sub(" ", pretty)
            pretty = re.sub(r"\s+", " ", pretty).strip()
            bucket[rec_id] = {"name": pretty, "desc": None, "flavor": None}
        out[sheet_name] = bucket
    return out


def sanitize(s: str | None) -> str:
    if not s:
        return ""
    return s.replace("\t", " ").replace("\n", " ").replace("\r", "")


def write_tsv(sheets: dict[str, dict], path: Path) -> int:
    lines = ["sheet\tid\tname\tdesc\tflavor\tcategory\tslot"]
    for sheet_name, records in sheets.items():
        for rec_id, fields in records.items():
            cat, slot = (classify(rec_id) if sheet_name == "item" else ("", ""))
            lines.append(
                f"{sheet_name}\t{rec_id}\t"
                f"{sanitize(fields['name'])}\t"
                f"{sanitize(fields['desc'])}\t"
                f"{sanitize(fields['flavor'])}\t"
                f"{cat}\t{slot}"
            )
    text = "\n".join(lines) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return len(text)


def main() -> int:
    buf, entries, data_start = open_pak(RES_PAK)

    targets_root = [
        GAME_DIR / "data",
        PROJ_DIR / "release-staging" / "farever-minimap-dps-0.4" / "data",
        PROJ_DIR / "release-staging" / "farever-minimap-dps-0.4.1" / "data",
    ]

    # Parse every language we can find. Keep the first non-empty one
    # around to seed the synthesized English TSV (we need the full ID
    # set, and any language's keys cover that).
    seed_sheets: dict[str, dict] | None = None
    parsed_langs: list[tuple[str, dict[str, dict]]] = []
    for lang in ALL_LANGS:
        sheets = parse_one_lang(buf, entries, data_start, lang)
        if sheets is None:
            print(f"  lang={lang:<6} -> skipped (no XML in pak)")
            continue
        total = sum(len(r) for r in sheets.values())
        print(f"  lang={lang:<6} -> {total} records across "
              f"{len(sheets)} sheets")
        parsed_langs.append((lang, sheets))
        if seed_sheets is None:
            seed_sheets = sheets

    if not seed_sheets:
        print("!! no language XML extracted; aborting", file=sys.stderr)
        return 1

    # Synthesize English from CDB IDs so users have a usable fallback.
    en_sheets = synthesize_english_from_ids(seed_sheets)
    parsed_langs.append(("en", en_sheets))

    # Drop one TSV per language under data/ in each known target root.
    for lang, sheets in parsed_langs:
        for root in targets_root:
            if not root.parent.exists():
                continue
            n = write_tsv(sheets, root / f"cdb_atlas_{lang}.tsv")
            print(f"-> wrote {root.name}/cdb_atlas_{lang}.tsv ({n:,} bytes)")

    # Backwards-compat: keep ``cdb_atlas.tsv`` pointing at the German
    # extract so older builds that hard-code that filename still work.
    de_sheets = dict(parsed_langs)[
        "de" if any(l == "de" for l, _ in parsed_langs) else parsed_langs[0][0]
    ]
    for root in targets_root:
        if not root.parent.exists():
            continue
        write_tsv(de_sheets, root / "cdb_atlas.tsv")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
