#pragma once

#include <cstddef>
#include <cstdint>

namespace farever {

// One local-player heal event (effect==1 DamageResult, weakSource==my UID).
struct HealEvent {
    std::uintptr_t dr_ptr;        // dedupe / id
    double         heal;          // _amount
    std::int32_t   hit_count;     // _hitCount (tick ordinal; we count 1/event)
    std::uint8_t   is_crit;
    char           skill[64];     // ASCII, NUL-terminated
    char           target_name[64];  // #87: heal recipient name (self / ally),
                                     // same form as farever.target.name()
};

// Register the st.skill.DamageResult watcher (call before hl_hook_install,
// next to damage_start). Drive heal_tick() from the same place(s) as
// damage_tick(). heal_drain() hands events to the aggregator.
void heal_start();
void heal_tick();
std::size_t heal_drain(HealEvent* out, std::size_t max);

}  // namespace farever
