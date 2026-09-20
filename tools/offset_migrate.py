"""Resolve every C++ offset constant from v017 -> v018 by field NAME.

For each (label, class, old_offset): find the field at old_offset in the v017
dump for `class`, then report that same-named field's offset in v018. This is
robust to non-uniform shifts (Status got +32 on its tail, Skill.kind +16, etc.)
because it follows the field name, not a blanket delta.
"""
import json

old = json.load(open('tools/classes_v017.json'))
new = json.load(open('tools/classes_v018.json'))


def field_at(dump, cls, off):
    c = dump.get(cls)
    if c is None:
        return None, 'CLASS-MISSING'
    hits = [f for f in c['fields'] if f.get('offset') == off]
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


# (label, class, old_offset)
QUERIES = [
    # --- ent.Hero / Unit / GameObject / Foe inherited reads ---
    ('HERO_OWNERPLAYER', 'ent.Hero', 16),
    ('HERO_POSX', 'ent.Hero', 152),
    ('HERO_POSY', 'ent.Hero', 160),
    ('HERO_POSZ', 'ent.Hero', 168),
    ('HERO_ROTZ', 'ent.Hero', 176),
    ('HERO_ISINCOMBAT', 'ent.Hero', 680),
    ('HERO_DYING', 'ent.Hero', 544),
    ('HERO_COMBAT_START', 'ent.Hero', 696),
    ('HERO_LEVEL', 'ent.Hero', 984),
    ('HERO_ATTR', 'ent.Hero', 976),
    ('HERO_TARGET', 'ent.Hero', 648),
    ('HERO_LOCKED_TARGET', 'ent.Hero', 1200),
    ('HERO_AUTO_TARGET', 'ent.Hero', 1208),
    ('HERO_WEAPON_IN_HAND', 'ent.Hero', 1304),
    ('HERO_LOADOUT', 'ent.Hero', 1192),
    ('HERO_SKILLS', 'ent.Hero', 456),
    ('UNIT_STATUSES(instigatedStatuses)', 'ent.Hero', 472),
    ('HERO_NAME', 'ent.Hero', 1240),
    ('HERO_UID(__uid)', 'ent.Hero', 32),
    ('UNIT_KIND(kind)', 'ent.Foe', 592),
    ('GO_REMOVED(removed)', 'ent.Foe', 8),
    ('FOE_COMBAT_END', 'ent.Foe', 704),
    ('FOE_DEATHREQ', 'ent.Foe', 1072),
    ('UNIT_HOST', 'ent.Hero', 48),
    ('INTERACT_STATEID', 'ent.interactible.Chest', 656),
    # --- st.Player ---
    ('PLAYER_HERO', 'st.Player', 280),
    ('PLAYER_ISME', 'st.Player', 288),
    ('PLAYER_UID', 'st.Player', 160),
    ('PLAYER_NAME', 'st.Player', 168),
    ('PLAYER_GROUP', 'st.Player', 232),
    ('PLAYER_PROGRESS', 'st.Player', 216),
    ('PLAYER___UID', 'st.Player', 32),
    # --- st.Group ---
    ('GROUP_ID', 'st.Group', 152),
    ('GROUP_PLAYERS', 'st.Group', 160),
    # --- st.skill.Skill (BaseSkill) ---
    ('SKILL_KIND', 'st.skill.Skill', 160),
    ('BS_INF', 'st.skill.Skill', 152),
    ('BASESKILL_START', 'st.skill.Skill', 168),
    ('BASESKILL_STOP', 'st.skill.Skill', 176),
    ('BASESKILL_DYN1', 'st.skill.Skill', 192),
    ('BASESKILL_DYN2', 'st.skill.Skill', 200),
    ('BASESKILL_DYN3', 'st.skill.Skill', 208),
    ('BASESKILL_EXEC_STEP', 'st.skill.Skill', 296),
    ('BASESKILL_RUNNING', 'st.skill.Skill', 336),
    # --- st.skill.Status ---
    ('STATUS_KIND', 'st.skill.Status', 160),
    ('STATUS_DURATION', 'st.skill.Status', 312),
    ('STATUS_STACKS', 'st.skill.Status', 416),
    ('STATUS_SHIELD_AMOUNT', 'st.skill.Status', 424),
    # --- st.skill.SkillContext ---
    ('SCTX_CAST_PROGRESS', 'st.skill.SkillContext', 168),
    ('SCTX_HIT_COUNT', 'st.skill.SkillContext', 180),
    ('SCTX_START_TIME', 'st.skill.SkillContext', 216),
    ('SCTX_STOP_TIME', 'st.skill.SkillContext', 224),
    ('SCTX_STOP_REASON', 'st.skill.SkillContext', 232),
    # --- st.item.* ---
    ('ITEM_KIND', 'st.item.Weapon', 88),
    ('ITEM_LEVEL', 'st.item.Weapon', 120),
    ('ITEM_UPGRADE', 'st.item.Weapon', 124),
    ('WEAPON_INF', 'st.item.Weapon', 96),
    ('STITEM_INF', 'st.Item', 96),
    ('ITEMSLOT_INF', 'ui.comp.ItemSlot', 1104),
    # --- loadout / equipment ---
    ('LOADOUT_EQUIPMENT', 'st.Loadout', 96),
    ('EQUIPMENT_CONTENT', 'st.Equipment', 96),
    # --- DamageResult (expected identical) ---
    ('DR_BASESKILL', 'st.skill.DamageResult', 8),
    ('DR_TARGET', 'st.skill.DamageResult', 40),
    ('DR_WEAKSOURCE', 'st.skill.DamageResult', 32),
    ('DR_EFFECT', 'st.skill.DamageResult', 56),
    ('DR_AMOUNT', 'st.skill.DamageResult', 80),
    ('DR_HITCOUNT', 'st.skill.DamageResult', 88),
    ('DR_BLOCK', 'st.skill.DamageResult', 96),
    ('DR_KILL', 'st.skill.DamageResult', 104),
    ('DR_CRITICAL', 'st.skill.DamageResult', 105),
    # --- DamageDisplay ---
    ('DD_DMG_PTR', 'ui.comp.DamageDisplay', 1176),
    # --- camera ---
    ('CAM_CURDIR', 'client.GameCamera', 256),
    # --- codex ---
    ('CD_NAME', 'data.CodexData', 32),
    ('CD_UNITNODES', 'data.CodexData', 88),
    ('CN_NAME', 'data.CodexNode', 8),
    ('CN_FULLPATH', 'data.CodexNode', 24),
    ('CN_PROGRESS', 'data.CodexNode', 76),
    ('CN_MAXPROGRESS', 'data.CodexNode', 88),
    ('CN_COMPLETED', 'data.CodexNode', 92),
    # --- progress ---
    ('PROGRESS_ACTIVITIES', 'st.player.Progress', 112),
    ('PROGRESS_ELEMENTS', 'st.player.Progress', 120),
    ('PROGRESS_ZONES', 'st.player.Progress', 168),
    ('PROGRESS_ACHIEVS', 'st.player.Progress', 176),
    ('MAPDATA_BIT', 'hxbit.MapData', 16),
    ('MAPDATA_MAP', 'hxbit.MapData', 40),
    # --- loot ---
    ('CHEST_INF', 'ent.interactible.Chest', 624),
    ('ITEM_CAP_INF', 'st.Item', 96),
    # --- ATB (expected identical) ---
    ('ATB_VALUE', 'ui.comp.AttributeBar', 1168),
    ('ATB_MAX', 'ui.comp.AttributeBar', 1184),
    ('ATB_UNIT', 'ui.comp.AttributeBar', 1328),
    ('ATB_ID', 'ui.comp.AttributeBar', 1336),
    # --- attr blocks (verify base anchors) ---
    ('ATTR_BLOCK health@240', 'ent.UnitAttributes', 240),
    ('HATTR poise@376', 'ent.HeroAttributes', 376),
    ('HATTR glideSpeed@520', 'ent.HeroAttributes', 520),
]

print(f"{'LABEL':36} {'CLASS':26} {'OLD':>5} {'NAME':24} {'NEW':>5}  {'DELTA':>5}")
print('-' * 110)
for label, cls, off in QUERIES:
    f, status = field_at(old, cls, off)
    if status == 'OK':
        name = f['name'] or '(anon)'
        noff = new_offset(cls, f['name'])
        delta = (noff - off) if noff is not None else None
        flag = '' if noff is not None else '  <<< NEW-MISSING'
        print(f"{label:36} {cls:26} {off:>5} {name:24} {str(noff):>5}  {str(delta):>5}{flag}")
    else:
        print(f"{label:36} {cls:26} {off:>5} {'??? ' + status:24}  <<< CHECK")
