// Standalone unit test for the pure anticheat matching helpers.
// Build (from repo root):
//   g++ -std=c++17 -I src/farever-mod tools/test_anticheat_match.cpp -o test_ac
//   ./test_ac
// (MSVC fallback, from a VS dev prompt:
//   cl /EHsc /std:c++17 /I src\farever-mod tools\test_anticheat_match.cpp /Fe:test_ac.exe)
#include "anticheat_match.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace farever;

    // Signature matching is case-insensitive substring.
    assert(ac_name_hits("easyanticheat_x64.dll"));
    assert(ac_name_hits(ac_to_lower("BEClient_x64.dll")));
    assert(ac_name_hits("vgk.sys"));
    assert(ac_name_hits(ac_to_lower("GameMon64.des")));
    assert(!ac_name_hits("farever.exe"));
    assert(!ac_name_hits("dinput8.dll"));
    assert(!ac_name_hits("libhl.dll"));

    // Allowlist: the hash must appear somewhere in the file text, lowercase.
    const std::string file =
        "[ { \"hash\": \"0B86107B\", \"label\": \"game v0.1.5\" } ]";
    const std::string lower = ac_to_lower(file);
    assert(ac_allowlist_has(lower, "0b86107b"));
    assert(!ac_allowlist_has(lower, "deadbeef"));
    assert(!ac_allowlist_has(lower, ""));   // empty hash never matches

    std::printf("anticheat_match: all tests passed\n");
    return 0;
}
