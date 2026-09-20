#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace farever {

enum class BossRunStatus { Idle, Running };

struct BossRecord {
    std::string         name;          // display name, e.g. "Crabgantua"
    double              best_s  = 0.0; // personal best clear time (0 = none)
    int                 kills   = 0;
    int                 deaths  = 0;   // deaths to this boss (for attempts)
    double              sum_s   = 0.0; // sum of clear times (for average)
    double              worst_s = 0.0; // slowest clear
    double              last_s  = 0.0; // most recent clear
    std::vector<double> recent;        // last 5 clear times, newest last
};

struct BossTimerSnapshot {
    bool                    enabled      = false;
    BossRunStatus           status       = BossRunStatus::Idle;
    std::string             current_boss;           // display name while Running
    double                  elapsed_s    = 0.0;     // live timer while Running
    double                  current_pb_s = 0.0;     // PB for the current boss
    bool                    has_pb       = false;
    std::string             last_boss;              // most recent clear
    double                  last_clear_s = 0.0;
    bool                    last_was_pb  = false;
    std::vector<BossRecord> records;                // all bosses (idle table)
    int                     session_kills     = 0;   // clears since launch
    double                  session_fastest_s = 0.0; // fastest clear this session
    double                  last_clear_age_s  = 1e9; // seconds since last clear
};

// Init (no alloc watcher; the boss pointer arrives from the damage path).
void boss_timer_start();

// Called from the damage path on an OUTGOING hit whose target is a boss.
// class_name is the raw HashLink class (e.g. "ent.boss.Crabgantua"). is_kill is
// the DamageResult kill flag, the reliable "this hit killed the boss" signal.
void boss_timer_on_boss_hit(std::uintptr_t boss_ptr, const char* class_name,
                            bool is_kill);

// Drive from the hl_pump worker tick.
void boss_timer_tick();

// Load the per-character record file (called next to poi_progress_set_profile).
void boss_timer_set_profile(const char* key);

// Mode gate. When off, on_boss_hit and tick are no-ops.
void boss_timer_set_enabled(bool on);
bool boss_timer_enabled();

// Published copy for the overlay (no game memory access).
BossTimerSnapshot boss_timer_snapshot();

// Record management (called from the overlay panel). Per active character.
void boss_timer_reset(const char* display_name);   // erase one boss's record
void boss_timer_reset_all();                        // erase all records

}  // namespace farever
