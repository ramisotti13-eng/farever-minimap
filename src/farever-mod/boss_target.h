#pragma once

#include <cstdint>
#include <string>

namespace farever {

// Process-wide "boss we are currently fighting" cache (#69).
//
// Boss-only-dungeon bosses expose their autoTarget as an hxbit UID that the
// target_state resolver can't turn into a pointer: our alloc-hook UID registry
// only watches ent.Foe and ent.Hero, not ent.boss.*. So farever.target stayed
// empty for the whole fight even though the boss timer named them fine.
//
// The damage path, however, sees the boss pointer directly on every outgoing
// hit (DamageResult.target). It records the boss here, and target_state falls
// back to this when its normal resolution comes up empty. Written on the render
// thread (damage decode), read on the hl_pump worker thread (target publish),
// so access is mutex-guarded.

// Note an outgoing hit on an ent.boss.* target. class_name is the raw HashLink
// class (e.g. "ent.boss.Mokshi"); the display name is derived and stored.
void boss_target_note_hit(std::uintptr_t boss_ptr, const char* class_name);

// If a boss was hit within the freshness window, fill out_ptr/out_name and
// return true. Returns false when nothing recent is cached. The caller still
// validates the pointer (type-anchor) before reading from it.
bool boss_target_recent(std::uintptr_t* out_ptr, std::string* out_name);

}  // namespace farever
