// Boss-target cache: see boss_target.h. Tiny mutex-guarded slot fed by the
// damage path and read by target_state's publish().

#include "boss_target.h"
#include "boss_names.h"   // boss_display_name

#include <windows.h>

#include <mutex>
#include <string>

namespace farever {
namespace {

// How long a boss hit stays "current" with no further hits. A boss fight keeps
// you attacking continuously, so this only needs to bridge brief gaps (dodging,
// a phase transition) without leaving a stale boss pinned after the fight ends.
constexpr DWORD kFreshMs = 8000;

std::mutex     g_mu;
std::uintptr_t g_ptr  = 0;
std::string    g_name;
DWORD          g_tick = 0;

}  // namespace

void boss_target_note_hit(std::uintptr_t boss_ptr, const char* class_name) {
    if (!boss_ptr) return;
    std::string display = boss_display_name(class_name ? class_name : "");
    std::lock_guard<std::mutex> lk(g_mu);
    g_ptr  = boss_ptr;
    g_name = std::move(display);
    g_tick = GetTickCount();
}

bool boss_target_recent(std::uintptr_t* out_ptr, std::string* out_name) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ptr) return false;
    if (GetTickCount() - g_tick > kFreshMs) return false;
    if (out_ptr)  *out_ptr  = g_ptr;
    if (out_name) *out_name = g_name;
    return true;
}

}  // namespace farever
