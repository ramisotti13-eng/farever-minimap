#pragma once

#include <cstdint>

namespace farever {

// Single party member, as published by the hl_pump worker to the
// render thread. hero_valid == false means the member's Hero
// pointer was null or pos was (0,0,0) this tick. uid is the hxbit
// __uid (stable per login).
struct PartyMember {
    char         name[64];
    char         class_id[64];   // #73: ent.Unit.kind id (empty if unread)
    std::int64_t uid;
    double       x, y, z;
    double       rot_z;
    double       health;         // #73: current HP (0 if attr unread)
    double       max_health;     // #73: max HP   (0 if attr unread)
    bool         attr_ok;        // #73: health/max_health read succeeded
    bool         hero_valid;
};

// Group_MaxPlayers in the game data table is 5; we publish at most
// five OTHER members (the local player is filtered out before
// publish). count == 0 means solo / no group / not resolved yet.
struct PartySnapshot {
    int          count;
    PartyMember  members[5];
};

// Register the alloc-hook watchers (st.Player + st.Group) so
// hl_hook_get_type fills the type cache for both classes the first
// time the game allocates one. Call once at module init, before any
// tick runs. The Group watcher only fires when the player joins a
// party; Player fires during world load (every player has one).
void party_state_start();

// Per-tick HL read; runs from the existing hl_pump worker.
void party_state_tick();

// Render-thread accessor. Brief mutex + copy into thread-local block.
const PartySnapshot& party_state_read();

}  // namespace farever
