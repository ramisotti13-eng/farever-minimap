// Local-Hero tracker driven by the hl_alloc_obj watcher.
//
// Each ent.Hero allocation pushes the raw pointer into a pending
// list. hero_state_tick() (called per frame from the Present hook,
// after damage_tick) iterates pending candidates, waits until the
// Haxe constructor has populated Hero.ownerPlayer, then verifies:
//   - ownerPlayer is a userland pointer
//   - ownerPlayer.hero == this Hero  (bidirectional integrity)
//   - ownerPlayer.isMe == 1          (we are the local player)
// First candidate that passes is locked. Subsequent allocs that
// arrive before the lock get retried up to a hard cap; the lock is
// re-validated every 64 ticks so dungeon transitions don't freeze
// us on a stale pointer.

#include "hero_state.h"
#include "hl_hook.h"
#include "damage.h"
#include "mem_scan.h"
#include "log.h"
#include "progress_state.h"
#include "plugins.h"
#include "skill_resolve.h"   // #96: skill_global_cooldown_reduction()

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>   // snprintf (class-scan diagnostic, #90)
#include <cstring>  // memcpy (attribute-UI value reads)
#include <cstdlib>  // strtod (stat-line amount parsing, #71)
#include <mutex>
#include <vector>

namespace farever {
namespace {

// ent.Hero offsets. v0.5.5 (game v0.1.5.25921): ent.Serializable / GameObject
// added two new fields (sleeping @ 138 + accDt @ 144), pushing every
// GameObject/Unit/Hero field from offset 144 onwards down by 8 bytes.
constexpr std::size_t OFF_HERO_OWNERPLAYER  = 16;
// v018 (game build 2026-06-26): ent.Serializable +__visibilityCount and the
// animStart/EndFrame removal split the layout into 3 zones; pos/rot (<496) get
// +16, but the 528..1056 band (isInCombat/dying/combatStart/_level/attr/target)
// nets to zero. Verified per-field via tools/offset_migrate.py.
constexpr std::size_t OFF_HERO_POSX         = 176;   // f64 (was 152)
constexpr std::size_t OFF_HERO_POSY         = 184;
constexpr std::size_t OFF_HERO_POSZ         = 192;
constexpr std::size_t OFF_HERO_ROTZ         = 200;
constexpr std::size_t OFF_HERO_ISINCOMBAT   = 688;   // u8 was 680 [v023 +8]
constexpr std::size_t OFF_HERO_DYING        = 560;   // u8 (ent.Unit.dying, was 552 [v023 +8])
constexpr std::size_t OFF_HERO_COMBAT_START = 704;  // f64 (combatStartTime) was 696 [v023 +8]
constexpr std::size_t OFF_HERO_LEVEL        = 992;  // i32 (ent.Unit._level) was 984 [v023 +8]
constexpr std::size_t OFF_HERO_ATTR         = 984;  // HOBJ (ent.Unit.attr) was 976 [v023 +8]

constexpr std::size_t OFF_HERO_TARGET       = 656;  // i64 (ent.Unit.target) was 648 [v023 +8]
// #62: ent.Hero.name, the local character's display name. This used to be
// written out as a bare `hero + 1272` inside hero_state_local_name, which is
// exactly the kind of literal a name-anchored offset migration cannot see -
// the v023 update shifted it and player.name() silently returned "". Keep it
// a named constant so the migration tooling catches it next time.
constexpr std::size_t OFF_HERO_NAME         = 1280;  // was 1272 [v023 +8]

// Equipped weapon (Hero.weaponInHand @ 1320 -> st.item.Weapon*) [v018].
// Item layout shared by Weapon/Armor/Gear: kind @ 104 (String), level @ 136,
// upgradeLevel @ 140 [v018 +16]. We only surface kind/level/upgrade for now;
// the deeper inf HVIRTUAL stays opaque.
constexpr std::size_t OFF_HERO_WEAPON_IN_HAND = 1344;  // was 1336 [v023 +8]
constexpr std::size_t OFF_ITEM_KIND           = 120;   // was 112 [v024 +8]
constexpr std::size_t OFF_ITEM_LEVEL          = 152;   // was 144 [v024 +8]
constexpr std::size_t OFF_ITEM_UPGRADE        = 156;   // was 148 [v024 +8]

// Full loadout walk: Hero.loadout @ 1208 -> st.Loadout.equipment @ 112
// -> st.Equipment.content @ 112 (ArrayObj) [v018 +16]. ArrayObj layout:
// length @8, varray @16, varray data starts at +24, element stride 8 bytes.
constexpr std::size_t OFF_HERO_LOADOUT          = 1232;  // was 1224 [v023 +8]
constexpr std::size_t OFF_LOADOUT_EQUIPMENT     = 128;   // was 120 [v024 +8]
constexpr std::size_t OFF_EQUIPMENT_CONTENT     = 128;   // was 120 [v024 +8]
constexpr std::size_t OFF_ARRAYOBJ_LENGTH       = 8;
constexpr std::size_t OFF_ARRAYOBJ_VARRAY       = 16;
constexpr std::size_t OFF_VARRAY_DATA           = 24;
constexpr int         kMaxEquipmentItems        = 32;   // cap per-frame work

// #93: bag inventory. Hero.loadout @ 1224 -> st.Loadout.inventory @ 136 ->
// st.Inventory.content @ 120 (ArrayObj), same item layout as equipment. The
// inventory holds far more than the gear slots, so a larger cap.
constexpr std::size_t OFF_LOADOUT_INVENTORY     = 144;   // was 136 [v024 +8]
constexpr std::size_t OFF_INVENTORY_CONTENT     = 128;   // was 120 [v024 +8]
constexpr int         kMaxInventoryItems        = 256;  // cap per-frame work

// #93: materials / currencies live in st.Loadout.currencies @ 168, an
// hxbit.ArrayProxyData of ObjProxy{amount, kind}. The chain ends in the same
// ArrayObj -> hl varray as equipment/inventory, so the element pointers start
// at OFF_VARRAY_DATA (24), not 16. #98: the old 16 shifted every element down
// by one, which silently dropped the LAST currency (the first read landed on
// the varray's own size/pad word and got filtered out as non-userland).
constexpr std::size_t OFF_LOADOUT_CURRENCIES    = 176;   // was 168 [v024 +8]
constexpr std::size_t OFF_PROXY_ARRAY           = 40;   // ArrayProxyData.array -> ArrayDyn
constexpr std::size_t OFF_ARRAYDYN_ARRAY        = 8;    // ArrayDyn.array -> ArrayObj
constexpr std::size_t OFF_VARRAY_SIZE           = 16;   // hl varray.size (i32), data at +24
constexpr std::size_t OFF_CURR_AMOUNT           = 20;   // ObjProxy.amount (i32)
constexpr std::size_t OFF_CURR_KIND             = 24;   // ObjProxy.kind (String)

// #90: player class. Farever has four player classes (Rogue, Mage, Priest,
// Warrior); each ships ~30 class-specific skill/talent scripts whose runtime
// class name is "script.skills.<Class>_...". The unit's skill list
// (Hero.skills@456, ArrayObj, same layout as equipment.content) carries the
// learned ones, so we tally the four class tokens across it and take the
// majority. Weapon-type skills (Bow_, Sword_, ...) and the plain
// st.skill.Skill base match no class token and are ignored.
constexpr std::size_t OFF_HERO_SKILLS = 488;   // was 480 [v023 +8]
constexpr int         kMaxSkillScan   = 128;   // cap per-refresh work

// #94: the arsenal / selected weapon skills. Same ArrayObj-of-st.skill.Skill
// layout as Hero.skills; the arsenal holds up to two, but a two-hander can
// place a weapon skill here, so we cap generously rather than at two.
constexpr std::size_t OFF_HERO_WEAPON_SKILLS = 1416;   // ent.Hero.weaponSkills was 1408 [v023 +8]
constexpr int         kMaxWeaponSkills        = 16;

// Active statuses. ent.Unit has TWO status lists and they mean opposite
// things (#110):
//
//   statuses @ 496            hxbit.ArrayProxyData - what is ON this unit
//   instigatedStatuses @ 504  ArrayObj              - what this unit PUT ON others
//
// We read the first one. Up to v1.2.7 we read the second, which looked right
// only because a self-buff appears in both lists: the hero instigates it and
// the hero carries it. Stun a foe and the foe's stun showed up as your own
// (gamersa22), while a debuff a foe put on YOU was missing entirely.
//
// `owner` is the unit carrying the status (`instigator` @ 416 is who applied
// it), so every entry is checked against the hero regardless of which list it
// came from. Status layout (v018): kind @ 176, duration @ 328, stacks @ 448,
// shieldAmount @ 456. NOTE the tail (stacks/shield) shifted +32 not +16
// because v018 inserted Status.config@384 and fromArea@416.
constexpr std::size_t OFF_UNIT_STATUSES         = 496;   // #110: was 504 (the wrong list)
constexpr std::size_t OFF_UNIT_INSTIGATED       = 504;   // was 496 [v023 +8]
constexpr std::size_t OFF_STATUS_KIND           = 184;   // was 160
constexpr std::size_t OFF_STATUS_OWNER          = 328;   // ent.GameObject carrying it
constexpr std::size_t OFF_STATUS_DURATION       = 336;   // was 312
constexpr std::size_t OFF_STATUS_STACKS         = 456;   // was 416 (+32)
constexpr std::size_t OFF_STATUS_SHIELD_AMOUNT  = 464;   // was 424 (+32) #70
constexpr int         kMaxStatuses              = 32;

// Haxe String reads use OFF_STR_BYTES = 8, OFF_STR_LEN = 16, declared
// inline where they're used to avoid leaking constants between files.
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

// UnitAttributes layout: the f64 block of interest runs from offset
// 48 (vitality) through offset 328 (heal) - 30 consecutive doubles
// covering stats, combat numbers, defense, resources, modifiers.
// We do ONE 240-byte batched read into a fixed-layout local struct,
// keyed by the indices below (each index = (offset - 48) / 8).
constexpr std::size_t OFF_ATTR_BLOCK_BASE   = 48;
constexpr std::size_t OFF_ATTR_BLOCK_BYTES  = 280;   // covers up to heal@320

enum UAIdx : int {
    UA_VITALITY = 0,            // 48
    UA_STRENGTH,                // 56
    UA_DEXTERITY,               // 64
    UA_FAITH,                   // 72
    UA_INTELLECT,               // 80
    UA_CRIT_CHANCE_RATING,      // 88
    UA_CRIT_CHANCE,             // 96
    UA_CRIT_DAMAGE,             // 104
    UA_ARMOR_PEN_RATING,        // 112
    UA_ARMOR_PEN,               // 120
    UA_SPELL_PEN_RATING,        // 128
    UA_SPELL_PEN,               // 136
    UA_FERVOR_RATING,           // 144
    UA_FERVOR,                  // 152
    UA_BLOCK_MITIGATION,        // 160
    UA_DODGE_CHANCE,            // 168
    UA_MAGIC_MASTERY,           // 176
    UA_PHYSICAL_MASTERY,        // 184
    UA_SPELL_CAST_TIME_RED,     // 192
    UA_KNOCK_RESISTANCE,        // 200
    UA_COOLDOWN_REDUCTION,      // 208
    UA_ARMOR,                   // 216
    UA_MAGIC_ARMOR,             // 224
    UA_MAGIC_REDUCTION,         // 232
    UA_HEALTH,                  // 240
    UA_MAX_HEALTH,              // 248
    UA_HEALTH_REGEN,            // 256
    UA_SHIELD,                  // 264
    UA_ENERGY,                  // 272
    UA_ENERGY_REGEN,            // 280
    UA_LIFETIME,                // 288
    UA_MOVE_SPEED_FACTOR,       // 296
    UA_THREAT,                  // 304
    UA_DAMAGE,                  // 312
    UA_HEAL,                    // 320
    UA_COUNT,
};

// HeroAttributes extends UnitAttributes. Hero-only fields live from
// offset 400 (poise) onwards [v023, was 392]. The whole block shifted a
// uniform +16, so only the base changes; the relative indices below hold.
constexpr std::size_t OFF_HATTR_BLOCK_BASE  = 400;   // was 376
constexpr std::size_t OFF_HATTR_BLOCK_BYTES = 152;   // through glideSpeed@544

enum HAIdx : int {
    HA_POISE = 0,               // 400
    HA_POISE_CONSUME,           // 408
    HA_POISE_REGEN,             // 416
    HA_OXYGEN,                  // 424
    HA_OXYGEN_REGEN,            // 432
    HA_OXYGEN_LOSS,             // 440
    HA_RAGE,                    // 448
    HA_RAGE_REGEN,              // 456
    HA_RAGE_GAIN_FACTOR,        // 464
    HA_SPARK,                   // 472
    HA_SPARK_REGEN,             // 480
    HA_COMBO_POINT,             // 488
    HA_FOCUS,                   // 496
    HA_SPARK_PARTICLE,          // 504
    HA_DAMAGE_MODIFIER,         // 512
    HA_DAMAGE_TAKEN_MODIFIER,   // 520
    HA_HEAL_GIVEN_MULT,         // 528
    HA_SHIELD_POWER_MULT,       // 536
    HA_GLIDE_SPEED,             // 544
    HA_COUNT,
};

constexpr std::size_t OFF_PLAYER_HERO       = 304;   // Player.hero (was 280) [v018 +16]
constexpr std::size_t OFF_PLAYER_ISME       = 312;   // u8 (was 288)         [v018 +16]
constexpr std::size_t OFF_PLAYER_UID        = 184;   // st.Player.uid (was 160) -
                                                     // the stable account id, #87


// How many ticks (~ frames) to retry a pending Hero before dropping
// it. Old value 600 = ~10s; bumped to 6000 (~100s) so dungeon-exit
// loading screens (~15s observed) don't silently flush all our
// candidates while we're waiting for the constructor to populate.
constexpr int kMaxPendingRetries = 6000;

struct Pending {
    std::uintptr_t hero_ptr;
    int            retries;
};

std::atomic<bool>           g_active{false};
std::atomic<int>            g_unlocked_alloc_log_left{16};
std::atomic<std::uintptr_t> g_locked_hero{0};
// Cached owner Player. The Player object is the stable session entity
// - it survives zone / dungeon transitions even when the Hero is
// swapped out. After a re-validation failure we follow Player.hero
// to find the fresh Hero without waiting for a new alloc-hook event.
std::atomic<std::uintptr_t> g_locked_player{0};
std::atomic<std::uint64_t>  g_ticks{0};

std::mutex                  g_pending_mu;
std::vector<Pending>        g_pending;

HeroSnapshot                g_snapshot{};
// v0.5.3.2 (#2): guards g_snapshot against torn cross-thread reads.
// HeroSnapshot is now ~480 bytes after Phase A/B expanded the
// attribute surface; a plain memcpy from a different thread can mix
// half-old / half-new fields. publish() (render thread) and
// hero_state_read() (overlay thread, plugin getters) on
// render thread) both take this lock.
std::mutex                  g_snapshot_mu;

// Plausible world-coordinate ranges (W1 spans ~6 tiles × 256 m).
// Used to reject sync-proxy / template Heroes whose isMe is set but
// whose position is still (0,0) - the structural scan in minimap-dll
// applied the same filter, and skipping it here was the cause of the
// "arrow doesn't track the player" bug.
constexpr double RX_LO = -10000.0, RX_HI = 10000.0;
constexpr double RY_LO = -10000.0, RY_HI = 10000.0;
constexpr double RZ_LO =   -500.0, RZ_HI =  1500.0;

// Read a Haxe String (UTF-16 underneath) at the given pointer into a
// plain std::string. ASCII-only, names up to 127 chars, returns false
// on bad pointer / non-ASCII codepoint. Same pattern as target_state.
bool read_haxe_string(std::uintptr_t str_ptr, std::string& out) {
    out.clear();
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return false;
    auto bytes_ptr = static_cast<std::uintptr_t>(bytes_u64);
    if (!mem_is_userland(bytes_ptr)) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return false;
    if (length <= 0 || length > 128) return false;

    std::uint8_t buf[256];
    std::size_t  nb = static_cast<std::size_t>(length) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(bytes_ptr, buf, nb)) return false;

    out.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c >= 0x80) return false;
        out.push_back(static_cast<char>(c));
    }
    return true;
}

// #89: each equipment-content slot is an hxbit network proxy (an HVIRTUAL,
// whose class name renders as "(...)"), NOT a plain st.item.Item. The old
// reader read kind@88 straight off the proxy and got garbage, so the gear
// list came back empty/unreliable. The real st.item.* object is wrapped inside
// the proxy at +48; once we follow that, kind@88 / level@120 / upgrade@124
// read exactly like weaponInHand. Confirmed live: every equipped slot resolves
// at +48; empty / non-item slots fail the st.item check below and are skipped.
constexpr std::size_t OFF_SLOT_ITEM      = 48;   // slot proxy -> wrapped st.item.*
constexpr std::size_t OFF_SLOT_ITEMCOUNT = 40;   // #93: slot stack count (i32)

// True if `obj` is an item instance: chases hl_type @0 -> obj @8 -> name @16
// (raw UTF-16) and matches the item class-name prefix, reading only the prefix
// bytes. Accepts BOTH the gear subclasses "st.item.*" (Weapon/Gear/Armor/Recipe/
// Mastery, lowercase i) AND the base "st.Item" (capital I) which is what
// materials / consumables / crafting mats are (#93). Used to validate the item
// wrapped in a slot proxy.
bool is_stitem(std::uintptr_t obj) {
    if (!obj || !mem_is_userland(obj)) return false;
    std::uint64_t t = 0;
    if (!mem_read_u64(obj, &t) || !mem_is_userland((std::uintptr_t)t))
        return false;
    std::uint64_t oi = 0;
    if (!mem_read_u64((std::uintptr_t)t + 8, &oi) ||
        !mem_is_userland((std::uintptr_t)oi)) return false;
    std::uint64_t np = 0;
    if (!mem_read_u64((std::uintptr_t)oi + 16, &np) ||
        !mem_is_userland((std::uintptr_t)np)) return false;
    // prefix "st.?tem" where char 3 is 'i' (st.item.*) or 'I' (st.Item)
    static const char kPfx[] = "st.item";
    for (int i = 0; kPfx[i]; ++i) {
        std::uint8_t lo = 0, hi = 0;
        if (!mem_read_u8((std::uintptr_t)np + i * 2, &lo) ||
            !mem_read_u8((std::uintptr_t)np + i * 2 + 1, &hi)) return false;
        if (hi != 0) return false;
        if (i == 3) { if (lo != 'i' && lo != 'I') return false; }
        else if (lo != static_cast<std::uint8_t>(kPfx[i])) return false;
    }
    return true;
}

// Chase obj -> hl_type -> hl_type_obj -> name and copy the class name out as
// ASCII. Those two hops are hl runtime struct layout (hl_type.obj @8,
// hl_type_obj.name @16), not game class layout, so they do not move when the
// game re-layouts between builds. Haxe class names are ASCII; a non-ASCII unit
// means we are not looking at a class name, so the caller gets a clean false.
bool read_type_name_ascii(std::uintptr_t obj, char* out, std::size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!obj || !mem_is_userland(obj)) return false;
    std::uint64_t t = 0;
    if (!mem_read_u64(obj, &t) || !mem_is_userland((std::uintptr_t)t))
        return false;
    std::uint64_t oi = 0;
    if (!mem_read_u64((std::uintptr_t)t + 8, &oi) ||
        !mem_is_userland((std::uintptr_t)oi)) return false;
    std::uint64_t np = 0;
    if (!mem_read_u64((std::uintptr_t)oi + 16, &np) ||
        !mem_is_userland((std::uintptr_t)np)) return false;
    for (std::size_t i = 0; i + 1 < cap; ++i) {
        std::uint8_t lo = 0, hi = 0;
        if (!mem_read_u8((std::uintptr_t)np + i * 2, &lo) ||
            !mem_read_u8((std::uintptr_t)np + i * 2 + 1, &hi)) return false;
        if (hi != 0) return false;          // not ASCII, not a class name
        out[i] = static_cast<char>(lo);
        if (lo == 0) return true;
    }
    return false;                           // longer than cap, treat as no match
}

// True only for the three item classes that actually declare `level` /
// `upgradeLevel`. The "st.item." NAMESPACE is deliberately NOT used as the test
// any more: it is not a reliable proxy for the layout. Verified against
// tools/classes_v024.json:
//
//   st.item.Weapon / Gear / Armor   level@152, upgradeLevel@156   <- these
//   st.item.Mastery                 no level; `mastery` sits at 152
//   st.item.Recipe                  neither; the object ENDS at afxUIDs@144
//
// so the old prefix match reported a Mastery's `mastery` value as its level and
// read clean off the end of a Recipe (that is where `upgrade=464` on
// Recipe_ElixirOfAbundance came from). Callers use this to decide whether the
// level / upgrade read is meaningful at all, which is more honest than clamping
// a garbage number into a plausible-looking range. Re-check the class dump when
// a new st.item.* subclass appears.
bool is_gear_item(std::uintptr_t obj) {
    char name[64];
    if (!read_type_name_ascii(obj, name, sizeof(name))) return false;
    return std::strcmp(name, "st.item.Weapon") == 0 ||
           std::strcmp(name, "st.item.Gear")   == 0 ||
           std::strcmp(name, "st.item.Armor")  == 0;
}

// #90: derive the player's class from the class-specific skills the hero has
// learned. The skill objects are all the data-driven base type st.skill.Skill,
// so the class identity is NOT in their runtime class name (confirmed: every
// element reads "st.skill.Skill"); it lives in the skill's `kind` String
// (st.skill.BaseSkill.kind@160 - the same field decode_skill_name reads off a
// DamageResult). Class skills are id-prefixed by class ("Mage_Conduit_...",
// "Rogue_Talent_..."); weapon / base-attack skills carry a weapon prefix
// ("Bow_...", "DS_Base_Attack") and match no class token. We walk
// Hero.skills@456 (ArrayObj), read each kind, and tally the four class
// prefixes; the most-seen class wins, empty when nothing resolves. The first
// populated scan logs its kinds once so a misresolve is self-diagnosing.
constexpr std::size_t OFF_SKILL_KIND = 184;   // st.skill.BaseSkill.kind (was 160) [v018]

std::string read_player_class(std::uintptr_t hero) {
    std::uint64_t arr_u64 = 0;
    if (!mem_read_u64(hero + OFF_HERO_SKILLS, &arr_u64)) return {};
    auto arr = static_cast<std::uintptr_t>(arr_u64);
    if (!mem_is_userland(arr)) return {};
    std::int32_t length = 0;
    std::uint64_t varr_u64 = 0;
    if (!mem_read_i32(arr + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arr + OFF_ARRAYOBJ_VARRAY, &varr_u64) ||
        length <= 0)
        return {};
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return {};
    auto data = varr + OFF_VARRAY_DATA;
    if (length > kMaxSkillScan) length = kMaxSkillScan;

    // class token + its prefixed form (a skill kind "Mage_..." => Mage).
    static const char* const kClasses[4] = {"Rogue", "Mage", "Priest",
                                            "Warrior"};
    static const char* const kPrefix[4]  = {"Rogue_", "Mage_", "Priest_",
                                            "Warrior_"};
    int counts[4] = {0, 0, 0, 0};

    // One-shot diagnostic: capture a few skill kinds from the first populated
    // scan so a misresolve is debuggable without a separate probe build.
    static bool s_diag_logged = false;
    char        diag[256];
    int         diag_len = 0;
    bool        want_diag = !s_diag_logged;

    for (int i = 0; i < length; ++i) {
        std::uint64_t sk_u64 = 0;
        if (!mem_read_u64(data + i * 8, &sk_u64)) continue;
        auto sk = static_cast<std::uintptr_t>(sk_u64);
        if (!mem_is_userland(sk)) continue;
        std::uint64_t kind_u64 = 0;
        if (!mem_read_u64(sk + OFF_SKILL_KIND, &kind_u64)) continue;
        std::string kind;
        if (!read_haxe_string(static_cast<std::uintptr_t>(kind_u64), kind))
            continue;
        if (want_diag && diag_len < (int)sizeof(diag) - 48) {
            int n = std::snprintf(diag + diag_len, sizeof(diag) - diag_len,
                                  "%s%s", diag_len ? "," : "", kind.c_str());
            if (n > 0) diag_len += n;
        }
        // #104: ask for this skill's icon while we already hold the pointer.
        // Class skills used to reach the icon cache only through a combat
        // damage event, which meant a skill you had not just hit something
        // with stayed nil. We are on the GC-invisible worker, so the dyn walk
        // has to happen on the safe thread - that is what the deferred queue
        // is for (dedup'd, so re-queuing an already-pending kind is free).
        SkillGfx have{};
        if (!skill_resolve_lookup_exact(kind.c_str(), &have))
            skill_resolve_request_deferred(kind.c_str(), sk);
        for (int c = 0; c < 4; ++c) {
            std::size_t pl = std::strlen(kPrefix[c]);
            if (kind.size() >= pl &&
                std::strncmp(kind.c_str(), kPrefix[c], pl) == 0) {
                counts[c]++;
                break;
            }
        }
    }

    int best = -1, bestn = 0;
    for (int c = 0; c < 4; ++c)
        if (counts[c] > bestn) { bestn = counts[c]; best = c; }
    std::string result = best >= 0 ? kClasses[best] : std::string{};

    if (want_diag) {
        s_diag_logged = true;
        logf("hero_state: class scan (#90) - skills=%d Rogue=%d Mage=%d "
             "Priest=%d Warrior=%d -> \"%s\"; kinds=[%.200s]",
             (int)length, counts[0], counts[1], counts[2], counts[3],
             result.c_str(), diag_len ? diag : "");
    }
    return result;
}

// #94: read the arsenal / selected weapon skills off the locked hero.
// ent.Hero.weaponSkills @ 1408 is an ArrayObj of st.skill.Skill, identical in
// layout to Hero.skills, so we walk it the same way and return each element's
// `kind` id in slot order. Position-preserving: a null / unresolved element
// yields an empty string so slot indices stay aligned with the game's array
// (the Lua layer turns the vector index into a 1-based slot). Empty vector when
// the chain hasn't resolved (pre-spawn) or the arsenal is empty.
std::vector<std::string> read_weapon_skills(std::uintptr_t hero) {
    std::vector<std::string> out;
    std::uint64_t arr_u64 = 0;
    if (!mem_read_u64(hero + OFF_HERO_WEAPON_SKILLS, &arr_u64)) return out;
    auto arr = static_cast<std::uintptr_t>(arr_u64);
    if (!mem_is_userland(arr)) return out;
    std::int32_t length = 0;
    std::uint64_t varr_u64 = 0;
    if (!mem_read_i32(arr + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arr + OFF_ARRAYOBJ_VARRAY, &varr_u64) ||
        length <= 0)
        return out;
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return out;
    auto data = varr + OFF_VARRAY_DATA;
    if (length > kMaxWeaponSkills) length = kMaxWeaponSkills;
    out.reserve(length);
    for (int i = 0; i < length; ++i) {
        std::string kind;
        std::uint64_t sk_u64 = 0;
        if (mem_read_u64(data + i * 8, &sk_u64)) {
            auto sk = static_cast<std::uintptr_t>(sk_u64);
            if (mem_is_userland(sk)) {
                std::uint64_t kind_u64 = 0;
                if (mem_read_u64(sk + OFF_SKILL_KIND, &kind_u64))
                    read_haxe_string(static_cast<std::uintptr_t>(kind_u64),
                                     kind);
                // #99: ask for this skill's icon if we have never resolved
                // it. We are on the GC-invisible worker here, so the actual
                // dyn walk has to happen on the safe thread - that is what
                // the deferred request is for. #104: the queue holds several
                // entries and dedups by kind, so the whole arsenal can be
                // requested in one pass instead of one slot per call.
                SkillGfx have{};
                if (!kind.empty() &&
                    !skill_resolve_lookup_exact(kind.c_str(), &have))
                    skill_resolve_request_deferred(kind.c_str(), sk);
            }
        }
        out.push_back(std::move(kind));
    }
    return out;
}


// Resolve an equipment / inventory array element to its st.item.* object. An
// element is either a direct st.item.* or an hxbit slot proxy wrapping one at
// +48 (#89). Returns 0 for empty / non-item slots.
std::uintptr_t resolve_item(std::uintptr_t elem) {
    if (!mem_is_userland(elem)) return 0;
    if (is_stitem(elem)) return elem;
    std::uint64_t w = 0;
    if (mem_read_u64(elem + OFF_SLOT_ITEM, &w) && is_stitem((std::uintptr_t)w))
        return (std::uintptr_t)w;
    return 0;
}

// #87: canonical name for a fixed EquipmentSlot ordinal (the index into the
// game's Equipment.content array). Verified against a live dump: indices 0-17
// are the gear / job-tool slots; 18+ (bags, consumables, mount, glider) are
// left unnamed. Re-verify with dump_equipment_slots_once if a game build
// reorders the EquipmentSlot enum. Returns "" for an unmapped index.
const char* equip_slot_name(int idx) {
    switch (idx) {
        case 0:  return "Weapon1";
        case 1:  return "Weapon2";
        case 2:  return "OffhandWeapon";
        case 3:  return "Head";
        case 4:  return "Neck";
        case 5:  return "Shoulders";
        case 6:  return "Chest";
        case 7:  return "Back";
        case 8:  return "Hands";
        case 9:  return "Waist";
        case 10: return "Legs";
        case 11: return "Feet";
        case 12: return "FingerLeft";
        case 13: return "Trinket";
        case 14: return "FingerRight";
        case 16: return "Pickaxe";
        case 17: return "Sickle";
        default: return "";
    }
}

// #93: read the player's bag inventory. Hero.loadout @ 1224 ->
// st.Loadout.inventory @ 136 -> st.Inventory.content @ 120 (ArrayObj). Each
// element resolves through resolve_item (direct or +48 proxy). Returns
// kind/level/upgrade per item, capped. Empty when the chain hasn't resolved.
std::vector<EquippedItem> read_inventory(std::uintptr_t hero) {
    std::vector<EquippedItem> out;
    std::uint64_t loadout_u64 = 0, inv_u64 = 0, arr_u64 = 0, varr_u64 = 0;
    if (!mem_read_u64(hero + OFF_HERO_LOADOUT, &loadout_u64)) return out;
    auto loadout = static_cast<std::uintptr_t>(loadout_u64);
    if (!mem_is_userland(loadout) ||
        !mem_read_u64(loadout + OFF_LOADOUT_INVENTORY, &inv_u64)) return out;
    auto inv = static_cast<std::uintptr_t>(inv_u64);
    if (!mem_is_userland(inv) ||
        !mem_read_u64(inv + OFF_INVENTORY_CONTENT, &arr_u64)) return out;
    auto arr = static_cast<std::uintptr_t>(arr_u64);
    if (!mem_is_userland(arr)) return out;
    std::int32_t length = 0;
    if (!mem_read_i32(arr + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arr + OFF_ARRAYOBJ_VARRAY, &varr_u64) || length <= 0)
        return out;
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return out;
    auto data = varr + OFF_VARRAY_DATA;
    if (length > kMaxInventoryItems) length = kMaxInventoryItems;
    out.reserve(length);
    for (int i = 0; i < length; ++i) {
        std::uint64_t elem_u64 = 0;
        if (!mem_read_u64(data + i * 8, &elem_u64)) continue;
        auto elem = static_cast<std::uintptr_t>(elem_u64);
        auto item = resolve_item(elem);
        if (!item) continue;
        std::uint64_t kind_u64 = 0;
        if (!mem_read_u64(item + OFF_ITEM_KIND, &kind_u64)) continue;
        EquippedItem ei;
        if (!read_haxe_string(static_cast<std::uintptr_t>(kind_u64), ei.kind))
            continue;
        // level / upgrade only exist on the gear subclasses (st.item.Gear/
        // Weapon/Armor). Base st.Item (materials, food, scrolls) has no such
        // field, so those reads land on unrelated memory - sanity-clamp them.
        std::int32_t lvl = 0, upg = 0;
        if (is_gear_item(item)) {
            mem_read_i32(item + OFF_ITEM_LEVEL, &lvl);
            mem_read_i32(item + OFF_ITEM_UPGRADE, &upg);
        }
        ei.level   = (lvl >= 0 && lvl < 1000) ? (int)lvl : 0;
        ei.upgrade = (upg >= 0 && upg < 1000) ? (int)upg : 0;
        // #93: stack count from the slot proxy at +40 (i32). Only proxy-wrapped
        // slots (item != elem) carry it; direct items stay 1.
        if (item != elem) {
            std::int32_t cnt = 0;
            if (mem_read_i32(elem + OFF_SLOT_ITEMCOUNT, &cnt) &&
                cnt > 0 && cnt < 1000000)
                ei.stack = (int)cnt;
        }
        out.push_back(std::move(ei));
    }
    return out;
}

// #93: read the player's currencies (gold + other counted currencies; crafting
// materials are NOT here, they are stackable items in inventory()).
// Hero.loadout @ 1224 -> st.Loadout.currencies @ 168 (hxbit.ArrayProxyData) ->
// .array @ 40 (ArrayDyn) -> .array @ 8 (ArrayObj, length @ 8 / varray @ 16) ->
// each element is an ObjProxy { amount@20 (i32), kind@24 (String ItemKind) }.
// The varray element pointers start at +24 like every other hl array we walk;
// the varray's own size word sits at +16 and is cross-checked against the
// ArrayObj length (#98). Returns {kind, amount} per resource.
std::vector<CurrencyStack> read_currencies(std::uintptr_t hero) {
    std::vector<CurrencyStack> out;
    std::uint64_t loadout_u64 = 0, proxy_u64 = 0, arrdyn_u64 = 0, arrobj_u64 = 0,
                  varr_u64 = 0;
    if (!mem_read_u64(hero + OFF_HERO_LOADOUT, &loadout_u64)) return out;
    auto loadout = static_cast<std::uintptr_t>(loadout_u64);
    if (!mem_is_userland(loadout) ||
        !mem_read_u64(loadout + OFF_LOADOUT_CURRENCIES, &proxy_u64)) return out;
    auto proxy = static_cast<std::uintptr_t>(proxy_u64);
    if (!mem_is_userland(proxy) ||
        !mem_read_u64(proxy + OFF_PROXY_ARRAY, &arrdyn_u64)) return out;
    auto arrdyn = static_cast<std::uintptr_t>(arrdyn_u64);
    if (!mem_is_userland(arrdyn) ||
        !mem_read_u64(arrdyn + OFF_ARRAYDYN_ARRAY, &arrobj_u64)) return out;
    auto arrobj = static_cast<std::uintptr_t>(arrobj_u64);
    if (!mem_is_userland(arrobj)) return out;
    std::int32_t length = 0;
    if (!mem_read_i32(arrobj + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arrobj + OFF_ARRAYOBJ_VARRAY, &varr_u64) || length <= 0)
        return out;
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return out;
    // #98 guard: the backing varray must be at least as long as the ArrayObj
    // says. If that does not hold we are not looking at an hl varray header and
    // reading data at +24 would walk unrelated memory, so bail instead.
    std::int32_t varr_size = 0;
    if (!mem_read_i32(varr + OFF_VARRAY_SIZE, &varr_size) || varr_size < length)
        return out;
    auto data = varr + OFF_VARRAY_DATA;
    if (length > kMaxInventoryItems) length = kMaxInventoryItems;
    out.reserve(length);
    for (int i = 0; i < length; ++i) {
        std::uint64_t elem_u64 = 0;
        if (!mem_read_u64(data + i * 8, &elem_u64)) continue;
        auto entry = static_cast<std::uintptr_t>(elem_u64);
        if (!mem_is_userland(entry)) continue;
        std::uint64_t kind_u64 = 0;
        if (!mem_read_u64(entry + OFF_CURR_KIND, &kind_u64)) continue;
        CurrencyStack cs;
        if (!read_haxe_string(static_cast<std::uintptr_t>(kind_u64), cs.kind))
            continue;
        std::int32_t amt = 0;
        mem_read_i32(entry + OFF_CURR_AMOUNT, &amt);
        cs.amount = (int)amt;
        out.push_back(std::move(cs));
    }
    return out;
}

// Walk one ArrayObj of st.skill.Status and append what it holds.
//
// `owner_filter` is a unit pointer, or 0 for "take everything". Pass 0 when the
// list is already per-unit (Unit.statuses is, by construction) and a pointer
// only when reading a list that mixes units. `owner` is inherited from
// BaseSkill, where it means the skill's owner, so on a Status it is *probably*
// the unit carrying it but could equally be the caster - filtering on it is a
// best effort, which is why the list that does not need it does not use it.
void collect_statuses(std::uintptr_t arrobj, std::uintptr_t owner_filter,
                      std::vector<ActiveStatus>* out) {
    if (!mem_is_userland(arrobj)) return;
    std::int32_t  length   = 0;
    std::uint64_t varr_u64 = 0;
    if (!mem_read_i32(arrobj + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arrobj + OFF_ARRAYOBJ_VARRAY, &varr_u64) ||
        length <= 0 || length > kMaxStatuses)
        return;
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return;
    // Same #98 guard as the currency walk: if the size word does not cover the
    // length we are not looking at an hl varray header.
    std::int32_t varr_size = 0;
    if (!mem_read_i32(varr + OFF_VARRAY_SIZE, &varr_size) || varr_size < length)
        return;
    auto data = varr + OFF_VARRAY_DATA;
    out->reserve(out->size() + (std::size_t)length);
    for (int i = 0; i < length; ++i) {
        std::uint64_t status_u64 = 0;
        if (!mem_read_u64(data + i * 8, &status_u64)) continue;
        auto status = static_cast<std::uintptr_t>(status_u64);
        if (!mem_is_userland(status)) continue;
        if (owner_filter) {
            std::uint64_t owner_u64 = 0;
            if (!mem_read_u64(status + OFF_STATUS_OWNER, &owner_u64) ||
                static_cast<std::uintptr_t>(owner_u64) != owner_filter)
                continue;                // #110: someone else's status
        }
        std::uint64_t kind_u64 = 0;
        if (!mem_read_u64(status + OFF_STATUS_KIND, &kind_u64)) continue;
        ActiveStatus st;
        if (!read_haxe_string(static_cast<std::uintptr_t>(kind_u64), st.kind))
            continue;
        mem_read_bytes(status + OFF_STATUS_DURATION, &st.duration,
                       sizeof(st.duration));
        std::int32_t stk = 0;
        mem_read_i32(status + OFF_STATUS_STACKS, &stk);
        st.stacks = (int)stk;
        // #70: live absorb this status grants (0 for non-shield statuses);
        // summed into s.shield by the caller.
        mem_read_bytes(status + OFF_STATUS_SHIELD_AMOUNT, &st.shield_amount,
                       sizeof(st.shield_amount));
        if (!(st.shield_amount > 0.0 && st.shield_amount < 1e8))
            st.shield_amount = 0.0;      // garbage / uninit guard
        out->push_back(std::move(st));
    }
}

// #87 (investigation): one-shot dump of the RAW equipment-content array,
// empty slots included, so the fixed EquipmentSlot -> index mapping can be read
// off a live character. The Lua equipment() API compacts out empty slots, which
// hides the positional layout; this logs index -> kind ("-" for an empty slot)
// exactly once from the first array it reads. Temporary; drives the slot-name
// design and gets removed once the mapping is pinned.
void dump_equipment_slots_once(std::uintptr_t hero) {
    static bool done = false;
    if (done) return;
    std::uint64_t loadout_u64 = 0, equip_u64 = 0, arr_u64 = 0, varr_u64 = 0;
    if (!mem_read_u64(hero + OFF_HERO_LOADOUT, &loadout_u64)) return;
    auto loadout = static_cast<std::uintptr_t>(loadout_u64);
    if (!mem_is_userland(loadout) ||
        !mem_read_u64(loadout + OFF_LOADOUT_EQUIPMENT, &equip_u64)) return;
    auto equipment = static_cast<std::uintptr_t>(equip_u64);
    if (!mem_is_userland(equipment) ||
        !mem_read_u64(equipment + OFF_EQUIPMENT_CONTENT, &arr_u64)) return;
    auto arr = static_cast<std::uintptr_t>(arr_u64);
    if (!mem_is_userland(arr)) return;
    std::int32_t length = 0;
    if (!mem_read_i32(arr + OFF_ARRAYOBJ_LENGTH, &length) ||
        !mem_read_u64(arr + OFF_ARRAYOBJ_VARRAY, &varr_u64) || length <= 0)
        return;
    auto varr = static_cast<std::uintptr_t>(varr_u64);
    if (!mem_is_userland(varr)) return;
    auto data = varr + OFF_VARRAY_DATA;
    if (length > kMaxEquipmentItems) length = kMaxEquipmentItems;
    char buf[640];
    int n = std::snprintf(buf, sizeof(buf), "equip-slot dump (#87): len=%d",
                          (int)length);
    for (int i = 0; i < length && n < (int)sizeof(buf) - 56; ++i) {
        std::uint64_t elem_u64 = 0;
        std::string kind = "-";
        if (mem_read_u64(data + i * 8, &elem_u64)) {
            auto item = resolve_item(static_cast<std::uintptr_t>(elem_u64));
            if (item) {
                std::uint64_t k = 0;
                if (mem_read_u64(item + OFF_ITEM_KIND, &k))
                    read_haxe_string(static_cast<std::uintptr_t>(k), kind);
            }
        }
        int m = std::snprintf(buf + n, sizeof(buf) - n, " [%d]=%s", i,
                              kind.c_str());
        if (m > 0) n += m;
    }
    done = true;
    logf("hero_state: %s", buf);
}

// ===== #71 live attribute reader (production) =========================
// The displayed character stats are computed on demand and live only in
// the character-menu / HUD widgets, NOT on the unit (the inline
// UnitAttributes f64s are class defaults; see task #246). We capture
// those widgets as they allocate, read each ONCE a moment later (built +
// certainly still alive), cache the numbers, and drop the pointer - no
// long-lived references to transient UI objects. HeroSnapshot's primary
// + max-health fields are then sourced from this cache so the existing
// farever.player.* getters return live values. Refreshes whenever the
// character menu stat rows or the HUD resource bars (re)build.
//   ui.hud.StatLine:    primaries - label (child FmtText.text@224, matched
//                       to a primary by localized name) + amount@1080's text.
//   ui.comp.AttributeBar: atbId@1336 (id String "Health") -> max@1184; unit@1328.
struct AttrUiCache {
    std::atomic<double> vitality{0}, strength{0}, dexterity{0};
    std::atomic<double> faith{0}, intellect{0}, max_health{0};
    // #91: live class-resource currents, re-read from the HUD AttributeBars
    // (value@1168) every tick. The inline HeroAttributes f64s @376 are class
    // DEFAULTS (read 0), so the resource getters were always 0; the live value
    // lives only on the bar widget. NaN until that resource's bar is read, so
    // attrui_apply only overrides a field we actually have live data for.
    std::atomic<double> poise{NAN}, oxygen{NAN}, rage{NAN}, spark{NAN};
    std::atomic<double> combo_point{NAN}, focus{NAN}, energy{NAN};
    // #91: the cap that goes with each of the above, off the same widget. A
    // rune can raise max combo points, so this is not a constant per class.
    std::atomic<double> poise_max{NAN}, oxygen_max{NAN}, rage_max{NAN},
                        spark_max{NAN};
    std::atomic<double> combo_point_max{NAN}, focus_max{NAN}, energy_max{NAN};
    std::atomic<bool>   seen{false};

    // Back to "nothing known yet". Used on a character switch, where every
    // value below belongs to the character we just left. Same NaN-vs-zero
    // split as the initialisers: a primary of 0 means unread, a resource of
    // NaN means "this character has no such bar".
    void reset() {
        vitality.store(0); strength.store(0); dexterity.store(0);
        faith.store(0); intellect.store(0); max_health.store(0);
        poise.store(NAN); oxygen.store(NAN); rage.store(NAN);
        spark.store(NAN); combo_point.store(NAN); focus.store(NAN);
        energy.store(NAN);
        poise_max.store(NAN); oxygen_max.store(NAN); rage_max.store(NAN);
        spark_max.store(NAN); combo_point_max.store(NAN);
        focus_max.store(NAN); energy_max.store(NAN);
        seen.store(false, std::memory_order_release);
    }
};

// Set by hero_state_reset_caches() on a character switch, consumed by the
// next publish(). A flag rather than a direct clear because the caches are
// function-local statics inside publish() and the switch is signalled from
// the pump thread.
std::atomic<bool> g_reset_caches{false};
AttrUiCache g_attrui;

// Full character-sheet rows (#71 stats() API): every captured StatLine's
// localized label + display text + parsed number, deduped by label, latest
// wins. Covers primaries AND secondaries (crit, block, penetration, ...),
// exactly as shown - no canonical mapping needed, so it works in every
// language and for every class.
std::mutex           g_stats_mu;
std::vector<UiStat>  g_stats;
void attrui_upsert_stat(const std::string& label, const std::string& text,
                        double value) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> lk(g_stats_mu);
    for (auto& e : g_stats)
        if (e.label == label) { e.text = text; e.value = value; return; }
    if (g_stats.size() < 64) g_stats.push_back({label, text, value});
}

// Primary-attribute identity by localized label. The character sheet shows
// the 5 primaries as ui.hud.StatLine rows whose label is the localized
// attribute name and whose amount is the value. The class's main attribute
// is shown apart from the others and shortStats only lists the rest, so we
// key by LABEL (not row position) - robust across classes. The table maps
// every shipped language's label (UTF-8) + the English canonical id to a
// primary slot, so it also works for non-German clients. Generated from the
// CDB attribute sheet (tools/gen_label_table.py).
enum { P_VIT, P_STR, P_DEX, P_FAITH, P_INT };
struct PrimLabel { const char* label; int prim; };
const PrimLabel kPrimLabels[] = {
    {"Vitality", P_VIT},
    {"Strength", P_STR},
    {"Dexterity", P_DEX},
    {"Faith", P_FAITH},
    {"Intellect", P_INT},
    {"\x56\x69\x74\x61\x6c\x69\x74\xc3\xa4\x74", P_VIT},               // de Vitality
    {"\x56\x69\x74\x61\x6c\x69\x64\x61\x64", P_VIT},                   // es
    {"\x56\x69\x74\x61\x6c\x69\x74\xc3\xa9", P_VIT},                   // fr
    {"\xe6\xb4\xbb\xe5\x8a\x9b", P_VIT},                               // ja/zh
    {"\xed\x99\x9c\xeb\xa0\xa5", P_VIT},                               // ko
    {"\x57\x69\x74\x61\x6c\x6e\x6f\xc5\x9b\xc4\x87", P_VIT},           // pl
    {"\x56\x69\x74\x61\x6c\x69\x64\x61\x64\x65", P_VIT},               // pt-BR
    {"\xd0\x96\xd0\xb8\xd0\xb2\xd1\x83\xd1\x87\xd0\xb5\xd1\x81\xd1\x82\xd1\x8c", P_VIT}, // ru
    {"\x53\x74\xc3\xa4\x72\x6b\x65", P_STR},                           // de Strength
    {"\x46\x75\x65\x72\x7a\x61", P_STR},                              // es
    {"\x46\x6f\x72\x63\x65", P_STR},                                  // fr
    {"\xe7\xad\x8b\xe5\x8a\x9b", P_STR},                              // ja
    {"\xed\x9e\x98", P_STR},                                          // ko
    {"\x53\x69\xc5\x82\x61", P_STR},                                  // pl
    {"\x46\x6f\x72\xc3\xa7\x61", P_STR},                              // pt-BR
    {"\xd0\xa1\xd0\xb8\xd0\xbb\xd0\xb0", P_STR},                      // ru
    {"\xe5\x8a\x9b\xe9\x87\x8f", P_STR},                              // zh
    {"\x47\x65\x73\x63\x68\x69\x63\x6b\x6c\x69\x63\x68\x6b\x65\x69\x74", P_DEX}, // de Dexterity
    {"\x44\x65\x73\x74\x72\x65\x7a\x61", P_DEX},                      // es/pt-BR
    {"\x44\x65\x78\x74\xc3\xa9\x72\x69\x74\xc3\xa9", P_DEX},          // fr
    {"\xe6\x8a\x80\xe9\x87\x8f", P_DEX},                              // ja
    {"\xeb\xaf\xbc\xec\xb2\xa9", P_DEX},                              // ko
    {"\x5a\x72\xc4\x99\x63\x7a\x6e\x6f\xc5\x9b\xc4\x87", P_DEX},      // pl
    {"\xd0\x9b\xd0\xbe\xd0\xb2\xd0\xba\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c", P_DEX}, // ru
    {"\xe6\x95\x8f\xe6\x8d\xb7", P_DEX},                              // zh
    {"\x47\x6c\x61\x75\x62\x65", P_FAITH},                            // de Faith
    {"\x46\x65", P_FAITH},                                            // es
    {"\x46\x6f\x69", P_FAITH},                                        // fr
    {"\xe4\xbf\xa1\xe4\xbb\xb0", P_FAITH},                            // ja/zh
    {"\xeb\xaf\xbf\xec\x9d\x8c", P_FAITH},                            // ko
    {"\x57\x69\x61\x72\x61", P_FAITH},                                // pl
    {"\x46\xc3\xa9", P_FAITH},                                        // pt-BR
    {"\xd0\x92\xd0\xb5\xd1\x80\xd0\xb0", P_FAITH},                    // ru
    {"\x49\x6e\x74\x65\x6c\x6c\x69\x67\x65\x6e\x7a", P_INT},          // de Intellect
    {"\x49\x6e\x74\x65\x6c\x65\x63\x74\x6f", P_INT},                  // es/pt-BR
    {"\x49\x6e\x74\x65\x6c\x6c\x69\x67\x65\x6e\x63\x65", P_INT},      // fr (== EN handled above)
    {"\xe7\x9f\xa5\xe5\x8a\x9b", P_INT},                              // ja
    {"\xec\xa7\x80\xeb\x8a\xa5", P_INT},                              // ko
    {"\x49\x6e\x74\x65\x6c\x65\x6b\x74", P_INT},                      // pl
    {"\xd0\x98\xd0\xbd\xd1\x82\xd0\xb5\xd0\xbb\xd0\xbb\xd0\xb5\xd0\xba\xd1\x82", P_INT}, // ru
    {"\xe6\x99\xba\xe5\x8a\x9b", P_INT},                              // zh
};
int attrui_match_primary(const std::string& s) {
    if (s.empty()) return -1;
    for (const auto& e : kPrimLabels)
        if (s == e.label) return e.prim;
    return -1;
}
// Read a Haxe String as UTF-8 (labels are localized, non-ASCII). BMP only.
bool attrui_read_utf8(std::uintptr_t str_ptr, std::string& out) {
    out.clear();
    if (!str_ptr || !mem_is_userland(str_ptr)) return false;
    std::uint64_t b = 0; std::int32_t len = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &b)) return false;
    std::uintptr_t bp = static_cast<std::uintptr_t>(b);
    if (!mem_is_userland(bp)) return false;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &len)) return false;
    if (len <= 0 || len > 128) return false;
    std::uint8_t buf[260];
    std::size_t nb = static_cast<std::size_t>(len) * 2;
    if (nb > sizeof(buf)) return false;
    if (!mem_read_bytes(bp, buf, nb)) return false;
    for (int i = 0; i < len; ++i) {
        std::uint16_t c = static_cast<std::uint16_t>(buf[i * 2]) |
                          (static_cast<std::uint16_t>(buf[i * 2 + 1]) << 8);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return true;
}

struct PendWidget { std::uintptr_t ptr; std::uint64_t tick; int kind; }; // 0=block 1=bar
std::mutex              g_pw_mu;
std::vector<PendWidget> g_pw;

void attrui_push(std::uintptr_t obj, int kind) {
    if (!obj) return;
    std::lock_guard<std::mutex> lk(g_pw_mu);
    if (g_pw.size() < 256)
        g_pw.push_back({obj, g_ticks.load(std::memory_order_relaxed), kind});
}
void on_statline_alloc(std::uintptr_t o)    { attrui_push(o, 3); }  // primaries by label
void on_attr_resbar_alloc(std::uintptr_t o) { attrui_push(o, 1); }  // max_health
void on_attr_blockbar_alloc(std::uintptr_t o) { attrui_push(o, 2); }  // #91 block gauges

// #91: ui.comp.AttributeBar live fields. value@1168 is the CURRENT value
// (what the player sees in the bar), max@1184 the cap. The Health bar gave us
// max_health (read once); the class resources need value re-read every tick.
constexpr std::size_t OFF_ATB_VALUE = 1184;
constexpr std::size_t OFF_ATB_MAX   = 1200;
constexpr std::size_t OFF_ATB_UNIT  = 1344;
constexpr std::size_t OFF_ATB_ID    = 1352;

// #91: the OTHER shape a resource takes. A resource drawn as discrete pips
// (combo points, rage segments) is a ui.comp.AttributeBlockBar, which descends
// from BlockGauge rather than BaseGauge and so has its own layout - and names
// the attribute in `attribute`, not `atbId`. `max` is an i32 here, and it is
// the number a rune can raise. [v023]
constexpr std::size_t OFF_ABB_VALUE = 1104;
constexpr std::size_t OFF_ABB_MAX   = 1112;   // i32
constexpr std::size_t OFF_ABB_UNIT  = 1136;
constexpr std::size_t OFF_ABB_ATTR  = 1144;

// Persistent table of the local hero's resource bars so their live value can be
// re-read each tick (a one-shot read won't track a value that changes in
// combat). Render-thread only (attrui_process). Bars persist for the HUD
// lifetime; re-validated every pass and re-captured on HUD rebuild.
// `kind` mirrors PendWidget's: 1 = AttributeBar, 2 = AttributeBlockBar.
struct ResBar { char atbId[24]; std::uintptr_t ptr; int kind; };
std::vector<ResBar> g_resbars;

// Route an atbId's live value + cap into the matching resource slot.
// Case-sensitive substring match so it survives id prefixes/suffixes. A max of
// NaN means "this widget does not carry one", leaving any previous cap alone.
// Unknown ids are left to the capture-time diagnostic. Returns true if it
// matched a known resource.
bool attrui_store_resource(const char* id, double v, double vmax) {
    std::atomic<double>* cur = nullptr;
    std::atomic<double>* cap = nullptr;
    if      (std::strstr(id, "Rage"))   { cur = &g_attrui.rage;        cap = &g_attrui.rage_max; }
    else if (std::strstr(id, "Spark"))  { cur = &g_attrui.spark;       cap = &g_attrui.spark_max; }
    else if (std::strstr(id, "Combo"))  { cur = &g_attrui.combo_point; cap = &g_attrui.combo_point_max; }
    else if (std::strstr(id, "Focus"))  { cur = &g_attrui.focus;       cap = &g_attrui.focus_max; }
    else if (std::strstr(id, "Poise"))  { cur = &g_attrui.poise;       cap = &g_attrui.poise_max; }
    else if (std::strstr(id, "Oxygen")) { cur = &g_attrui.oxygen;      cap = &g_attrui.oxygen_max; }
    else if (std::strstr(id, "Energy")) { cur = &g_attrui.energy;      cap = &g_attrui.energy_max; }
    else return false;
    cur->store(v);
    if (!std::isnan(vmax)) cap->store(vmax);
    return true;
}

void resbar_upsert(const std::string& id, std::uintptr_t ptr, int kind) {
    for (auto& rb : g_resbars)
        if (std::strcmp(rb.atbId, id.c_str()) == 0) {
            rb.ptr  = ptr;
            rb.kind = kind;
            return;
        }
    if (g_resbars.size() < 16) {
        ResBar rb{};
        std::strncpy(rb.atbId, id.c_str(), sizeof(rb.atbId) - 1);
        rb.ptr  = ptr;
        rb.kind = kind;
        g_resbars.push_back(rb);
    }
}

// A resource bar's class name is not one fixed string: the Mage spark bar is
// `ui.hud.hero.MageSparkBar`, a SUBCLASS of AttributeBar with the same field
// layout. hl_hook matches names exactly, so it needs its own watcher, and the
// re-validation here has to accept it too - checking only for "AttributeBar"
// dropped every spark reading on the floor.
bool is_attribute_bar_class(const char* cls) {
    return std::strstr(cls, "AttributeBar") || std::strstr(cls, "SparkBar");
}

// Drain settled widget captures, read each once, update the cache. Runs on
// the render-thread tick (where the snapshot is built). All reads are
// SEH-guarded and re-validate the class name + owning unit, so a widget
// freed before we read it fails gracefully rather than feeding garbage.
void attrui_process() {
    std::uintptr_t hero = g_locked_hero.load(std::memory_order_acquire);
    std::uint64_t  now  = g_ticks.load(std::memory_order_relaxed);
    // NOTE: no early return on an empty queue - the dedicated shortStats slot
    // below must be checked every tick regardless of AttributeBar traffic.
    std::vector<PendWidget> ready;
    {
        std::lock_guard<std::mutex> lk(g_pw_mu);
        std::vector<PendWidget> keep;
        keep.reserve(g_pw.size());
        for (const auto& w : g_pw) {
            std::uint64_t age = now - w.tick;
            if (age < 60)        keep.push_back(w);   // not settled yet - retry later
            else if (age <= 1800) ready.push_back(w); // settled - read once, then drop
            // else: too old - drop silently
        }
        g_pw.swap(keep);
    }

    auto rp = [](std::uintptr_t b, std::size_t o) -> std::uintptr_t {
        std::uint64_t v = 0; if (!mem_read_u64(b + o, &v)) return 0;
        std::uintptr_t p = static_cast<std::uintptr_t>(v);
        return mem_is_userland(p) ? p : 0;
    };
    auto rf = [](std::uintptr_t b, std::size_t o) -> double {
        std::uint64_t v = 0; if (!mem_read_u64(b + o, &v)) return 0.0;
        double d; std::memcpy(&d, &v, 8); return d;
    };
    for (const auto& w : ready) {
        char cls[80];
        if (!hl_hook_class_name(w.ptr, cls, sizeof cls)) continue;
        if (w.kind == 2) {                                   // AttributeBlockBar
            if (!std::strstr(cls, "AttributeBlockBar")) continue;
            if (rp(w.ptr, OFF_ABB_UNIT) != hero) continue;
            std::string id;
            read_haxe_string(rp(w.ptr, OFF_ABB_ATTR), id);   // attribute@1144
            if (id.empty()) continue;
            std::int32_t cap = 0;
            mem_read_i32(w.ptr + OFF_ABB_MAX, &cap);
            {   // Same one-shot per-id diagnostic as the bar path.
                static std::vector<std::string> s_seen_block;
                bool known = false;
                for (auto& s : s_seen_block) if (s == id) { known = true; break; }
                if (!known && s_seen_block.size() < 32) {
                    s_seen_block.push_back(id);
                    logf("hero_state: AttributeBlockBar (#91) attribute=\"%s\" "
                         "value=%.1f max=%d", id.c_str(),
                         rf(w.ptr, OFF_ABB_VALUE), (int)cap);
                }
            }
            if (attrui_store_resource(id.c_str(), rf(w.ptr, OFF_ABB_VALUE),
                                      (double)cap))
                resbar_upsert(id, w.ptr, 2);
        } else if (w.kind == 1) {                            // AttributeBar
            if (!is_attribute_bar_class(cls)) continue;
            if (rp(w.ptr, OFF_ATB_UNIT) != hero) continue;   // unit@1328 - must be us
            std::string id;
            read_haxe_string(rp(w.ptr, OFF_ATB_ID), id);     // atbId@1336
            if (id == "Health") {                            // max_health = bar max@1184
                g_attrui.max_health.store(rf(w.ptr, OFF_ATB_MAX));
                g_attrui.seen.store(true, std::memory_order_release);
            }
            // #91: capture-time diagnostic - log every distinct atbId on our
            // hero once, with its value/max, so the exact resource bar ids are
            // visible in a normal log (and wrong name guesses are debuggable).
            {
                static std::vector<std::string> s_seen_ids;
                bool known = false;
                for (auto& s : s_seen_ids) if (s == id) { known = true; break; }
                if (!known && !id.empty() && s_seen_ids.size() < 32) {
                    s_seen_ids.push_back(id);
                    logf("hero_state: AttributeBar (#91) atbId=\"%s\" "
                         "value=%.1f max=%.1f", id.c_str(),
                         rf(w.ptr, OFF_ATB_VALUE), rf(w.ptr, OFF_ATB_MAX));
                }
            }
            // #91: register the resource bars we know how to route so their
            // live value gets re-read every tick (below).
            if (!id.empty() && attrui_store_resource(id.c_str(),
                                                     rf(w.ptr, OFF_ATB_VALUE),
                                                     rf(w.ptr, OFF_ATB_MAX)))
                resbar_upsert(id, w.ptr, 1);
        } else if (w.kind == 3) {                            // StatLine -> primary by label
            if (!std::strstr(cls, "StatLine")) continue;
            // label = first FmtText child's text; value = amount@1080's text.
            std::uintptr_t ch = rp(w.ptr, 8);                // children
            std::int32_t len = 0; std::uintptr_t varr = 0;
            if (ch) { mem_read_i32(ch + 8, &len); varr = rp(ch, 16); }
            std::string label;
            for (int i = 0; varr && i < len && i < 16; ++i) {
                std::uintptr_t node = rp(varr, 16 + static_cast<std::size_t>(i) * 8);
                if (!node) continue;
                char nc[64];
                if (!hl_hook_class_name(node, nc, sizeof nc)) continue;
                if (std::strstr(nc, "FmtText") || std::strstr(nc, "Text")) {
                    attrui_read_utf8(rp(node, 224), label);  // Text.text@224
                    break;
                }
            }
            int prim = attrui_match_primary(label);
            std::uintptr_t amt = rp(w.ptr, 1096);            // amount FmtText
            std::string av;
            if (amt) attrui_read_utf8(rp(amt, 224), av);     // amount text "124"/"11.1%"
            double v = std::strtod(av.c_str(), nullptr);
            // Typed primary getters (vitality()/strength()/...) come from here.
            if (prim >= 0 && v > 0.0) {
                switch (prim) {
                    case P_VIT:   g_attrui.vitality.store(v);  break;
                    case P_STR:   g_attrui.strength.store(v);  break;
                    case P_DEX:   g_attrui.dexterity.store(v); break;
                    case P_FAITH: g_attrui.faith.store(v);     break;
                    case P_INT:   g_attrui.intellect.store(v); break;
                }
                g_attrui.seen.store(true, std::memory_order_release);
            }
            // The full sheet (primaries + secondaries) for farever.player.stats().
            if (!label.empty() && !av.empty())
                attrui_upsert_stat(label, av, v);
        }
    }

    // One-shot confirmation once the primaries have resolved (character menu
    // opened at least once). Aids support and smoke-tests; logs the local
    // player's own values only, once per session.
    static std::atomic<bool> s_logged{false};
    if (g_attrui.vitality.load() > 0.0 && !s_logged.exchange(true))
        logf("hero_state: live attributes resolved from UI - vit=%.0f str=%.0f "
             "dex=%.0f faith=%.0f int=%.0f maxHP=%.0f",
             g_attrui.vitality.load(), g_attrui.strength.load(),
             g_attrui.dexterity.load(), g_attrui.faith.load(),
             g_attrui.intellect.load(), g_attrui.max_health.load());

    // One-shot confirmation that the full sheet (primaries + secondaries) is
    // captured for farever.player.stats() - fires when the expanded view is
    // opened (>= 10 rows seen).
    static std::atomic<bool> s_full_logged{false};
    if (!s_full_logged.load(std::memory_order_acquire)) {
        std::size_t n;
        { std::lock_guard<std::mutex> lk(g_stats_mu); n = g_stats.size(); }
        if (n >= 10 && !s_full_logged.exchange(true))
            logf("hero_state: character sheet captured - %zu rows for player.stats()", n);
    }

    // #91: re-read each registered resource bar's live value@1168 every pass.
    // Re-validate (class name + our unit + atbId still matches) so a freed or
    // reused widget is dropped instead of feeding garbage. Bars re-register via
    // the alloc watcher after a HUD rebuild.
    if (hero && !g_resbars.empty()) {
        std::vector<ResBar> keep;
        keep.reserve(g_resbars.size());
        for (const auto& rb : g_resbars) {
            char cls[80];
            if (!hl_hook_class_name(rb.ptr, cls, sizeof cls)) continue;
            std::string id;
            double      val = 0.0, cap = NAN;
            if (rb.kind == 2) {
                if (!std::strstr(cls, "AttributeBlockBar")) continue;
                if (rp(rb.ptr, OFF_ABB_UNIT) != hero) continue;
                read_haxe_string(rp(rb.ptr, OFF_ABB_ATTR), id);
                std::int32_t m = 0;
                mem_read_i32(rb.ptr + OFF_ABB_MAX, &m);
                val = rf(rb.ptr, OFF_ABB_VALUE);
                cap = (double)m;
            } else {
                if (!is_attribute_bar_class(cls)) continue;
                if (rp(rb.ptr, OFF_ATB_UNIT) != hero) continue;
                read_haxe_string(rp(rb.ptr, OFF_ATB_ID), id);
                val = rf(rb.ptr, OFF_ATB_VALUE);
                cap = rf(rb.ptr, OFF_ATB_MAX);
            }
            if (std::strcmp(id.c_str(), rb.atbId) != 0) continue;   // reused ptr
            attrui_store_resource(rb.atbId, val, cap);
            g_attrui.seen.store(true, std::memory_order_release);
            keep.push_back(rb);
        }
        g_resbars.swap(keep);

        // Periodic diagnostic: confirm the live values track (and that the
        // routed atbIds are right). A few times early, then quiet.
        static int s_diag_count = 0;
        std::uint64_t nn = g_ticks.load(std::memory_order_relaxed);
        if (s_diag_count < 6 && nn % 600 == 0) {
            ++s_diag_count;
            logf("hero_state: resources (#91) poise=%.1f/%.1f oxygen=%.1f/%.1f "
                 "rage=%.1f/%.1f spark=%.1f/%.1f combo=%.1f/%.1f "
                 "focus=%.1f/%.1f energy=%.1f/%.1f (bars=%zu)",
                 g_attrui.poise.load(),       g_attrui.poise_max.load(),
                 g_attrui.oxygen.load(),      g_attrui.oxygen_max.load(),
                 g_attrui.rage.load(),        g_attrui.rage_max.load(),
                 g_attrui.spark.load(),       g_attrui.spark_max.load(),
                 g_attrui.combo_point.load(), g_attrui.combo_point_max.load(),
                 g_attrui.focus.load(),       g_attrui.focus_max.load(),
                 g_attrui.energy.load(),      g_attrui.energy_max.load(),
                 g_resbars.size());
        }
    }
}

// Override the snapshot's primary + max-health fields with live UI values
// once the cache has been populated (character menu opened at least once).
// Inline defaults stay as the fallback until then. Current health stays
// from the live inline health@240; only max_health comes from the UI.
void attrui_apply(HeroSnapshot& s) {
    if (!g_attrui.seen.load(std::memory_order_acquire)) return;
    double v;
    if ((v = g_attrui.vitality.load())   > 0.0) s.vitality   = v;
    if ((v = g_attrui.strength.load())   > 0.0) s.strength   = v;
    if ((v = g_attrui.dexterity.load())  > 0.0) s.dexterity  = v;
    if ((v = g_attrui.faith.load())      > 0.0) s.faith      = v;
    if ((v = g_attrui.intellect.load())  > 0.0) s.intellect  = v;
    if ((v = g_attrui.max_health.load()) > 0.0) s.max_health = v;
}

// #91: live class resources from the HUD bars. Must run AFTER the inline
// HeroAttributes read (which writes class-default 0s into these fields), so
// it is separate from attrui_apply (which runs before that, for primaries).
// Override only a resource we actually read a bar for (non-NaN); a class
// without that resource keeps the inline 0. 0 is a valid live value (empty
// bar), so this is deliberately not a >0 gate.
void attrui_apply_resources(HeroSnapshot& s) {
    double v;
    // The Lua resource getters return 0 unless hero_attr_ok is set, and that
    // flag is owned by the inline HeroAttributes read. Once a HUD bar has given
    // us a live value the hero-only fields ARE meaningful, so say so - otherwise
    // a failed inline read would zero out numbers we can see on screen.
    if (!std::isnan(g_attrui.poise.load())  || !std::isnan(g_attrui.rage.load()) ||
        !std::isnan(g_attrui.spark.load())  || !std::isnan(g_attrui.focus.load()) ||
        !std::isnan(g_attrui.oxygen.load()) ||
        !std::isnan(g_attrui.combo_point.load()))
        s.hero_attr_ok = true;
    if (!std::isnan(v = g_attrui.poise.load()))       s.poise       = v;
    if (!std::isnan(v = g_attrui.oxygen.load()))      s.oxygen      = v;
    if (!std::isnan(v = g_attrui.rage.load()))        s.rage        = v;
    if (!std::isnan(v = g_attrui.spark.load()))       s.spark       = v;
    if (!std::isnan(v = g_attrui.combo_point.load())) s.combo_point = v;
    if (!std::isnan(v = g_attrui.focus.load()))       s.focus       = v;
    if (!std::isnan(v = g_attrui.energy.load()))      s.energy      = v;
    // Caps, same rule: only a resource we actually saw a bar for.
    if (!std::isnan(v = g_attrui.poise_max.load()))       s.poise_max       = v;
    if (!std::isnan(v = g_attrui.oxygen_max.load()))      s.oxygen_max      = v;
    if (!std::isnan(v = g_attrui.rage_max.load()))        s.rage_max        = v;
    if (!std::isnan(v = g_attrui.spark_max.load()))       s.spark_max       = v;
    if (!std::isnan(v = g_attrui.combo_point_max.load())) s.combo_point_max = v;
    if (!std::isnan(v = g_attrui.focus_max.load()))       s.focus_max       = v;
    if (!std::isnan(v = g_attrui.energy_max.load()))      s.energy_max      = v;
}

bool position_is_plausible(std::uintptr_t hero_ptr) {
    double pos[4];
    if (!mem_read_bytes(hero_ptr + OFF_HERO_POSX, pos, sizeof(pos)))
        return false;
    double x = pos[0], y = pos[1], z = pos[2];
    if (std::isnan(x) || std::isinf(x)) return false;
    if (std::isnan(y) || std::isinf(y)) return false;
    if (std::isnan(z) || std::isinf(z)) return false;
    if (x < RX_LO || x > RX_HI) return false;
    if (y < RY_LO || y > RY_HI) return false;
    if (z < RZ_LO || z > RZ_HI) return false;
    // (0, 0) is the canonical "uninitialised" pose - reject it
    // explicitly so we don't lock on a template Hero whose isMe was
    // already set by the network deserialiser.
    if (std::fabs(x) < 0.01 && std::fabs(y) < 0.01) return false;
    return true;
}

// Returns true if this Hero pointer is currently the local player's:
//   - ownerPlayer.isMe == 1
//   - bidirectional Player.hero == this Hero
//   - position is a plausible in-world coordinate (not the world
//     origin and not the bogus values a not-yet-streamed Hero shows)
bool is_local_hero(std::uintptr_t hero_ptr) {
    std::uint64_t owner_u64 = 0;
    if (!mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &owner_u64)) return false;
    auto owner = static_cast<std::uintptr_t>(owner_u64);
    if (!mem_is_userland(owner)) return false;

    std::uint64_t player_hero = 0;
    if (!mem_read_u64(owner + OFF_PLAYER_HERO, &player_hero)) return false;
    if (player_hero != hero_ptr) return false;

    std::uint8_t is_me = 0;
    if (!mem_read_u8(owner + OFF_PLAYER_ISME, &is_me)) return false;
    if (is_me != 1) return false;

    return position_is_plausible(hero_ptr);
}

// Verbose debug dump used around lock-lifecycle events. Exists so we
// can trace dungeon entry / exit transitions in detail without
// changing the hot-path is_local_hero() above.
void debug_dump_hero(const char* tag, std::uintptr_t hero_ptr) {
    if (!hero_ptr) {
        logf("hero_state[%s]: hero=NULL", tag);
        return;
    }
    if (!mem_is_userland(hero_ptr)) {
        logf("hero_state[%s]: hero=0x%llx INVALID(not userland)",
             tag, (unsigned long long)hero_ptr);
        return;
    }
    std::uint64_t owner_u64 = 0;
    bool owner_ok = mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &owner_u64);
    std::uint64_t back_hero = 0;
    std::uint8_t  isme      = 255;
    if (owner_ok && mem_is_userland(owner_u64)) {
        mem_read_u64(owner_u64 + OFF_PLAYER_HERO, &back_hero);
        mem_read_u8 (owner_u64 + OFF_PLAYER_ISME, &isme);
    }
    double pos[4] = {0,0,0,0};
    bool pos_ok = mem_read_bytes(hero_ptr + OFF_HERO_POSX, pos, sizeof(pos));
    std::uint8_t in_combat = 255;
    mem_read_u8(hero_ptr + OFF_HERO_ISINCOMBAT, &in_combat);

    logf("hero_state[%s]: hero=0x%llx owner=0x%llx isMe=%u "
         "player.hero=0x%llx (match=%d) pos=(%.1f,%.1f,%.1f%s) "
         "inCombat=%u",
         tag,
         (unsigned long long)hero_ptr,
         (unsigned long long)owner_u64,
         (unsigned)isme,
         (unsigned long long)back_hero,
         (int)(back_hero == hero_ptr),
         pos_ok ? pos[0] : 0.0,
         pos_ok ? pos[1] : 0.0,
         pos_ok ? pos[2] : 0.0,
         pos_ok ? "" : " READFAIL",
         (unsigned)in_combat);
}

void on_hero_alloc(std::uintptr_t obj) {
    if (!obj) return;
    // v0.4.14: when already locked, the pending queue exists only to
    // catch zone-transition re-locks. City scenes with 30+ remote
    // players were piling up hundreds of entries per second, each
    // costing a full is_local_hero check per tick. Tight cap when
    // locked keeps zone-transitions detectable without the churn.
    const bool locked = g_locked_hero.load(std::memory_order_acquire) != 0;
    const std::size_t kMaxPending = locked ? 8 : 256;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.push_back({obj, 0});
        if (g_pending.size() > kMaxPending) {
            g_pending.erase(
                g_pending.begin(),
                g_pending.begin() + (g_pending.size() - kMaxPending));
        }
    }
    // Diagnostic: log every alloc that arrives while we're unlocked.
    // Resets each time the lock drops.
    if (g_unlocked_alloc_log_left.fetch_sub(1) > 0) {
        debug_dump_hero("alloc", obj);
    }
}

// Read ownerPlayer from a known-good Hero. Used right after locking so
// we can chase Player.hero on later re-validation failures.
std::uintptr_t read_owner_player(std::uintptr_t hero_ptr) {
    std::uint64_t v = 0;
    if (!mem_read_u64(hero_ptr + OFF_HERO_OWNERPLAYER, &v)) return 0;
    if (!mem_is_userland(static_cast<std::uintptr_t>(v))) return 0;
    return static_cast<std::uintptr_t>(v);
}

// Try to recover a fresh Hero by walking Player.hero. Returns the new
// Hero pointer on success, or 0 if the player back-ref is gone or the
// new Hero doesn't validate.
std::uintptr_t try_relock_via_player() {
    std::uintptr_t player = g_locked_player.load(std::memory_order_acquire);
    if (!player || !mem_is_userland(player)) return 0;
    std::uint64_t hero_u64 = 0;
    if (!mem_read_u64(player + OFF_PLAYER_HERO, &hero_u64)) return 0;
    auto hero = static_cast<std::uintptr_t>(hero_u64);
    if (!hero || !mem_is_userland(hero)) return 0;
    if (!is_local_hero(hero)) return 0;
    return hero;
}

// v0.5.3.2 (#2): every g_snapshot mutation goes through this helper so
// the lock acquisition is impossible to forget. Called from publish()
// (5 call sites) and from hero_state_start.
void publish_snapshot(const HeroSnapshot& s) {
    std::lock_guard<std::mutex> lk(g_snapshot_mu);
    g_snapshot = s;
}

void publish() {
    HeroSnapshot s{};
    std::uintptr_t a = g_locked_hero.load(std::memory_order_acquire);
    if (a == 0) {
        publish_snapshot(s);
        return;
    }
    // v0.4.14 (E): type-tag check before dereferencing. Boehm GC reuses
    // dead object slots without unmapping pages, so an invalidated Hero
    // can still memcpy cleanly but reads garbage. Comparing the type
    // pointer at +0 against our learned hl_type for ent.Hero is the
    // cheapest possible guard and matches what hl_hook itself learns
    // when caching the watcher. Mismatch -> drop the lock.
    static std::uintptr_t s_hero_type = 0;
    if (s_hero_type == 0) s_hero_type = hl_hook_get_type(L"ent.Hero");
    if (s_hero_type) {
        std::uint64_t got_type = 0;
        if (!mem_read_u64(a, &got_type) ||
            static_cast<std::uintptr_t>(got_type) != s_hero_type) {
            logf("hero_state: type-tag mismatch on locked Hero @ 0x%llx "
                 "(got 0x%llx, want 0x%llx) - dropping lock",
                 (unsigned long long)a,
                 (unsigned long long)got_type,
                 (unsigned long long)s_hero_type);
            g_locked_hero.store(0);
            g_locked_player.store(0);
            g_unlocked_alloc_log_left.store(16);
            publish_snapshot(s);
            return;
        }
    }
    double pos[4]{0,0,0,0};
    if (!mem_read_bytes(a + OFF_HERO_POSX, pos, sizeof(pos))) {
        // Pointer went stale (memory unmapped). Drop the lock and let
        // the next alloc seed a fresh candidate. Player back-ref also
        // becomes meaningless without a valid Hero anchor.
        logf("hero_state: read FAILED on locked Hero @ 0x%llx - "
             "dropping lock",
             (unsigned long long)a);
        g_locked_hero.store(0);
        g_locked_player.store(0);
        g_unlocked_alloc_log_left.store(16);
        publish_snapshot(s);
        return;
    }
    // Drop locks that read NaN / inf - that's the signature of a
    // Hero whose memory got overwritten by the GC during a zone
    // transition (we saw owner=garbage, pos=NaN in the logs).
    if (std::isnan(pos[0]) || std::isnan(pos[1]) || std::isnan(pos[2]) ||
        std::isinf(pos[0]) || std::isinf(pos[1]) || std::isinf(pos[2])) {
        logf("hero_state: NaN / inf in pos on locked Hero @ 0x%llx - "
             "dropping lock",
             (unsigned long long)a);
        g_locked_hero.store(0);
        g_locked_player.store(0);
        g_unlocked_alloc_log_left.store(16);
        publish_snapshot(s);
        return;
    }
    s.locked = true;
    s.x      = pos[0];
    s.y      = pos[1];
    s.z      = pos[2];
    s.rot_z  = pos[3];

    std::uint8_t in_combat = 0;
    if (mem_read_u8(a + OFF_HERO_ISINCOMBAT, &in_combat)) {
        s.in_combat = (in_combat != 0);
    }
    std::uint8_t dying = 0;
    if (mem_read_u8(a + OFF_HERO_DYING, &dying)) {
        s.dying = (dying != 0);
    }
    mem_read_bytes(a + OFF_HERO_COMBAT_START, &s.combat_start,
                   sizeof(s.combat_start));
    std::int32_t lvl = 0;
    if (mem_read_i32(a + OFF_HERO_LEVEL, &lvl)) s.level = (int)lvl;
    std::uint64_t tgt = 0;
    if (mem_read_u64(a + OFF_HERO_TARGET, &tgt)) {
        s.has_target = (tgt != 0);
    }

    // Chase Hero.attr -> UnitAttributes / HeroAttributes. The pointer
    // can be null while the Hero is mid-spawn; validate before any
    // dereference. Two batched reads: 35 contiguous f64s from the
    // UnitAttributes layer, then 19 more from the HeroAttributes
    // extension if the runtime type is the Hero subclass.
    std::uint64_t attr_u64 = 0;
    if (mem_read_u64(a + OFF_HERO_ATTR, &attr_u64)) {
        std::uintptr_t attr = static_cast<std::uintptr_t>(attr_u64);
        if (mem_is_userland(attr)) {
            double ua[UA_COUNT]{};
            if (mem_read_bytes(attr + OFF_ATTR_BLOCK_BASE, ua,
                               OFF_ATTR_BLOCK_BYTES)) {
                s.attr_ok                  = true;
                s.vitality                 = ua[UA_VITALITY];
                s.strength                 = ua[UA_STRENGTH];
                s.dexterity                = ua[UA_DEXTERITY];
                s.faith                    = ua[UA_FAITH];
                s.intellect                = ua[UA_INTELLECT];
                s.crit_chance              = ua[UA_CRIT_CHANCE];
                s.crit_damage              = ua[UA_CRIT_DAMAGE];
                s.armor_penetration        = ua[UA_ARMOR_PEN];
                s.spell_penetration        = ua[UA_SPELL_PEN];
                s.fervor                   = ua[UA_FERVOR];
                s.block_mitigation         = ua[UA_BLOCK_MITIGATION];
                s.dodge_chance             = ua[UA_DODGE_CHANCE];
                s.magic_mastery            = ua[UA_MAGIC_MASTERY];
                s.physical_mastery         = ua[UA_PHYSICAL_MASTERY];
                s.spell_cast_time_reduction= ua[UA_SPELL_CAST_TIME_RED];
                s.knock_resistance         = ua[UA_KNOCK_RESISTANCE];
                s.cooldown_reduction       = ua[UA_COOLDOWN_REDUCTION];
                s.armor                    = ua[UA_ARMOR];
                s.magic_armor              = ua[UA_MAGIC_ARMOR];
                s.magic_reduction          = ua[UA_MAGIC_REDUCTION];
                s.health                   = ua[UA_HEALTH];
                s.max_health               = ua[UA_MAX_HEALTH];
                s.health_regen             = ua[UA_HEALTH_REGEN];
                // ua[UA_SHIELD] reads a dead 0 (#70); s.shield is computed from
                // the active statuses' shieldAmount after the status walk below.
                s.energy                   = ua[UA_ENERGY];
                s.energy_regen             = ua[UA_ENERGY_REGEN];
                s.move_speed_factor        = ua[UA_MOVE_SPEED_FACTOR];
                s.damage                   = ua[UA_DAMAGE];
                s.heal                     = ua[UA_HEAL];

                // #71: the inline f64s are class defaults; override the
                // primaries + max_health with the live character-menu
                // widget values once they've been captured.
                attrui_apply(s);

                // #96: cooldownReduction@208 inline is the engine base (0); the
                // live value isn't stored on the unit. Estimate the global (gear)
                // CDR from resolved skills' effective-vs-base cooldowns instead.
                // 0 until skills have resolved (played a little); leaves the base
                // value untouched if so.
                double gcdr = skill_global_cooldown_reduction();
                if (gcdr > 0.0) s.cooldown_reduction = gcdr;
            }


            // v0.5.3.2 (#1): only read the HeroAttributes-specific
            // block (offsets 376..528) if the attr object's runtime
            // type really IS ent.HeroAttributes. Before this check
            // we relied on mem_read_bytes' SEH guard to gracefully
            // fail when attr was a plain UnitAttributes (368 bytes
            // total). But SEH only fires on an actual page fault -
            // if the bytes past the end of a UnitAttributes happen
            // to be allocated heap from a neighbouring object the
            // read succeeds and writes garbage into the Hero
            // resource fields. Compare attr's type tag against the
            // cached hl_type for ent.HeroAttributes; only proceed
            // if it matches.
            static std::uintptr_t s_hattr_type = 0;
            if (s_hattr_type == 0) {
                s_hattr_type = hl_hook_get_type(L"ent.HeroAttributes");
            }
            std::uint64_t attr_type_u64 = 0;
            bool is_hero_attr = false;
            if (s_hattr_type &&
                mem_read_u64(attr, &attr_type_u64) &&
                static_cast<std::uintptr_t>(attr_type_u64) == s_hattr_type) {
                is_hero_attr = true;
            }
            if (is_hero_attr) {
                double ha[HA_COUNT]{};
                if (mem_read_bytes(attr + OFF_HATTR_BLOCK_BASE, ha,
                                   OFF_HATTR_BLOCK_BYTES)) {
                    s.hero_attr_ok            = true;
                    s.poise                   = ha[HA_POISE];
                    s.poise_regen             = ha[HA_POISE_REGEN];
                    s.oxygen                  = ha[HA_OXYGEN];
                    s.rage                    = ha[HA_RAGE];
                    s.rage_regen              = ha[HA_RAGE_REGEN];
                    s.spark                   = ha[HA_SPARK];
                    s.spark_regen             = ha[HA_SPARK_REGEN];
                    s.combo_point             = ha[HA_COMBO_POINT];
                    s.focus                   = ha[HA_FOCUS];
                    s.damage_modifier         = ha[HA_DAMAGE_MODIFIER];
                    s.damage_taken_modifier   = ha[HA_DAMAGE_TAKEN_MODIFIER];
                    s.heal_given_multiplier   = ha[HA_HEAL_GIVEN_MULT];
                    s.shield_power_multiplier = ha[HA_SHIELD_POWER_MULT];
                    s.glide_speed             = ha[HA_GLIDE_SPEED];
                }
            }
        }
    }

    // #91: live class-resource override. Runs after the inline HeroAttributes
    // read above (which fills these with class-default 0s) and regardless of
    // whether attr resolved, so the HUD-bar values always win when present.
    attrui_apply_resources(s);

    // Equipped weapon chase. Hero.weaponInHand can be null briefly when
    // the player is switching weapons; all reads are guarded.
    std::uint64_t weapon_u64 = 0;
    if (mem_read_u64(a + OFF_HERO_WEAPON_IN_HAND, &weapon_u64)) {
        auto weapon = static_cast<std::uintptr_t>(weapon_u64);
        if (mem_is_userland(weapon)) {
            std::uint64_t kind_u64 = 0;
            if (mem_read_u64(weapon + OFF_ITEM_KIND, &kind_u64)) {
                auto kind = static_cast<std::uintptr_t>(kind_u64);
                if (read_haxe_string(kind, s.weapon_kind)) {
                    s.weapon_ok = true;
                    std::int32_t lvl = 0, upg = 0;
                    mem_read_i32(weapon + OFF_ITEM_LEVEL,   &lvl);
                    mem_read_i32(weapon + OFF_ITEM_UPGRADE, &upg);
                    s.weapon_level   = (int)lvl;
                    s.weapon_upgrade = (int)upg;
                }
            }
        }
    }

    // Detect weapon kind transitions and emit weapon_changed once per
    // change. Static cache survives across publish() calls; cleared
    // (via empty s.weapon_kind) when the lock drops, so the first read
    // after a re-lock also fires the event with prev="".
    static std::string s_prev_weapon_kind;
    if (s.weapon_ok && s.weapon_kind != s_prev_weapon_kind) {
        plugins_emit_weapon_changed(s.weapon_kind.c_str(),
                                    s_prev_weapon_kind.c_str(),
                                    s.weapon_level, s.weapon_upgrade);
        s_prev_weapon_kind = s.weapon_kind;
    } else if (!s.weapon_ok && !s_prev_weapon_kind.empty()) {
        // Lost the weapon (hero re-lock, mid-swap, or read failure).
        // Don't fire an event for the transient null, just reset the
        // cache so the next observed kind re-fires as a fresh change.
        s_prev_weapon_kind.clear();
    }

    // ---- Full loadout walk ----------------------------------------
    // Throttle to ~1 Hz: gear doesn't change rapidly and the walk
    // touches user-space inventory memory that may be mid-mutation
    // while the player has the inventory window open (drag/drop).
    // Cached equipment / statuses persist between throttled refreshes
    // so plugins see a stable snapshot. publish() runs at ~15 Hz so
    // we refresh every 15th call.
    static std::vector<EquippedItem> s_cached_equipment;
    static std::vector<ActiveStatus> s_cached_statuses;
    static std::string               s_cached_class;
    static std::vector<std::string>  s_cached_weapon_skills;
    static std::vector<EquippedItem>  s_cached_inventory;
    static std::vector<CurrencyStack> s_cached_currencies;
    static std::uint64_t             s_inventory_tick = 0;
    // On a character switch every one of these still holds the PREVIOUS
    // character's data, and the blip guards below deliberately keep a cached
    // value when a scan comes back empty. So a character who genuinely owns
    // none of something inherits the last one's numbers instead of a zero.
    // Reported by @dpiza against currencies (gold carried over), but it is the
    // same for equipment, inventory, class and the arsenal. Cleared here rather
    // than from the switch itself so the caches stay private to this function.
    if (g_reset_caches.exchange(false, std::memory_order_acq_rel)) {
        s_cached_equipment.clear();
        s_cached_statuses.clear();
        s_cached_class.clear();
        s_cached_weapon_skills.clear();
        s_cached_inventory.clear();
        s_cached_currencies.clear();
        logf("hero_state: character changed, resource caches cleared");
    }
    bool refresh_inventory = (s_inventory_tick++ % 15) == 0;
    if (!refresh_inventory) {
        s.equipment      = s_cached_equipment;
        s.statuses       = s_cached_statuses;
        s.player_class   = s_cached_class;
        s.weapon_skills  = s_cached_weapon_skills;
        s.inventory      = s_cached_inventory;
        s.currencies     = s_cached_currencies;
    }
    std::uint64_t loadout_u64 = 0;
    if (refresh_inventory && mem_read_u64(a + OFF_HERO_LOADOUT, &loadout_u64)) {
        auto loadout = static_cast<std::uintptr_t>(loadout_u64);
        if (mem_is_userland(loadout)) {
            std::uint64_t equip_u64 = 0;
            if (mem_read_u64(loadout + OFF_LOADOUT_EQUIPMENT, &equip_u64)) {
                auto equipment = static_cast<std::uintptr_t>(equip_u64);
                if (mem_is_userland(equipment)) {
                    std::uint64_t arr_u64 = 0;
                    if (mem_read_u64(equipment + OFF_EQUIPMENT_CONTENT,
                                     &arr_u64)) {
                        auto arr = static_cast<std::uintptr_t>(arr_u64);
                        if (mem_is_userland(arr)) {
                            std::int32_t length = 0;
                            std::uint64_t varr_u64 = 0;
                            if (mem_read_i32(arr + OFF_ARRAYOBJ_LENGTH,
                                             &length) &&
                                mem_read_u64(arr + OFF_ARRAYOBJ_VARRAY,
                                             &varr_u64) &&
                                length > 0 &&
                                length <= kMaxEquipmentItems) {
                                auto varr = static_cast<std::uintptr_t>(
                                    varr_u64);
                                if (mem_is_userland(varr)) {
                                    auto data = varr + OFF_VARRAY_DATA;
                                    s.equipment.reserve(length);
                                    for (int i = 0; i < length; ++i) {
                                        std::uint64_t slot_u64 = 0;
                                        if (!mem_read_u64(
                                                data + i * 8, &slot_u64))
                                            continue;
                                        auto slot = static_cast<
                                            std::uintptr_t>(slot_u64);
                                        if (!mem_is_userland(slot)) continue;
                                        // #89: the slot is an hxbit proxy;
                                        // the real st.item.* is wrapped at
                                        // +48. Empty / non-item slots fail
                                        // is_stitem and are skipped.
                                        std::uint64_t item_u64 = 0;
                                        if (!mem_read_u64(
                                                slot + OFF_SLOT_ITEM,
                                                &item_u64)) continue;
                                        auto item = static_cast<
                                            std::uintptr_t>(item_u64);
                                        if (!is_stitem(item)) continue;
                                        std::uint64_t kind_u64 = 0;
                                        if (!mem_read_u64(
                                                item + OFF_ITEM_KIND,
                                                &kind_u64)) continue;
                                        EquippedItem ei;
                                        if (!read_haxe_string(
                                                static_cast<std::uintptr_t>(
                                                    kind_u64),
                                                ei.kind)) continue;
                                        std::int32_t lvl = 0, upg = 0;
                                        mem_read_i32(item + OFF_ITEM_LEVEL,
                                                     &lvl);
                                        mem_read_i32(item + OFF_ITEM_UPGRADE,
                                                     &upg);
                                        // level / upgrade only exist on the
                                        // gear subclasses. Quickslot, bag and
                                        // mount entries are base st.Item, so
                                        // these reads land on unrelated bytes
                                        // (they used to surface as things like
                                        // level=-582159368, upgrade=396).
                                        if (!is_gear_item(item)) {
                                            lvl = 0;
                                            upg = 0;
                                        }
                                        ei.level   = (lvl >= 0 && lvl < 1000)
                                                         ? (int)lvl : 0;
                                        ei.upgrade = (upg >= 0 && upg < 1000)
                                                         ? (int)upg : 0;
                                        ei.slot      = i;                 // #87
                                        ei.slot_name = equip_slot_name(i);
                                        // #105: queue the item's own icon.
                                        // Trinkets grant statuses that carry no
                                        // artwork ("PurifiedHeart" grants
                                        // "PurifiedHeart_Status"), and the icon
                                        // the game shows for that buff is the
                                        // item's. Deferred because dyn cannot
                                        // run on this thread; the drain skips
                                        // kinds already resolved.
                                        {
                                            SkillGfx have{};
                                            if (!skill_resolve_lookup_exact(
                                                    ei.kind.c_str(), &have))
                                                skill_resolve_request_item(
                                                    ei.kind.c_str(), item);
                                        }
                                        s.equipment.push_back(std::move(ei));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Active statuses walk (#110) ------------------------------
    // Unit.statuses is an hxbit.ArrayProxyData, so it takes the same
    // proxy -> ArrayDyn -> ArrayObj chase as the currencies. Everything in it
    // is on this unit already, so no owner filter is needed (or wanted: a
    // debuff a foe put on us has to survive, and we are not certain enough
    // about what `owner` means on a Status to gate the common path on it).
    //
    // The fallback runs only when that chain does not RESOLVE, not when it
    // comes back empty. An empty list is the normal state out of combat, and
    // falling back on it would walk instigatedStatuses constantly and let the
    // #110 bug back in through the side door whenever the owner check misses.
    if (refresh_inventory) {
        bool          chain_ok  = false;
        std::uint64_t proxy_u64 = 0, arrdyn_u64 = 0, arrobj_u64 = 0;
        if (mem_read_u64(a + OFF_UNIT_STATUSES, &proxy_u64)) {
            auto proxy = static_cast<std::uintptr_t>(proxy_u64);
            if (mem_is_userland(proxy) &&
                mem_read_u64(proxy + OFF_PROXY_ARRAY, &arrdyn_u64)) {
                auto arrdyn = static_cast<std::uintptr_t>(arrdyn_u64);
                if (mem_is_userland(arrdyn) &&
                    mem_read_u64(arrdyn + OFF_ARRAYDYN_ARRAY, &arrobj_u64) &&
                    mem_is_userland(
                        static_cast<std::uintptr_t>(arrobj_u64))) {
                    chain_ok = true;
                    collect_statuses(static_cast<std::uintptr_t>(arrobj_u64), 0,
                                     &s.statuses);
                }
            }
        }
        if (!chain_ok) {
            // Old source, kept so a layout change on the proxy chain costs the
            // buff bar its foreign entries rather than all of it. Filtered by
            // owner, which is the best we can do on a list that by definition
            // holds other units' statuses.
            std::uint64_t inst_u64 = 0;
            if (mem_read_u64(a + OFF_UNIT_INSTIGATED, &inst_u64))
                collect_statuses(static_cast<std::uintptr_t>(inst_u64), a,
                                 &s.statuses);
        }
        // A single "it failed once" line is useless here: the very first
        // refresh after the hero lock legitimately finds an empty proxy, so a
        // once-only warning fires on a healthy client and then never tells us
        // whether the chain came good. Count both outcomes instead and report
        // the running tally a few times, with the pointers of the last failure
        // so a broken link in the chain is identifiable from the log alone.
        {
            static int s_ok = 0, s_fail = 0, s_reports = 0;
            if (chain_ok) ++s_ok; else ++s_fail;
            int total = s_ok + s_fail;
            if (s_reports < 5 && (total == 1 || total % 40 == 0)) {
                ++s_reports;
                logf("hero: statuses source (#110) proxy_ok=%d fallback=%d "
                     "now=%zu via=%s [proxy=0x%llx arrdyn=0x%llx arrobj=0x%llx]",
                     s_ok, s_fail, s.statuses.size(),
                     chain_ok ? "Unit.statuses" : "instigatedStatuses",
                     (unsigned long long)proxy_u64,
                     (unsigned long long)arrdyn_u64,
                     (unsigned long long)arrobj_u64);
            }
        }
    }

    // ---- Player class (#90) + arsenal weapon skills (#94) ---------
    // Same ~1 Hz cadence as the inventory: class only changes on a class /
    // arsenal switch, and the weapon skills only change on a weapon / arsenal
    // swap. Don't let a transient empty scan (mid-spawn) wipe a good cache,
    // same guard as the equipment list.
    if (refresh_inventory) {
        s.player_class  = read_player_class(a);
        s.weapon_skills = read_weapon_skills(a);
        s.inventory     = read_inventory(a);       // #93 (incl. materials + stacks)
        s.currencies    = read_currencies(a);       // #93 gold + currencies
    }

    // Cache the equipment / statuses / class we just refreshed so the next
    // 14 throttled publishes can re-use them without re-reading.
    if (refresh_inventory) {
        // #89: a mid-mutation read can momentarily yield no items; don't let
        // that wipe a good cache (equipment rarely goes genuinely empty).
        if (!s.equipment.empty() || s_cached_equipment.empty())
            s_cached_equipment = s.equipment;
        else
            s.equipment = s_cached_equipment;
        s_cached_statuses = s.statuses;
        if (!s.player_class.empty() || s_cached_class.empty())
            s_cached_class = s.player_class;
        else
            s.player_class = s_cached_class;
        // #94: arsenal skills go genuinely empty when no weapon is drawn, so
        // only refresh the cache from a non-empty scan (mirrors equipment).
        if (!s.weapon_skills.empty() || s_cached_weapon_skills.empty())
            s_cached_weapon_skills = s.weapon_skills;
        else
            s.weapon_skills = s_cached_weapon_skills;
        // #93: same guard for the inventory (a mid-mutation read can blip empty).
        if (!s.inventory.empty() || s_cached_inventory.empty())
            s_cached_inventory = s.inventory;
        else
            s.inventory = s_cached_inventory;
        // #93: currencies, same blip guard.
        if (!s.currencies.empty() || s_cached_currencies.empty())
            s_cached_currencies = s.currencies;
        else
            s.currencies = s_cached_currencies;
    }

    // ---- Shield total (#70) ---------------------------------------
    // The live shield absorb lives on the active statuses (read above as
    // st.skill.Status.shieldAmount); the inline UA `shield` is a dead 0.
    // Sum the shielding statuses into s.shield. The shield_applied event and
    // the SHIELD meter are both driven from the aggregator (render thread),
    // which edge-triggers off this total, so detection lives next to the meter.
    {
        double total = 0.0;
        for (const auto& st : s.statuses)
            if (st.shield_amount > 0.0) total += st.shield_amount;
        s.shield = total;
    }

    publish_snapshot(s);
}

}  // namespace

void hero_state_start() {
    if (g_active.exchange(true)) return;
    g_locked_hero.store(0);
    g_locked_player.store(0);
    g_ticks.store(0);
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_snapshot_mu);
        g_snapshot = HeroSnapshot{};
    }
    hl_hook_register(L"ent.Hero", on_hero_alloc);
    logf("hero_state: watcher registered (render-thread tick)");
    // #71: live-attribute reader - capture the character-menu stat rows
    // (ui.hud.StatLine -> primaries, identified by localized label) + the
    // HUD Health bar (max_health) so they resolve to real values instead of
    // the inline class defaults.
    hl_hook_register(L"ui.hud.StatLine",      on_statline_alloc);
    hl_hook_register(L"ui.comp.AttributeBar", on_attr_resbar_alloc);
    // #91: hl_hook compares class names exactly, so a watcher on the base
    // class never sees these two. The Mage spark bar is an AttributeBar
    // subclass (same offsets); the block gauge is its own shape.
    hl_hook_register(L"ui.hud.hero.MageSparkBar",   on_attr_resbar_alloc);
    hl_hook_register(L"ui.comp.AttributeBlockBar",  on_attr_blockbar_alloc);
    logf("hero_state: attribute-UI watchers registered (#71)");
}

void hero_state_stop() {
    g_active.store(false);
}

// v0.4.14 (F): consecutive SEH trips before we auto-disable this whole
// module. Mirrors the auto-disable on the overlay side. Self-healing
// safety net for users whose game state happens to corrupt one of
// our pointer chases despite the type-tag check.
constexpr int               kMaxConsecutiveFailures = 5;
static std::atomic<int>     g_consecutive_failures{0};

// v0.4.15 anticrash mode. Issues #11 and #16 retest on v0.4.14 showed
// the throttled HL-read path still triggers the AV. Even at 1 damage
// alloc / minute (kesmese alt-tabbed) the trampoline overhead through
// hl_alloc_obj on every game allocation accumulates. The only fix is
// to remove the trampoline entirely once the lock is stable.
//
// When armed (data/anticrash.flag at boot, dllmain calls
// hero_state_set_anticrash(true)):
//   1. Watcher-driven lock acquisition runs normally until first lock.
//   2. We count consecutive ticks where the lock holds.
//   3. After kAnticrashStableTicks (5 s at 60 Hz), we call
//      hl_hook_disable_alloc() to surgically remove the alloc-hook
//      trampoline and damage_stop() to drop the damage pipeline.
//   4. From then on, hero_state_tick polls Player.hero each
//      throttled-publish frame to detect zone transitions; no more
//      alloc-hook events ever arrive.
//
// Trade-off: DPS tracking stops. Users opt in via the flag file.
constexpr std::uint64_t           kAnticrashStableTicks = 300;
static std::atomic<bool>          g_anticrash_on{false};
static std::atomic<bool>          g_anticrash_disarmed{false};
static std::atomic<std::uint64_t> g_lock_stable_ticks{0};

// Read Hero pointer via the back-reference from the cached Player.
// Player itself survives zone transitions; only Hero gets swapped.
// Used after anticrash disarm to detect zone-transition re-locks
// without needing the alloc-hook watcher.
std::uintptr_t poll_hero_via_player() {
    std::uintptr_t player = g_locked_player.load(std::memory_order_acquire);
    if (!player || !mem_is_userland(player)) return 0;
    std::uint64_t hero_u64 = 0;
    if (!mem_read_u64(player + OFF_PLAYER_HERO, &hero_u64)) return 0;
    auto hero = static_cast<std::uintptr_t>(hero_u64);
    if (!hero || !mem_is_userland(hero)) return 0;
    return hero;
}

static void hero_state_tick_body(std::uint64_t n, std::uintptr_t locked) {
    // #71: drain any captured character-menu / HUD stat widgets and refresh
    // the live-attribute cache (consumed by attrui_apply during publish).
    attrui_process();
    // v0.4.15 anticrash post-disarm path: alloc-hook is gone, no more
    // watcher events arrive. Poll Player.hero on the throttled-publish
    // cadence to detect zone-transition re-locks. is_local_hero still
    // re-validates correctness via the position-plausibility check.
    // v0.4.15.1: self-heal addition - when polling loses the lock
    // (zone transition that swapped BOTH Hero and Player, e.g. dungeon
    // instance entry), re-arm the alloc-hook so the watcher catches
    // the new Hero alloc. The next stable lock then disarms again.
    // Self-healing loop: alloc-hook only armed during transitions,
    // not during steady-state play.
    if (g_anticrash_disarmed.load(std::memory_order_acquire)) {
        if ((n & 0x3) == 0) {
            std::uintptr_t fresh = poll_hero_via_player();
            if (fresh && fresh != locked && is_local_hero(fresh)) {
                g_locked_hero.store(fresh, std::memory_order_release);
                logf("hero_state: polling RE-LOCKED via Player.hero "
                     "@ 0x%llx (tick %llu, prev=0x%llx)",
                     (unsigned long long)fresh, (unsigned long long)n,
                     (unsigned long long)locked);
                locked = fresh;
            } else if (locked && !is_local_hero(locked)) {
                logf("hero_state: polling LOST lock at tick %llu - "
                     "self-heal: re-arming alloc-hook",
                     (unsigned long long)n);
                g_locked_hero.store(0);
                g_locked_player.store(0);
                g_unlocked_alloc_log_left.store(16);
                if (hl_hook_re_enable_alloc()) {
                    g_anticrash_disarmed.store(false,
                                               std::memory_order_release);
                    g_lock_stable_ticks.store(0,
                                              std::memory_order_release);
                }
                locked = 0;
                return;   // next tick will use the normal armed branch
            }
            publish();
        }
        if (locked && (n % 600 == 0)) {
            logf("hero_state[anticrash]: pos=(%.1f, %.1f, %.1f) tick %llu",
                 g_snapshot.x, g_snapshot.y, g_snapshot.z,
                 (unsigned long long)n);
        }
        return;
    }

    // v0.4.14 (D): cap drain per tick so a burst of allocs (city scene
    // with many players entering range, dungeon-exit burst of ~16
    // template Heroes) can't run dozens of is_local_hero checks in
    // a single frame.
    constexpr std::size_t kMaxDrainPerTick = 8;
    std::vector<Pending> work;
    std::vector<Pending> remainder;
    {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        std::size_t take = std::min(kMaxDrainPerTick, g_pending.size());
        work.assign(g_pending.begin(), g_pending.begin() + take);
        if (take < g_pending.size()) {
            remainder.assign(g_pending.begin() + take, g_pending.end());
        }
        g_pending.clear();
    }
    std::vector<Pending> retry;
    retry.reserve(work.size());
    std::uintptr_t found = 0;
    for (Pending p : work) {
        if (is_local_hero(p.hero_ptr)) {
            // Prefer the newest match in this batch (the dungeon-exit
            // burst allocates ~16 Heros back-to-back; only the last is
            // the live one - the rest are sync-proxies / templates).
            found = p.hero_ptr;
        } else if (++p.retries < kMaxPendingRetries) {
            retry.push_back(p);
        }
    }
    if (found && found != locked) {
        std::uintptr_t player = read_owner_player(found);
        g_locked_player.store(player, std::memory_order_release);
        g_locked_hero.store(found, std::memory_order_release);
        const char* verb = (locked == 0) ? "LOCKED" : "SWITCHED";
        logf("hero_state: %s Hero @ 0x%llx (player=0x%llx, tick %llu, "
             "prev=0x%llx)",
             verb,
             (unsigned long long)found,
             (unsigned long long)player,
             (unsigned long long)n,
             (unsigned long long)locked);
        debug_dump_hero(verb, found);
        plugins_emit_hero_locked();
        progress_state_inspect(found);   // Phase 2A diagnostic
        locked = found;
    } else if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.insert(g_pending.end(), retry.begin(), retry.end());
    }
    // v0.4.14 (D): push back whatever we left untouched this tick.
    if (!remainder.empty()) {
        std::lock_guard<std::mutex> lk(g_pending_mu);
        g_pending.insert(g_pending.begin(),
                         remainder.begin(), remainder.end());
    }

    // Re-validate the current lock every 4 ticks (~70 ms) and drop if
    // dead. Dropping is the safety net - the preempting switch above
    // handles the common case where a fresh Hero appears before the
    // old one is invalidated.
    if (locked && (n & 0x3) == 0) {
        if (!is_local_hero(locked)) {
            logf("hero_state: re-validation FAILED at tick %llu",
                 (unsigned long long)n);
            debug_dump_hero("invalid_locked", locked);

            std::uintptr_t fresh = try_relock_via_player();
            if (fresh) {
                g_locked_hero.store(fresh, std::memory_order_release);
                logf("hero_state: RE-LOCKED via Player.hero "
                     "@ 0x%llx (tick %llu)",
                     (unsigned long long)fresh, (unsigned long long)n);
            } else {
                g_locked_hero.store(0);
                g_locked_player.store(0);
                g_unlocked_alloc_log_left.store(16);
                logf("hero_state: DROPPED lock (no fresh Hero "
                     "available)");
            }
        }
    }

    // v0.4.14 (B): the original throttle (every 4th frame = 15 Hz at
    // 60 Hz Present) was a budget for the render thread. v0.6.0
    // workermode runs at 20 Hz total - quartering that to 5 Hz makes
    // the minimap dot visibly judder. Worker tick body is cheap
    // (~8 mem_reads per publish = 160 reads/s at 20 Hz, well below
    // the old 15 Hz budget on Present). Just publish every tick.
    publish();

    // v0.4.15 anticrash arm/disarm. If the flag was set at boot and we
    // currently hold a lock, count consecutive stable ticks. After 5 s
    // (300 ticks) of uninterrupted lock, surgically remove the
    // hl_alloc_obj trampoline and stop damage tracking. From the next
    // tick onward the body's anticrash-post-disarm short-circuit at the
    // top kicks in and we poll Player.hero instead of watcher events.
    if (g_anticrash_on.load(std::memory_order_acquire) &&
        !g_anticrash_disarmed.load(std::memory_order_acquire)) {
        if (locked) {
            std::uint64_t stable = g_lock_stable_ticks.fetch_add(
                                       1, std::memory_order_relaxed) + 1;
            if (stable >= kAnticrashStableTicks) {
                logf("hero_state: anticrash trigger at tick %llu - "
                     "removing hl_alloc_obj hook and damage pipeline",
                     (unsigned long long)n);
                damage_stop();
                hl_hook_disable_alloc();
                g_anticrash_disarmed.store(true,
                                           std::memory_order_release);
            }
        } else {
            // Lost the lock before we got to disarm. Reset counter so
            // we wait another 5 s after the next stable lock.
            g_lock_stable_ticks.store(0, std::memory_order_relaxed);
        }
    }

    // Heartbeat: log current position every ~10 s so we can sanity-
    // check that the locked Hero is actually moving with the player.
    if (locked && (n % 600 == 0)) {
        logf("hero_state: pos=(%.1f, %.1f, %.1f) rot=%.3f (tick %llu)",
             g_snapshot.x, g_snapshot.y, g_snapshot.z, g_snapshot.rot_z,
             static_cast<unsigned long long>(n));
    }
}

void hero_state_tick() {
    if (!g_active.load(std::memory_order_acquire)) return;
    std::uint64_t n = g_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uintptr_t locked = g_locked_hero.load(std::memory_order_acquire);
    // v0.4.14 (F): SEH-wrap the body. If any of our mem_reads or
    // dyn-followed pointer chases trips an AV the structured exception
    // catches it; we count consecutive failures and auto-disable the
    // whole module after a threshold so the game stays alive even if
    // our reads keep tripping. The body cannot itself contain C++
    // objects with destructors directly, hence the separate function.
    __try {
        hero_state_tick_body(n, locked);
        g_consecutive_failures.store(0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        int fails = g_consecutive_failures.fetch_add(1) + 1;
        logf("hero_state: SEH trip #%d in tick %llu (code 0x%08lx)",
             fails, (unsigned long long)n,
             GetExceptionCode());
        if (fails >= kMaxConsecutiveFailures) {
            g_active.store(false);
            g_locked_hero.store(0);
            g_locked_player.store(0);
            logf("hero_state: %d consecutive SEH trips - auto-disabling "
                 "module to keep the game alive. Restart to re-enable.",
                 fails);
        }
    }
}

HeroSnapshot hero_state_read() {
    // v0.5.3.2 (#2): guarded copy. Render thread writes via
    // publish_snapshot; overlay-thread plugin getters read here.
    // Without the lock the ~480-byte
    // memcpy across threads can tear.
    std::lock_guard<std::mutex> lk(g_snapshot_mu);
    return g_snapshot;
}

std::vector<UiStat> hero_state_ui_stats() {
    std::lock_guard<std::mutex> lk(g_stats_mu);
    return g_stats;
}

void hero_state_reset_caches() {
    // Everything here belongs to the character we just left. The loadout
    // caches are function-local to publish(), so they get a flag; the rest we
    // can clear directly, all of it either atomic or mutex-guarded.
    g_reset_caches.store(true, std::memory_order_release);
    g_attrui.reset();
    {
        std::lock_guard<std::mutex> lk(g_stats_mu);
        g_stats.clear();
    }
    // g_resbars is deliberately left alone: it is touched only from
    // attrui_process on the render thread, and each entry is re-validated
    // against the locked hero every pass, so the old character's bars drop out
    // there on their own. Clearing it from this thread would be the race.
}

std::uintptr_t hero_state_locked_ptr() {
    return g_locked_hero.load(std::memory_order_acquire);
}

// #62: read ent.Hero.name @ 1256 (a Haxe String: bytes ptr @ +8, UTF-16
// length @ +16) off the locked hero, encode BMP code points to UTF-8. [v018]
bool hero_state_local_name(std::string& out) {
    out.clear();
    std::uintptr_t hero = hero_state_locked_ptr();
    if (!hero) return false;

    std::uint64_t name_ptr = 0;
    if (!mem_read_u64(hero + OFF_HERO_NAME, &name_ptr)) return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(name_ptr))) return false;

    std::uint64_t bytes_ptr = 0;
    if (!mem_read_u64(static_cast<std::uintptr_t>(name_ptr) + 8, &bytes_ptr))
        return false;
    if (!mem_is_userland(static_cast<std::uintptr_t>(bytes_ptr))) return false;

    std::int32_t len = 0;
    if (!mem_read_i32(static_cast<std::uintptr_t>(name_ptr) + 16, &len))
        return false;
    if (len <= 0) return false;
    if (len > 64) len = 64;                       // names are short; clamp

    for (int i = 0; i < len; ++i) {
        std::uint16_t cp = 0;
        if (!mem_read_bytes(static_cast<std::uintptr_t>(bytes_ptr) +
                                static_cast<std::size_t>(i) * 2,
                            &cp, sizeof(cp)))
            break;
        if (cp == 0) break;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return !out.empty();
}

// #87: the local player's stable account uid (st.Player.uid@176, a String). [v018]
// Unlike Hero.__uid (per-login, changes every session), this is the constant
// id FareverLogs uses to correlate one player's logs across sessions. Empty
// until a player is locked or if the field isn't populated client-side.
// Logged once so it's verifiable from a normal farever-mod.log.
bool hero_state_local_uid(std::string& out) {
    out.clear();
    std::uintptr_t player = g_locked_player.load(std::memory_order_acquire);
    if (!player || !mem_is_userland(player)) return false;
    std::uint64_t uid_ptr = 0;
    if (!mem_read_u64(player + OFF_PLAYER_UID, &uid_ptr)) return false;
    if (!read_haxe_string(static_cast<std::uintptr_t>(uid_ptr), out)) {
        out.clear();
        return false;
    }
    static bool s_logged = false;
    if (!s_logged && !out.empty()) {
        s_logged = true;
        logf("hero_state: player uid (#87) resolved -> \"%s\"", out.c_str());
    }
    return !out.empty();
}

void hero_state_set_anticrash(bool on) {
    g_anticrash_on.store(on);
    if (!on) g_lock_stable_ticks.store(0);
}

bool hero_state_anticrash_armed()    { return g_anticrash_on.load(); }
bool hero_state_anticrash_disarmed() { return g_anticrash_disarmed.load(); }

}  // namespace farever
