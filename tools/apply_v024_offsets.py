"""Apply the v023 -> v024 offset migration to the compiled C++ state modules.

v024 = Farever v0.2.4.29918 (public test, 2026-09-11). Every class deriving
from st.DBState gained a `dbStatePlayer` field, so st.Loadout / st.Equipment /
st.Inventory / st.Item (+ Weapon/Gear/Armor) / st.player.Progress all shift +8
from that point on. ent.* / ui.* / hxbit.* / st.skill.* / st.Player / st.Group
are byte-identical, which is why only these 13 constants are touched.

Name-anchored: each edit matches `constexpr std::size_t <NAME> = <OLD>;` and
requires EXACTLY ONE hit in that file, so a stale/renamed constant fails loud
instead of silently mis-patching. Values come from tools/migrate_v023_v024.py.

Run from repo root:  python tools/apply_v024_offsets.py
"""
import re
import sys

SRC = 'src/farever-mod/'

# file -> [(const_name, old, new)]   (only fields whose offset actually moved)
CONST = {
    'hero_state.cpp': [
        ('OFF_ITEM_KIND', 112, 120),
        ('OFF_ITEM_LEVEL', 144, 152),
        ('OFF_ITEM_UPGRADE', 148, 156),
        ('OFF_LOADOUT_EQUIPMENT', 120, 128),
        ('OFF_LOADOUT_INVENTORY', 136, 144),
        ('OFF_LOADOUT_CURRENCIES', 168, 176),
        ('OFF_EQUIPMENT_CONTENT', 120, 128),
        ('OFF_INVENTORY_CONTENT', 120, 128),
    ],
    'progress_state.cpp': [
        ('OFF_PROGRESS_ACTIVITIES', 136, 144),
        ('OFF_PROGRESS_ELEMENTS', 144, 152),
        ('OFF_PROGRESS_ZONES', 192, 200),
        ('OFF_PROGRESS_ACHIEVS', 200, 208),
    ],
    'skill_resolve.cpp': [
        ('OFF_ITEM_INF', 120, 128),
    ],
}


def main():
    failures = []
    total = 0
    for fname, edits in CONST.items():
        path = SRC + fname
        try:
            text = open(path, encoding='utf-8').read()
        except OSError as e:
            failures.append('%s: %s' % (path, e))
            continue
        for name, old, new in edits:
            pat = re.compile(r'(constexpr std::size_t\s+%s\s*=\s*)%d(\s*;)'
                             % (re.escape(name), old))
            hits = pat.findall(text)
            if len(hits) != 1:
                failures.append('%s: %s = %d -> %d hits (expected 1)'
                                % (path, name, old, len(hits)))
                continue
            text = pat.sub(lambda m: '%s%d%s' % (m.group(1), new, m.group(2)), text)
            total += 1
            print('%-22s %-26s %5d -> %d' % (fname, name, old, new))
        open(path, 'w', encoding='utf-8', newline='').write(text)

    print()
    if failures:
        print('FAILED:')
        for f in failures:
            print('   ', f)
        sys.exit(1)
    print('patched %d constants across %d files' % (total, len(CONST)))


if __name__ == '__main__':
    main()
