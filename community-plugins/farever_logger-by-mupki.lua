-- ==============================================================
-- farever_logger.lua
-- Submitted by @Mupki  (https://github.com/Mupki/farever-toolset)
-- Tested against farever-mod v1.0.0
-- License: MIT
--
-- Quiet background combat logger: records each fight (skills, damage,
-- crits, kills, live target HP, buffs/debuffs, casts, gear snapshot) to
-- a store file for the Farever Log Viewer. Read-only; uses only the
-- documented farever.player / farever.target API. The only os.* calls
-- are os.date / os.time for log timestamps, both guarded (if os ...)
-- so the plugin runs fine if the sandbox omits the os table.
-- ==============================================================
--
-- farever_logger.lua  (v4 - comprehensive real-data capture)
-- Quiet background combat logger for the Farever toolset.
-- Drop into  <game>/data/plugins/  (hot-reloads within ~1s).
--
-- v4 CAPTURES (deliberately broad; we filter to what carries signal):
--  * Per hit: skill / amount / crit / kill PLUS live target hp/max_hp/hp_pct
--    (these target getters are REAL — unlike player attribute getters, which the
--    mod exposes as BASE values only, so we do NOT trust them for build stats).
--  * STATUS samples (~1/s): active buffs/debuffs (kind/duration/stacks) + player
--    resources (energy/rage/spark/focus/combo/poise/hp/shield) → uptime analysis.
--  * Events: TARGETCHANGE, CAST start/end (boss phase timing), WEAPONCHANGE.
--  * Per-fight SNAPSHOT: weapon kind/level/upgrade, full gear loadout, statuses,
--    and base-only stats (clearly labelled stats_base_only — real stats live in
--    UnitAttributes MapData which the mod does not expose to plugins).
--
-- Kept from v3: 8s-gap self-segmentation, continuous autosave, safe delete.
-- The "dbg/decode KEPT ..." prefix stays byte-compatible with the viewer; new
-- fields/lines are appended and ignored by the current parser until we add them.
--
-- FILE: data/plugins/farever_logger.store.lua   (keys flog_1, flog_2, ...)

local GAP        = 8.0     -- seconds of no damage that ends a fight
local SAVE_EVERY = 25      -- autosave the open fight every N new hits
local CHAR_NAME   = ""
local fight_count = 0      -- highest fight id written
local buf         = nil    -- current fight, or nil
local base_mono   = 0
local base_wall   = 0
local last_summary = "idle"
local resetArm    = false  -- two-step delete guard

local MAGIC_PFX = { Mage=true, Scepter=true, Book=true, Halos=true, Staff=true }

-- ---------- safe reads ----------
local function safe_num(fn) local ok,v = pcall(fn); if ok and type(v)=="number" then return v end return 0 end
local function safe_str(fn) local ok,v = pcall(fn); if ok and type(v)=="string" then return v end return "" end

-- ---------- formatting helpers ----------
local function nowstamp()
  local epoch = base_wall + (farever.now() - base_mono)
  local whole = math.floor(epoch)
  local ms = math.floor((epoch - whole) * 1000 + 0.5); if ms > 999 then ms = 999 end
  if os and os.date then
    return string.format("[%s.%03d]", os.date("%Y-%m-%d %H:%M:%S", whole), ms)
  end
  local s=whole%60; local m=math.floor(whole/60)%60; local h=math.floor(whole/3600)%24
  return string.format("[1970-01-01 %02d:%02d:%02d.%03d]", h, m, s, ms)
end

local function addr_token(s)
  s = s or ""; if s == "" then return "0x0" end
  local h = 5381
  for i = 1, #s do h = (h * 33 + s:byte(i)) % 4294967296 end
  return string.format("0x%x", h)
end

local function affinity_of(skill)
  local pfx = skill:match("^([^_]+)")
  if pfx and MAGIC_PFX[pfx] then return "Magic" end
  return "Physical"
end

local function jstr(s)
  s = tostring(s or ""); s = s:gsub('\\','\\\\'):gsub('"','\\"')
  return '"' .. s .. '"'
end

local function gear_json()
  local ok, items = pcall(farever.player.equipment)
  if not ok or type(items) ~= "table" then return "[]" end
  local parts = {}
  for _, it in ipairs(items) do
    parts[#parts+1] = string.format('{"kind":%s,"level":%d,"upgrade":%d}',
      jstr(it.kind), it.level or 0, it.upgrade or 0)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end

-- ---------- snapshot ----------
local function read_stats()
  local s = {
    level   = safe_num(farever.player.level),
    crit    = safe_num(farever.player.crit_chance),
    crit_dmg= safe_num(farever.player.crit_damage),
    armor_pen=safe_num(farever.player.armor_penetration),
    spell_pen=safe_num(farever.player.spell_penetration),
    fervor  = safe_num(farever.player.fervor),
    cdr     = safe_num(farever.player.cooldown_reduction),
    vit     = safe_num(farever.player.vitality),
    str     = safe_num(farever.player.strength),
    dex     = safe_num(farever.player.dexterity),
    int     = safe_num(farever.player.intellect),
    fth     = safe_num(farever.player.faith),
    armor   = safe_num(farever.player.armor),
    magic_armor = safe_num(farever.player.magic_armor),
    max_hp  = safe_num(farever.player.max_health),
  }
  local good = (s.vit>0 or s.int>0 or s.str>0 or s.dex>0 or s.fth>0 or s.max_hp>0)
  return s, good
end

local function statuses_json()
  local ok, s = pcall(farever.player.statuses)
  if not ok or type(s) ~= "table" then return "[]" end
  local parts = {}
  for _, st in ipairs(s) do
    parts[#parts+1] = string.format('{"kind":%s,"dur":%.1f,"stacks":%d}',
      jstr(st.kind), st.duration or 0, st.stacks or 0)
  end
  return "[" .. table.concat(parts, ",") .. "]"
end
local function resources_json()
  local p = farever.player
  return string.format(
    '{"energy":%g,"rage":%g,"spark":%g,"focus":%g,"combo":%g,"poise":%g,'
    .. '"hp":%g,"max_hp":%g,"shield":%g}',
    safe_num(p.energy), safe_num(p.rage), safe_num(p.spark), safe_num(p.focus),
    safe_num(p.combo_point), safe_num(p.poise),
    safe_num(p.health), safe_num(p.max_health), safe_num(p.shield))
end

local function build_snap(fid, target, maxhp, s)
  local stats = string.format(
    '{"crit":%g,"crit_dmg":%g,"armor_pen":%g,"spell_pen":%g,"fervor":%g,'
    .. '"cdr":%g,"vit":%g,"str":%g,"dex":%g,"int":%g,"fth":%g,'
    .. '"armor":%g,"magic_armor":%g,"max_hp":%g}',
    s.crit, s.crit_dmg, s.armor_pen, s.spell_pen, s.fervor, s.cdr,
    s.vit, s.str, s.dex, s.int, s.fth, s.armor, s.magic_armor, s.max_hp)
  return string.format(
    '%s SNAPSHOT {"fight_id":%d,"char":%s,"level":%d,"target":%s,'
    .. '"target_max_hp":%g,"weapon":%s,"weapon_level":%d,"weapon_upgrade":%d,'
    .. '"stats_base_only":%s,"gear":%s,"statuses":%s}',
    nowstamp(), fid or 0, jstr(CHAR_NAME ~= "" and CHAR_NAME or "You"),
    s.level, jstr(target), maxhp or 0,
    jstr(safe_str(farever.player.weapon_kind)),
    safe_num(farever.player.weapon_level), safe_num(farever.player.weapon_upgrade),
    stats, gear_json(), statuses_json())
end

local function refine_snapshot()
  if not buf then return end
  local mh = safe_num(farever.target.max_hp)
  if mh > buf.maxhp then buf.maxhp = mh end
  if buf.snap_good then return end
  local s, good = read_stats()
  buf.snap = build_snap(buf.fid, buf.target0, buf.maxhp, s)
  if good then buf.snap_good = true end
end

-- ---------- damage line ----------
-- The "dbg/decode KEPT ..." prefix is byte-compatible with the viewer's parser;
-- we append REAL live target HP after kill= (viewer ignores trailing fields).
-- target.hp/max_hp/hp_pct are confirmed-real getters (unlike player stats, which
-- the mod only exposes as base values). They give true boss HP + a clean kill.
local function damage_line(skill, amount, is_crit, is_kill, target_kind)
  local thp   = safe_num(farever.target.hp)
  local tmax  = safe_num(farever.target.max_hp)
  local tpct  = safe_num(farever.target.hp_pct)
  return string.format(
    '%s dbg/decode KEPT weak_source=0 source_name=Some(%s) target=%s '
    .. 'target_name=Some(%s) skill_name=Some(%s) affinity_id=Some(%s) '
    .. 'effect=0 amount=%.1f crit=%s kill=%s tgt_hp=%.0f tgt_maxhp=%.0f tgt_pct=%.4f',
    nowstamp(), jstr(CHAR_NAME ~= "" and CHAR_NAME or "You"),
    addr_token(target_kind), jstr(target_kind), jstr(skill),
    jstr(affinity_of(skill)), amount or 0,
    tostring(is_crit == true), tostring(is_kill == true),
    thp, tmax, tpct)
end

-- ---------- extra capture lines (viewer ignores until we parse them) ----------
-- emit a STATUS sample (buff/debuff uptime + resources) at most every second
local STATUS_EVERY = 1.0
local function maybe_status(now)
  if not buf then return end
  if buf.last_status and (now - buf.last_status) < STATUS_EVERY then return end
  buf.last_status = now
  table.insert(buf.lines, string.format('%s STATUS %s res=%s',
    nowstamp(), statuses_json(), resources_json()))
end

-- ---------- store I/O ----------
local function fight_body()
  local snap = buf.snap or build_snap(buf.fid, buf.target0, buf.maxhp, (read_stats()))
  return snap .. "\n" .. table.concat(buf.lines, "\n")
end

-- Write the open fight to its real key. Persists fight_count immediately so a
-- crash leaves the (partial) fight recoverable and visible to the viewer.
local function autosave()
  if not buf or #buf.lines == 0 then return end
  farever.store.set("flog_" .. buf.fid, fight_body())
  if fight_count < buf.fid then
    fight_count = buf.fid
    farever.store.set("flog_count", fight_count)
  end
  buf.savedLen = #buf.lines
end

-- ---------- fight lifecycle (self-segmented) ----------
local function open_fight()
  buf = {
    fid = fight_count + 1, lines = {}, snap = nil, snap_good = false,
    maxhp = 0, first = farever.now(), last = farever.now(),
    total = 0, tdmg = {}, target0 = safe_str(farever.target.name), savedLen = 0,
  }
  refine_snapshot()
end

local function flush_fight()
  if not buf then return end
  if #buf.lines == 0 then buf = nil; return end
  autosave()                    -- final write (also bumps fight_count)
  local primary, best = buf.target0, -1
  for name, dmg in pairs(buf.tdmg) do if dmg > best then best, primary = dmg, name end end
  local dur = buf.last - buf.first
  local dps = dur > 0 and (buf.total / dur) or 0
  last_summary = string.format("#%d  %s  %.0f dmg / %.1fs / %.0f dps%s",
    buf.fid, primary ~= "" and primary or "?", buf.total, dur, dps,
    buf.snap_good and "" or "  (stats unread)")
  farever.toast(string.format("Saved fight #%d (%.0f dps)", buf.fid, dps))
  buf = nil
end

local function gap_close()
  if buf and (farever.now() - buf.last) > GAP then flush_fight() end
end

-- ---------- lifecycle ----------
-- Try to read the character name straight from the game so the user never has
-- to type it. We probe a few likely getters; if none exist on this mod version
-- we silently fall back to the stored/manual value (the input field still works).
local function auto_char_name()
  local p = farever.player
  if not p then return "" end
  for _, fn in ipairs({ p.name, p.char_name, p.hero_name, p.display_name }) do
    if type(fn) == "function" then
      local ok, v = pcall(fn)
      if ok and type(v) == "string" and v ~= "" then return v end
    end
  end
  return ""
end

function on_init()
  CHAR_NAME   = farever.store.get("char_name", "")
  -- prefer a live auto-detected name when the API exposes one
  local auto = auto_char_name()
  if auto ~= "" then
    CHAR_NAME = auto
    farever.store.set("char_name", auto)
  end
  fight_count = farever.store.get("flog_count", 0)
  base_mono   = farever.now()
  base_wall   = (os and os.time and os.time()) or 0
  buf = nil; resetArm = false
end

function on_event(name, data)
  gap_close()
  if name == "damage_dealt" then
    if not buf then open_fight() end
    if not buf.snap_good then refine_snapshot() end
    local now = farever.now()
    local tk = safe_str(farever.target.name); if tk == "" then tk = buf.target0 end
    buf.last  = now
    buf.total = buf.total + (data.amount or 0)
    buf.tdmg[tk] = (buf.tdmg[tk] or 0) + (data.amount or 0)
    table.insert(buf.lines, damage_line(data.skill, data.amount, data.is_crit, data.is_kill, tk))
    maybe_status(now)
    if (#buf.lines - (buf.savedLen or 0)) >= SAVE_EVERY then autosave() end
  elseif name == "target_changed" then
    if buf then table.insert(buf.lines, string.format('%s TARGETCHANGE %s',
      nowstamp(), jstr(data and data.kind or ""))) end
  elseif name == "cast_start" then
    if buf then table.insert(buf.lines, string.format('%s CAST start %s total=%.1f',
      nowstamp(), jstr(data and data.skill or ""), (data and data.total_sec) or 0)) end
  elseif name == "cast_end" then
    if buf then table.insert(buf.lines, string.format('%s CAST end %s dur=%.1f',
      nowstamp(), jstr(data and data.skill or ""), (data and data.duration) or 0)) end
  elseif name == "weapon_changed" then
    if buf then table.insert(buf.lines, string.format('%s WEAPONCHANGE %s level=%d upgrade=%d',
      nowstamp(), jstr(data and data.kind or ""), (data and data.level) or 0, (data and data.upgrade) or 0)) end
  elseif name == "fight_end" then
    flush_fight()
  end
end

-- ---------- panel ----------
function on_render()
  gap_close()
  if buf and not buf.snap_good then refine_snapshot() end

  imgui.text("Farever Logger v4 - comprehensive capture")
  imgui.separator()

  local nm, changed = imgui.input_text("Character name (auto; edit to override)", CHAR_NAME)
  if changed then CHAR_NAME = nm; farever.store.set("char_name", nm) end
  imgui.text("(type once - it is remembered across sessions)")

  imgui.spacing()
  imgui.text(buf and "Status: RECORDING (autosaving to disk)" or "Status: idle")
  imgui.text(string.format("Fights saved (all sessions): %d", fight_count))
  imgui.text("Last: " .. last_summary)

  imgui.separator()
  if imgui.button("Flush current fight") then flush_fight() end
  imgui.text("Finalizes the in-progress fight now. Combat already autosaves")
  imgui.text("every few seconds, so a crash keeps your run.")
  imgui.text("File: data/plugins/farever_logger.store.lua")

  imgui.separator()
  if not resetArm then
    if imgui.button("Reset log store") then resetArm = true end
  else
    imgui.text_colored(1.0, 0.45, 0.3, 1.0,
      string.format("Delete ALL %d saved fights? This cannot be undone.", fight_count))
    if imgui.button("CONFIRM delete") then
      for i = 1, fight_count do farever.store.set("flog_" .. i, nil) end
      fight_count = 0; farever.store.set("flog_count", 0)
      last_summary = "store cleared"; resetArm = false
      farever.toast("Log store cleared")
    end
    imgui.same_line()
    if imgui.button("Cancel") then resetArm = false end
  end
end