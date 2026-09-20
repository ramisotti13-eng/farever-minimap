"""Resolve every C++ offset constant from v023 -> v024 by field NAME.

v024 = Farever v0.2.4.29918 (public test, 2026-09-11). Every class deriving
from st.DBState gained `dbStatePlayer`, so the st.Loadout / st.Equipment /
st.Inventory / st.Item / st.player.Progress families shift +8 from that point
on. Nothing in ent.* / ui.* / hxbit.* / st.skill.* moved.

For each (label, class, current_offset): find the field at that offset in the
v023 dump, then report where the same-named field sits in v024. Follows the
name, not a blanket delta.

Run from repo root:  python tools/migrate_v023_v024.py
"""
import json

old = json.load(open('tools/classes_v023.json'))
new = json.load(open('tools/classes_v024.json'))


def field_at(dump, cls, off):
    c = dump.get(cls)
    if c is None:
        return None, 'CLASS-MISSING'
    hits = [f for f in c['fields'] if f.get('offset') == off and f.get('name')]
    if not hits:
        return None, 'NO-FIELD-AT-OFFSET'
    if len(hits) > 1:
        return hits, 'MULTIPLE'
    return hits[0], 'OK'


def new_offset(cls, name):
    c = new.get(cls)
    if c is None:
        return None
    for f in c['fields']:
        if f['name'] == name:
            return f.get('offset')
    return None


CURR_PROXY = 'hxbit.ObjProxy_Oamount_Int_kind_Data_ItemKind'

# (const_name, file, class, offset as it stands in the code today)
QUERIES = [
    # --- hero_state.cpp ---
    ('OFF_HERO_OWNERPLAYER', 'hero_state.cpp', 'ent.Hero', 16),
    ('OFF_HERO_POSX', 'hero_state.cpp', 'ent.Hero', 176),
    ('OFF_HERO_POSY', 'hero_state.cpp', 'ent.Hero', 184),
    ('OFF_HERO_POSZ', 'hero_state.cpp', 'ent.Hero', 192),
    ('OFF_HERO_ROTZ', 'hero_state.cpp', 'ent.Hero', 200),
    ('OFF_HERO_ISINCOMBAT', 'hero_state.cpp', 'ent.Hero', 688),
    ('OFF_HERO_DYING', 'hero_state.cpp', 'ent.Hero', 560),
    ('OFF_HERO_COMBAT_START', 'hero_state.cpp', 'ent.Hero', 704),
    ('OFF_HERO_LEVEL', 'hero_state.cpp', 'ent.Hero', 992),
    ('OFF_HERO_ATTR', 'hero_state.cpp', 'ent.Hero', 984),
    ('OFF_HERO_TARGET', 'hero_state.cpp', 'ent.Hero', 656),
    ('OFF_HERO_NAME', 'hero_state.cpp', 'ent.Hero', 1280),
    ('OFF_HERO_WEAPON_IN_HAND', 'hero_state.cpp', 'ent.Hero', 1344),
    ('OFF_HERO_LOADOUT', 'hero_state.cpp', 'ent.Hero', 1232),
    ('OFF_HERO_SKILLS', 'hero_state.cpp', 'ent.Hero', 488),
    ('OFF_HERO_WEAPON_SKILLS', 'hero_state.cpp', 'ent.Hero', 1416),
    ('OFF_UNIT_STATUSES', 'hero_state.cpp', 'ent.Hero', 496),
    ('OFF_UNIT_INSTIGATED', 'hero_state.cpp', 'ent.Hero', 504),
    ('OFF_ITEM_KIND', 'hero_state.cpp', 'st.item.Weapon', 112),
    ('OFF_ITEM_LEVEL', 'hero_state.cpp', 'st.item.Weapon', 144),
    ('OFF_ITEM_UPGRADE', 'hero_state.cpp', 'st.item.Weapon', 148),
    ('OFF_LOADOUT_EQUIPMENT', 'hero_state.cpp', 'st.Loadout', 120),
    ('OFF_LOADOUT_INVENTORY', 'hero_state.cpp', 'st.Loadout', 136),
    ('OFF_LOADOUT_CURRENCIES', 'hero_state.cpp', 'st.Loadout', 168),
    ('OFF_EQUIPMENT_CONTENT', 'hero_state.cpp', 'st.Equipment', 120),
    ('OFF_INVENTORY_CONTENT', 'hero_state.cpp', 'st.Inventory', 120),
    ('OFF_CURR_AMOUNT', 'hero_state.cpp', CURR_PROXY, 20),
    ('OFF_CURR_KIND', 'hero_state.cpp', CURR_PROXY, 24),
    ('OFF_STATUS_KIND', 'hero_state.cpp', 'st.skill.Status', 184),
    ('OFF_STATUS_OWNER', 'hero_state.cpp', 'st.skill.Status', 328),
    ('OFF_STATUS_DURATION', 'hero_state.cpp', 'st.skill.Status', 336),
    ('OFF_STATUS_STACKS', 'hero_state.cpp', 'st.skill.Status', 456),
    ('OFF_STATUS_SHIELD_AMOUNT', 'hero_state.cpp', 'st.skill.Status', 464),
    ('OFF_ATTR_BLOCK_BASE', 'hero_state.cpp', 'ent.UnitAttributes', 48),
    ('OFF_HATTR_BLOCK_BASE', 'hero_state.cpp', 'ent.HeroAttributes', 400),
    ('OFF_PLAYER_HERO', 'hero_state.cpp', 'st.Player', 304),
    ('OFF_PLAYER_ISME', 'hero_state.cpp', 'st.Player', 312),
    ('OFF_PLAYER_UID', 'hero_state.cpp', 'st.Player', 184),
    ('OFF_SKILL_KIND', 'hero_state.cpp', 'st.skill.Skill', 184),
    ('OFF_ATB_VALUE', 'hero_state.cpp', 'ui.comp.AttributeBar', 1184),
    ('OFF_ATB_MAX', 'hero_state.cpp', 'ui.comp.AttributeBar', 1200),
    ('OFF_ATB_UNIT', 'hero_state.cpp', 'ui.comp.AttributeBar', 1344),
    ('OFF_ATB_ID', 'hero_state.cpp', 'ui.comp.AttributeBar', 1352),
    ('OFF_ABB_VALUE', 'hero_state.cpp', 'ui.comp.AttributeBlockBar', 1104),
    ('OFF_ABB_MAX', 'hero_state.cpp', 'ui.comp.AttributeBlockBar', 1112),
    ('OFF_ABB_UNIT', 'hero_state.cpp', 'ui.comp.AttributeBlockBar', 1136),
    ('OFF_ABB_ATTR', 'hero_state.cpp', 'ui.comp.AttributeBlockBar', 1144),
    # --- mob_watch.cpp ---
    ('OFF_GO_REMOVED', 'mob_watch.cpp', 'ent.Foe', 8),
    ('OFF_GO_UID', 'mob_watch.cpp', 'ent.Foe', 32),
    ('OFF_GO_POSX', 'mob_watch.cpp', 'ent.Foe', 176),
    ('OFF_GO_DYING', 'mob_watch.cpp', 'ent.Foe', 560),
    ('OFF_UNIT_KIND', 'mob_watch.cpp', 'ent.Foe', 600),
    # --- boss_timer.cpp ---
    ('OFF_FOE_REMOVED', 'boss_timer.cpp', 'ent.Foe', 8),
    ('OFF_FOE_DYING', 'boss_timer.cpp', 'ent.Foe', 560),
    ('OFF_FOE_ISINCOMBAT', 'boss_timer.cpp', 'ent.Foe', 688),
    ('OFF_FOE_COMBAT_START', 'boss_timer.cpp', 'ent.Foe', 704),
    ('OFF_FOE_COMBAT_END', 'boss_timer.cpp', 'ent.Foe', 712),
    ('OFF_FOE_DEATHREQ', 'boss_timer.cpp', 'ent.Foe', 1112),
    # --- entity_state.cpp ---
    ('OFF_GO_REMOVED', 'entity_state.cpp', 'ent.Foe', 8),
    ('OFF_GO_POSX', 'entity_state.cpp', 'ent.Foe', 176),
    ('OFF_GO_POSY', 'entity_state.cpp', 'ent.Foe', 184),
    ('OFF_INTERACT_STATEID', 'entity_state.cpp', 'ent.interactible.Chest', 664),
    # --- party_state.cpp ---
    ('OFF_HERO_POS_X', 'party_state.cpp', 'ent.Hero', 176),
    ('OFF_HERO_NAME', 'party_state.cpp', 'ent.Hero', 1280),
    ('OFF_UNIT_KIND', 'party_state.cpp', 'ent.Hero', 600),
    ('OFF_UNIT_ATTR', 'party_state.cpp', 'ent.Hero', 984),
    ('OFF_UNIT_UID', 'party_state.cpp', 'ent.Hero', 32),
    ('OFF_HERO_OWNERPLAYER', 'party_state.cpp', 'ent.Hero', 16),
    ('OFF_PLAYER_NAME', 'party_state.cpp', 'st.Player', 192),
    ('OFF_PLAYER_GROUP', 'party_state.cpp', 'st.Player', 256),
    ('OFF_PLAYER_HERO', 'party_state.cpp', 'st.Player', 304),
    ('OFF_GROUP_ID', 'party_state.cpp', 'st.Group', 176),
    ('OFF_GROUP_PLAYERS', 'party_state.cpp', 'st.Group', 184),
    # --- progress_state.cpp ---
    ('OFF_HERO_OWNERPLAYER', 'progress_state.cpp', 'ent.Hero', 16),
    ('OFF_PLAYER_PROGRESS', 'progress_state.cpp', 'st.Player', 240),
    ('OFF_PROGRESS_ACTIVITIES', 'progress_state.cpp', 'st.player.Progress', 136),
    ('OFF_PROGRESS_ELEMENTS', 'progress_state.cpp', 'st.player.Progress', 144),
    ('OFF_PROGRESS_ZONES', 'progress_state.cpp', 'st.player.Progress', 192),
    ('OFF_PROGRESS_ACHIEVS', 'progress_state.cpp', 'st.player.Progress', 200),
    ('OFF_MAPDATA_BIT', 'progress_state.cpp', 'hxbit.MapData', 16),
    ('OFF_MAPDATA_MAP', 'progress_state.cpp', 'hxbit.MapData', 40),
    # --- target_state.cpp ---
    ('OFF_UNIT_HOST', 'target_state.cpp', 'ent.Hero', 48),
    ('OFF_GAMEOBJECT_POSX', 'target_state.cpp', 'ent.Hero', 176),
    ('OFF_UNIT_KIND', 'target_state.cpp', 'ent.Hero', 600),
    ('OFF_UNIT_SKILLS', 'target_state.cpp', 'ent.Hero', 488),
    ('OFF_UNIT_ATTR', 'target_state.cpp', 'ent.Hero', 984),
    ('OFF_UNIT_LEVEL', 'target_state.cpp', 'ent.Hero', 992),
    ('OFF_HERO_TARGET', 'target_state.cpp', 'ent.Hero', 656),
    ('OFF_HERO_LOCKED_TARGET', 'target_state.cpp', 'ent.Hero', 1240),
    ('OFF_HERO_AUTO_TARGET', 'target_state.cpp', 'ent.Hero', 1248),
    ('OFF_BASESKILL_KIND', 'target_state.cpp', 'st.skill.Skill', 184),
    ('OFF_BASESKILL_START', 'target_state.cpp', 'st.skill.Skill', 192),
    ('OFF_BASESKILL_STOP', 'target_state.cpp', 'st.skill.Skill', 200),
    ('OFF_BASESKILL_DYN1', 'target_state.cpp', 'st.skill.Skill', 216),
    ('OFF_BASESKILL_DYN2', 'target_state.cpp', 'st.skill.Skill', 224),
    ('OFF_BASESKILL_DYN3', 'target_state.cpp', 'st.skill.Skill', 232),
    ('OFF_BASESKILL_EXEC_STEP', 'target_state.cpp', 'st.skill.Skill', 320),
    ('OFF_BASESKILL_RUNNING', 'target_state.cpp', 'st.skill.Skill', 360),
    ('OFF_SCTX_CAST_PROGRESS', 'target_state.cpp', 'st.skill.SkillContext', 176),
    ('OFF_SCTX_HIT_COUNT', 'target_state.cpp', 'st.skill.SkillContext', 188),
    ('OFF_SCTX_START_TIME', 'target_state.cpp', 'st.skill.SkillContext', 232),
    ('OFF_SCTX_STOP_TIME', 'target_state.cpp', 'st.skill.SkillContext', 240),
    ('OFF_SCTX_STOP_REASON', 'target_state.cpp', 'st.skill.SkillContext', 248),
    ('OFF_NHOST_CURRENT_TIME', 'target_state.cpp', 'hxbit.NetworkHost', 88),
    ('OFF_NHOST_CTX', 'target_state.cpp', 'hxbit.NetworkHost', 112),
    # --- skill_resolve.cpp ---
    ('OFF_BS_INF', 'skill_resolve.cpp', 'st.skill.Skill', 176),
    ('OFF_ITEM_INF', 'skill_resolve.cpp', 'st.Item', 120),
    ('OFF_SKILL_KIND', 'skill_resolve.cpp', 'st.skill.Skill', 184),
    ('OFF_SKILL_CHARGES', 'skill_resolve.cpp', 'st.skill.Skill', 416),
    ('OFF_SKILL_TRIG_CD', 'skill_resolve.cpp', 'st.skill.Skill', 448),
    ('OFF_SKILL_COOLDOWN', 'skill_resolve.cpp', 'st.skill.Skill', 456),
    # --- damage.cpp / heal.cpp ---
    ('OFF_DD_DMG_PTR', 'damage.cpp', 'ui.comp.DamageDisplay', 1176),
    ('OFF_DR_BASESKILL', 'damage.cpp', 'st.skill.DamageResult', 8),
    ('OFF_DR_WEAKSOURCE', 'heal.cpp', 'st.skill.DamageResult', 40),
    ('OFF_DR_TARGET', 'damage.cpp', 'st.skill.DamageResult', 48),
    ('OFF_DR_EFFECT', 'heal.cpp', 'st.skill.DamageResult', 64),
    ('OFF_DR_AMOUNT', 'damage.cpp', 'st.skill.DamageResult', 88),
    ('OFF_DR_HITCOUNT', 'damage.cpp', 'st.skill.DamageResult', 96),
    ('OFF_DR_BLOCK', 'damage.cpp', 'st.skill.DamageResult', 104),
    ('OFF_DR_KILL', 'damage.cpp', 'st.skill.DamageResult', 112),
    ('OFF_DR_CRITICAL', 'damage.cpp', 'st.skill.DamageResult', 113),
    ('OFF_UNIT_KIND', 'damage.cpp', 'ent.Unit', 600),
    ('OFF_UNIT_KIND', 'heal.cpp', 'ent.Unit', 600),
    ('OFF_SKILL_KIND', 'damage.cpp', 'st.skill.BaseSkill', 184),
    # --- codex_state.cpp ---
    ('OFF_CD_NAME', 'codex_state.cpp', 'data.$CodexData', 32),
    ('OFF_CD_UNITNODES', 'codex_state.cpp', 'data.$CodexData', 88),
    ('OFF_CN_NAME', 'codex_state.cpp', 'data.CodexNode', 8),
    ('OFF_CN_FULLPATH', 'codex_state.cpp', 'data.CodexNode', 24),
    ('OFF_CN_PROGRESS', 'codex_state.cpp', 'data.CodexNode', 84),
    ('OFF_CN_MAXPROGRESS', 'codex_state.cpp', 'data.CodexNode', 96),
    ('OFF_CN_COMPLETED', 'codex_state.cpp', 'data.CodexNode', 100),
    # --- camera_state.cpp / activity_icons.cpp ---
    ('OFF_CAM_CURDIR', 'camera_state.cpp', 'client.GameCamera', 256),
    ('OFF_ACTIVITY_INF', 'activity_icons.cpp', 'st.Activity', 480),
]

print('%-28s %-20s %-30s %5s %-26s %5s %4s'
      % ('CONST', 'FILE', 'CLASS', 'OLD', 'FIELD', 'NEW', 'D'))
print('-' * 128)
moved, bad = [], []
for name, fil, cls, off in QUERIES:
    f, status = field_at(old, cls, off)
    if status != 'OK':
        print('%-28s %-20s %-30s %5d %-26s  <<< CHECK'
              % (name, fil, cls, off, '??? ' + status))
        bad.append((name, fil, status))
        continue
    fname = f['name']
    noff = new_offset(cls, fname)
    if noff is None:
        print('%-28s %-20s %-30s %5d %-26s %5s  <<< CHECK'
              % (name, fil, cls, off, fname, 'GONE'))
        bad.append((name, fil, 'GONE'))
        continue
    d = noff - off
    mark = '' if d == 0 else '  <<<'
    print('%-28s %-20s %-30s %5d %-26s %5d %+4d%s'
          % (name, fil, cls, off, fname, noff, d, mark))
    if d:
        moved.append((fil, name, off, noff))

print()
if bad:
    print('%d constant(s) could not be resolved:' % len(bad))
    for n, fl, s in bad:
        print('    %s (%s): %s' % (n, fl, s))
    print()
print('%d constants moved:' % len(moved))
by_file = {}
for fil, name, o, n in moved:
    by_file.setdefault(fil, []).append((name, o, n))
for fil in sorted(by_file):
    print("    '%s': [" % fil)
    for name, o, n in by_file[fil]:
        print("        ('%s', %d, %d)," % (name, o, n))
    print('    ],')
