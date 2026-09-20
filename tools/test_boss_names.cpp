// Standalone unit test for the pure boss-name / time helpers.
// Build (MSVC, from the repo root, in a VS developer prompt):
//   cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_boss_names.cpp /Fe:test_boss_names.exe && test_boss_names.exe'
#include "boss_names.h"
#include <cassert>
#include <cstdio>

using namespace farever;

int main() {
    // is_boss_type: prefix match on "ent.boss."
    assert(is_boss_type("ent.boss.Crabgantua"));
    assert(is_boss_type("ent.boss.MunsterChuck"));
    assert(!is_boss_type("ent.Foe"));
    assert(!is_boss_type("ent.boss"));      // no trailing dot, not a boss
    assert(!is_boss_type(""));

    // boss_display_name: strip the prefix, else unchanged
    assert(boss_display_name("ent.boss.Crabgantua") == "Crabgantua");
    assert(boss_display_name("ent.boss.Mokshi") == "Mokshi");
    assert(boss_display_name("Goblin") == "Goblin");

    // format_run_time: M:SS.cc, rounded centiseconds, negatives clamped
    assert(format_run_time(83.07) == "1:23.07");
    assert(format_run_time(47.31) == "0:47.31");
    assert(format_run_time(0.0)   == "0:00.00");
    assert(format_run_time(-3.0)  == "0:00.00");
    assert(format_run_time(125.5) == "2:05.50");

    // boss_avg_s: mean clear time, divide-by-zero guard
    assert(boss_avg_s(150.0, 3) == 50.0);
    assert(boss_avg_s(10.0, 0) == 0.0);

    // boss_win_pct: kills / (kills + deaths) as a percentage, guarded
    assert(boss_win_pct(3, 1) == 75.0);
    assert(boss_win_pct(5, 0) == 100.0);
    assert(boss_win_pct(0, 0) == 0.0);

    std::printf("boss_names: all tests passed\n");
    return 0;
}
