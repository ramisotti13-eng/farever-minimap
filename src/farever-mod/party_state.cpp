#include "party_state.h"
#include "hero_state.h"   // hero_state_locked_ptr
#include "hl_hook.h"      // hl_hook_register, hl_hook_get_type
#include "log.h"
#include "mem_scan.h"     // mem_read_*, mem_is_userland

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace farever {
namespace {

// Field offsets, verified in tools/classes_v016.json (2026-05-28 dump).
// Same as classes_v0152 in practice - the layouts didn't shift.
// ent.Hero (super ent.Unit -> ent.GameObject -> ent.Serializable):
constexpr std::size_t OFF_HERO_POS_X         = 176;  // GameObject pos f64 (was 152) [v018 +16]
constexpr std::size_t OFF_HERO_POS_Y         = 184;
constexpr std::size_t OFF_HERO_POS_Z         = 192;
constexpr std::size_t OFF_HERO_ROT_Z         = 200;
constexpr std::size_t OFF_HERO_NAME          = 1280; // ent.Hero.name was 1272 [v023 +8]
constexpr std::size_t OFF_UNIT_KIND          = 600;  // ent.Unit.kind (String) #73 was 592 [v023 +8]
constexpr std::size_t OFF_UNIT_ATTR          = 984;  // ent.Unit.attr was 976 [v023 +8] #73 health
constexpr std::size_t OFF_ATTR_BLOCK_BASE    = 48;   // first f64 in the attr block
constexpr int         UA_IDX_HEALTH          = 24;   // 48 + 24*8 = 240
constexpr int         UA_IDX_MAX_HEALTH      = 25;   // 48 + 25*8 = 248
constexpr std::size_t OFF_UNIT_UID           = 32;   // hxbit __uid
// ent.Hero -> st.Player*. MUST match hero_state.cpp (which locks the local
// player off this field and works): it is 16, NOT 296. The old 296 pointed
// into unrelated memory, so every downstream read (player name, group) was
// garbage -- that was why party matching never worked.
constexpr std::size_t OFF_HERO_OWNERPLAYER   = 16;

// st.Player:
constexpr std::size_t OFF_PLAYER_NAME        = 192;  // was 168 [v018 +16]
constexpr std::size_t OFF_PLAYER_GROUP       = 256;  // was 232 [v018 +16]
constexpr std::size_t OFF_PLAYER_UID         = 32;   // hxbit __uid (unchanged)

// st.Group.groupId (HI64) -- the server-side party identity. Group objects are
// hxbit-replicated, so the Group POINTER differs between clients/replicas;
// groupId is stable and shared across party members, so we match on it.
constexpr std::size_t OFF_GROUP_ID           = 176;  // was 152 [v018 +16]
constexpr std::size_t OFF_GROUP_PLAYERS      = 184;  // st.Group.players (was 160) [v018 +16]
constexpr std::size_t OFF_PLAYER_HERO        = 304;  // st.Player.hero (was 280) [v018 +16]

// Party-roster chase. st.Group.players is an hxbit.ArrayProxyData whose
// .array@40 is an hl.types.ArrayDyn; ArrayDyn.array@8 is an ArrayObj with
// length@8 (HI32) and a backing hl varray@16. The varray header is
// {type, elem-type, size, pad} = 24 bytes, so the element pointers
// (st.Player*) start at +24, 8 bytes each. #98: this used to read from +16,
// which shifted every slot down by one and dropped the last roster entry
// (invisible in a duo, where the dropped entry was usually the local player).
constexpr std::size_t OFF_PROXY_ARRAY        = 40;
constexpr std::size_t OFF_ARRAYDYN_ARRAY     = 8;
constexpr std::size_t OFF_ARRAYOBJ_LENGTH    = 8;
constexpr std::size_t OFF_ARRAYOBJ_VARRAY    = 16;
constexpr std::size_t OFF_VARRAY_SIZE        = 16;
constexpr std::size_t OFF_VARRAY_DATA        = 24;

// Haxe String:
constexpr std::size_t OFF_STR_BYTES          = 8;
constexpr std::size_t OFF_STR_LEN            = 16;

constexpr int kMaxKnownHeroes = 256;   // cap on tracked Hero pointers

std::mutex                     g_mu;
PartySnapshot                  g_snap{};

std::mutex                     g_heroes_mu;
std::vector<std::uintptr_t>    g_known_heroes;

std::atomic<std::uintptr_t>    g_hero_type{0};
std::atomic<std::uintptr_t>    g_player_type{0};

// ---- helpers ---------------------------------------------------------

std::uintptr_t read_ptr(std::uintptr_t base, std::size_t off) {
    std::uint64_t v = 0;
    if (!mem_read_u64(base + off, &v)) return 0;
    auto p = static_cast<std::uintptr_t>(v);
    if (!mem_is_userland(p)) return 0;
    return p;
}

// Haxe String -> ASCII char[]. Returns true on any successful read.
bool read_haxe_string(std::uintptr_t s_ptr, char out[64]) {
    out[0] = '\0';
    if (!mem_is_userland(s_ptr)) return false;
    auto bytes_ptr = read_ptr(s_ptr, OFF_STR_BYTES);
    if (!bytes_ptr) return false;
    std::int32_t length = 0;
    if (!mem_read_i32(s_ptr + OFF_STR_LEN, &length)) return false;
    if (length < 0 || length > 256) return false;
    int max_chars = length < 63 ? length : 63;
    for (int i = 0; i < max_chars; ++i) {
        std::uint16_t cp = 0;
        if (!mem_read_bytes(bytes_ptr + static_cast<std::size_t>(i) * 2,
                            &cp, sizeof(cp))) {
            out[i] = '\0';
            return i > 0;
        }
        out[i] = (cp < 0x80) ? static_cast<char>(cp) : '?';
    }
    out[max_chars] = '\0';
    return max_chars > 0;
}

// Cheap structural check: header pointer at +0 matches the cached
// ent.Hero hl_type. Catches GC slot reuse.
bool hero_ptr_still_valid(std::uintptr_t hero) {
    auto expected = g_hero_type.load();
    if (!expected) return true;   // pre-anchor: trust optimistically
    std::uint64_t hdr = 0;
    if (!mem_read_u64(hero, &hdr)) return false;
    return hdr == static_cast<std::uint64_t>(expected);
}

// ---- alloc watchers --------------------------------------------------

void on_player_alloc(std::uintptr_t /*obj*/) {
    // No-op; we only need the alloc to fill the type cache so we can
    // do plausibility checks elsewhere.
}

void on_group_alloc(std::uintptr_t /*obj*/) {
    // No-op.
}

void on_hero_alloc_party(std::uintptr_t hero) {
    if (!hero) return;
    std::lock_guard<std::mutex> lk(g_heroes_mu);
    // De-dup: skip if we already have this pointer.
    for (auto p : g_known_heroes) if (p == hero) return;
    if (g_known_heroes.size() >= kMaxKnownHeroes) {
        // Prune obviously-dead entries (type-tag mismatch) first.
        g_known_heroes.erase(
            std::remove_if(g_known_heroes.begin(), g_known_heroes.end(),
                           [](std::uintptr_t p) {
                               return !hero_ptr_still_valid(p);
                           }),
            g_known_heroes.end());
        // If still capped, drop the oldest.
        if (g_known_heroes.size() >= kMaxKnownHeroes) {
            g_known_heroes.erase(g_known_heroes.begin());
        }
    }
    g_known_heroes.push_back(hero);
}

// ---- per-member decode ----------------------------------------------

bool decode_member(std::uintptr_t hero, PartyMember& out) {
    out = PartyMember{};

    // Read pos/rot. Plausibility filter: (0,0,0) means sync-proxy, skip.
    double px = 0, py = 0, pz = 0, rz = 0;
    bool ok =
        mem_read_bytes(hero + OFF_HERO_POS_X, &px, sizeof(px)) &&
        mem_read_bytes(hero + OFF_HERO_POS_Y, &py, sizeof(py)) &&
        mem_read_bytes(hero + OFF_HERO_POS_Z, &pz, sizeof(pz)) &&
        mem_read_bytes(hero + OFF_HERO_ROT_Z, &rz, sizeof(rz));
    if (!ok) return false;

    // Name: read Hero.name@1240 directly. The ownerPlayer->Player.name chase is
    // garbage for hxbit-replicated (non-local) players, but Hero.name carries
    // the real character name on every replica. uid from Hero.__uid.
    char name[64] = {};
    std::uintptr_t hn = read_ptr(hero, OFF_HERO_NAME);
    if (hn) read_haxe_string(hn, name);
    std::int64_t h_uid = 0;
    mem_read_bytes(hero + OFF_UNIT_UID, &h_uid, sizeof(h_uid));
    out.uid = h_uid;
    std::strncpy(out.name, name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = '\0';

    // #73: class id (Unit.kind) + current/max health for party HUD plugins.
    // Same UnitAttributes layout the local hero and targets use; these are
    // replicated fields, so they may read 0 on a member whose attr replica
    // hasn't synced yet (attr_ok flags whether the read landed).
    char cls[64] = {};
    std::uintptr_t kind_ptr = read_ptr(hero, OFF_UNIT_KIND);
    if (kind_ptr) read_haxe_string(kind_ptr, cls);
    std::strncpy(out.class_id, cls, sizeof(out.class_id) - 1);
    out.class_id[sizeof(out.class_id) - 1] = '\0';

    std::uintptr_t attr = read_ptr(hero, OFF_UNIT_ATTR);
    if (attr) {
        double ua[UA_IDX_MAX_HEALTH + 1] = {};
        if (mem_read_bytes(attr + OFF_ATTR_BLOCK_BASE, ua, sizeof(ua))) {
            out.health     = ua[UA_IDX_HEALTH];
            out.max_health = ua[UA_IDX_MAX_HEALTH];
            out.attr_ok    = true;
        }
    }

    if (px == 0.0 && py == 0.0 && pz == 0.0) {
        out.hero_valid = false;
    } else {
        out.hero_valid = true;
    }
    out.x     = px;
    out.y     = py;
    out.z     = pz;
    out.rot_z = rz;
    return true;
}

}  // namespace

void party_state_start() {
    hl_hook_register(L"st.Player", on_player_alloc);
    hl_hook_register(L"st.Group",  on_group_alloc);
    hl_hook_register(L"ent.Hero",  on_hero_alloc_party);
    logf("party_state: watchers registered (st.Player, st.Group, ent.Hero)");
}

void party_state_tick() {
    // Pick up the cached type pointers.
    if (g_hero_type.load() == 0) {
        auto t = hl_hook_get_type(L"ent.Hero");
        if (t) {
            g_hero_type.store(t);
            logf("party: anchored ent.Hero hl_type=0x%llx",
                 static_cast<unsigned long long>(t));
        }
    }
    if (g_player_type.load() == 0) {
        auto t = hl_hook_get_type(L"st.Player");
        if (t) {
            g_player_type.store(t);
            logf("party: anchored st.Player hl_type=0x%llx",
                 static_cast<unsigned long long>(t));
        }
    }

    static int s_diag_throttle = 0;
    bool do_log = (++s_diag_throttle % 200) == 0;

    PartySnapshot s{};

    std::uintptr_t self_hero = hero_state_locked_ptr();
    if (!self_hero) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
        return;
    }

    std::uintptr_t self_player = read_ptr(self_hero, OFF_HERO_OWNERPLAYER);
    if (!self_player) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
        return;
    }

    std::uintptr_t self_group = read_ptr(self_player, OFF_PLAYER_GROUP);
    if (!self_group) {
        if (do_log) logf("party: solo (self_player.group=NULL)");
        std::lock_guard<std::mutex> lk(g_mu);
        g_snap = s;
        return;
    }

    // Party roster walk. self_group.players (hxbit.ArrayProxyData) -> ArrayDyn
    // -> ArrayObj {length, varray}; each varray slot is a party member's
    // st.Player*, and Player.hero@280 is that member's Hero on our client. We
    // read pos + Hero.name@1240 off it. This replaces the old "scan every
    // server hero and match by Player.group" approach, which cannot work:
    // group@232 reads 0 on hxbit-replicated (non-local) players.
    int matched    = 0;
    int roster_len = 0;
    std::uintptr_t proxy  = read_ptr(self_group, OFF_GROUP_PLAYERS);
    std::uintptr_t arrdyn = proxy  ? read_ptr(proxy,  OFF_PROXY_ARRAY)     : 0;
    std::uintptr_t arrobj = arrdyn ? read_ptr(arrdyn, OFF_ARRAYDYN_ARRAY)  : 0;
    std::uintptr_t varr   = arrobj ? read_ptr(arrobj, OFF_ARRAYOBJ_VARRAY) : 0;
    if (arrobj) mem_read_i32(arrobj + OFF_ARRAYOBJ_LENGTH, &roster_len);
    // The backing varray must hold at least roster_len slots; if it does not,
    // this is not an hl varray header and the element walk would read garbage.
    std::int32_t varr_size = 0;
    if (varr && (!mem_read_i32(varr + OFF_VARRAY_SIZE, &varr_size) ||
                 varr_size < roster_len))
        varr = 0;

    const int kCap = static_cast<int>(sizeof(s.members) / sizeof(s.members[0]));
    if (varr && roster_len > 0 && roster_len < 256) {
        for (int i = 0; i < roster_len && s.count < kCap; ++i) {
            std::uintptr_t player =
                read_ptr(varr, OFF_VARRAY_DATA + static_cast<std::size_t>(i) * 8);
            if (!player || player == self_player) continue;   // skip empty + me
            std::uintptr_t hero = read_ptr(player, OFF_PLAYER_HERO);
            if (!hero || hero == self_hero) continue;
            PartyMember m{};
            if (!decode_member(hero, m)) continue;
            s.members[s.count++] = m;
            ++matched;
        }
    }

    if (do_log) {
        logf("party: roster_len=%d matched=%d self_group=0x%llx",
             roster_len, matched, static_cast<unsigned long long>(self_group));
        // Light fallback only if the roster chain breaks, to localize it.
        if (matched == 0 && proxy)
            logf("party: (no members) proxy=0x%llx arrdyn=0x%llx arrobj=0x%llx "
                 "varr=0x%llx", (unsigned long long)proxy,
                 (unsigned long long)arrdyn, (unsigned long long)arrobj,
                 (unsigned long long)varr);
    }

    std::lock_guard<std::mutex> lk(g_mu);
    g_snap = s;
}

const PartySnapshot& party_state_read() {
    thread_local PartySnapshot s_local{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s_local = g_snap;
    }
    return s_local;
}

}  // namespace farever
