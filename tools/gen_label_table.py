import json
from pathlib import Path

rows = json.load(open(Path(__file__).resolve().parent / 'prim_labels.json',
                encoding='utf-8'))
field = {'Vitality': 'P_VIT', 'Strength': 'P_STR', 'Dexterity': 'P_DEX',
         'Faith': 'P_FAITH', 'Intellect': 'P_INT'}

def esc(s):
    return ''.join('\\x%02x' % b for b in s.encode('utf-8'))

lines, seen = [], set()
for canon in ['Vitality', 'Strength', 'Dexterity', 'Faith', 'Intellect']:
    lines.append('    {"%s", %s},' % (canon, field[canon]))
    seen.add(canon)
for canon, labels in rows.items():
    for lab in labels:
        if lab in seen:
            continue
        seen.add(lab)
        lines.append('    {"%s", %s},  // %s' % (esc(lab), field[canon], canon))

Path(__file__).resolve().parent / 'label_table.txt'.write_text('\n'.join(lines), encoding='utf-8')
print('wrote', len(lines), 'entries')
