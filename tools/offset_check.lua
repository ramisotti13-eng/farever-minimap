-- ==============================================================
-- v024_offset_check.lua  --  THROWAWAY verification plugin
--
-- Exercises exactly the readers whose offsets moved in the v023 -> v024
-- migration (st.Loadout / st.Equipment / st.Inventory / st.Item family),
-- because nothing in the normal log path touches them:
--
--   OFF_LOADOUT_EQUIPMENT   120 -> 128     equipment()
--   OFF_EQUIPMENT_CONTENT   120 -> 128     equipment()
--   OFF_LOADOUT_INVENTORY   136 -> 144     inventory()
--   OFF_INVENTORY_CONTENT   120 -> 128     inventory()
--   OFF_LOADOUT_CURRENCIES  168 -> 176     currencies()
--   OFF_ITEM_KIND           112 -> 120     all three + weapon_kind()
--   OFF_ITEM_LEVEL          144 -> 152     all three + weapon_level()
--   OFF_ITEM_UPGRADE        148 -> 156     all three + weapon_upgrade()
--
-- Dumps one snapshot into farever-mod.log a few seconds after login, then
-- goes quiet. Delete the file once the migration is signed off.
-- ==============================================================

local done      = false
local first_ok  = nil
local DELAY_SEC = 6.0     -- let the loadout populate after the world loads

local function s(v)       -- nil-safe tostring for the log line
    if v == nil then return "nil" end
    return tostring(v)
end

-- A kind string that is empty, or that contains anything outside the
-- [A-Za-z0-9_] the game uses for item ids, means we are reading the wrong
-- field -- that is the exact failure mode a bad offset produces.
local function suspicious(kind)
    if kind == nil or kind == "" then return true end
    return kind:match("^[%w_]+$") == nil
end

function on_init()
    farever.log.info("v024_offset_check: armed, dumping in " .. DELAY_SEC .. "s")
end

function on_render()
    if done then return end
    if first_ok == nil then first_ok = farever.now() end
    if farever.now() - first_ok < DELAY_SEC then return end
    done = true

    local bad = 0

    -- weapon in hand: OFF_ITEM_KIND / LEVEL / UPGRADE straight off the Hero
    local wk = farever.player.weapon_kind and farever.player.weapon_kind() or nil
    farever.log.info(string.format(
        "v024_offset_check: weapon kind=%s level=%s upgrade=%s",
        s(wk), s(farever.player.weapon_level and farever.player.weapon_level()),
        s(farever.player.weapon_upgrade and farever.player.weapon_upgrade())))
    if suspicious(wk) then bad = bad + 1 end

    -- equipment(): Loadout -> Equipment -> content -> slot proxy -> st.item
    local eq = farever.player.equipment and farever.player.equipment() or {}
    farever.log.info(string.format("v024_offset_check: equipment() = %d entries", #eq))
    for _, it in ipairs(eq) do
        farever.log.info(string.format(
            "v024_offset_check:   equip slot=%s name=%s kind=%s level=%s upgrade=%s",
            s(it.slot), s(it.slot_name), s(it.kind), s(it.level), s(it.upgrade)))
        if suspicious(it.kind) then bad = bad + 1 end
    end

    -- inventory(): Loadout -> Inventory -> content, plus the stack count
    local inv = farever.player.inventory and farever.player.inventory() or {}
    farever.log.info(string.format("v024_offset_check: inventory() = %d entries", #inv))
    for _, it in ipairs(inv) do
        farever.log.info(string.format(
            "v024_offset_check:   item kind=%s level=%s upgrade=%s stack=%s",
            s(it.kind), s(it.level), s(it.upgrade), s(it.stack)))
        if suspicious(it.kind) then bad = bad + 1 end
    end

    -- currencies(): Loadout -> currencies proxy (proxy layout itself unchanged)
    local cur = farever.player.currencies and farever.player.currencies() or {}
    farever.log.info(string.format("v024_offset_check: currencies() = %d entries", #cur))
    for _, c in ipairs(cur) do
        farever.log.info(string.format("v024_offset_check:   currency kind=%s amount=%s",
            s(c.kind), s(c.amount)))
        if suspicious(c.kind) then bad = bad + 1 end
    end

    if #eq == 0 and #inv == 0 and #cur == 0 then
        farever.log.info("v024_offset_check: VERDICT INCONCLUSIVE - all three lists " ..
            "empty; loadout may not have replicated yet, reload and retry")
    elseif bad == 0 then
        farever.log.info("v024_offset_check: VERDICT OK - every kind string is a " ..
            "clean item id, migrated offsets read correctly")
    else
        farever.log.info(string.format(
            "v024_offset_check: VERDICT BAD - %d suspicious kind string(s), " ..
            "an offset is still wrong", bad))
    end
end
