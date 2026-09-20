#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace farever {

// Rare/sparkling-mob proximity alert (#65, the "Silver Dragon" idea). Watches
// ent.Foe allocations, and each worker tick checks every live foe within the
// alert range against a user-defined list of name substrings (matched on
// ent.Unit.kind, case-insensitive). Matches are published for the overlay to
// draw a banner + minimap/compass markers, and a beep fires once when a new
// matching mob enters range.

// One matching foe within range, published for the render thread.
struct MobHit {
    std::int64_t uid;        // ent.Unit.__uid (stable identity, rising-edge beep)
    char         kind[64];   // the ent.Unit.kind that matched
    char         term[32];   // the watch term that matched it
    double       x, y, z;    // world position
    double       dist;       // planar distance from the hero (world units ~ m)
};

struct MobWatchSnapshot {
    bool   enabled;
    int    count;
    MobHit hits[16];
};

// Register the ent.Foe alloc watcher and load the saved config. Call once at
// module init, BEFORE hl_hook_install (same as the other watchers).
void mob_watch_start();

// Per-tick scan; runs on the hl_pump worker after hero_state_tick.
void mob_watch_tick();

// Render-thread accessor. Brief mutex + copy into a thread-local block.
const MobWatchSnapshot& mob_watch_read();

// --- config, driven from the Options panel (render thread) ------------------
bool                     mob_watch_enabled();
void                     mob_watch_set_enabled(bool on);
// #78: built-in "Sparkling bosses" toggle. When on, foes whose kind contains
// "Sparkling" (the rare boss variants) are matched + shown on the map even
// with no custom watch terms, and the scan runs regardless of mob_watch_enabled.
bool                     mob_watch_sparkling();
void                     mob_watch_set_sparkling(bool on);
double                   mob_watch_range();          // world units
void                     mob_watch_set_range(double r);
std::vector<std::string> mob_watch_terms();          // copy of the watch list
void                     mob_watch_add_term(const char* term);
void                     mob_watch_remove_term(const char* term);

}  // namespace farever
