// Standalone unit test for the pure render-mode parse helpers.
// Build (MSVC, from repo root):
//   cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_render_mode.cpp /Fe:test_render_mode.exe'
//   .\test_render_mode.exe
#include "render_mode_parse.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace farever;

int main() {
    // Exact tokens.
    assert(parse_render_mode("game_swapchain").has_value());
    assert(*parse_render_mode("game_swapchain") == true);
    assert(*parse_render_mode("dcomp") == false);

    // Surrounding whitespace / newline (the file is written with a trailing \n).
    assert(*parse_render_mode("game_swapchain\n") == true);
    assert(*parse_render_mode("  dcomp  ") == false);
    assert(*parse_render_mode("\r\n game_swapchain \r\n") == true);

    // Unrecognized / empty -> no value (treated as "no choice").
    assert(!parse_render_mode("").has_value());
    assert(!parse_render_mode("   ").has_value());
    assert(!parse_render_mode("garbage").has_value());
    assert(!parse_render_mode("GAME_SWAPCHAIN").has_value());   // case-sensitive
    assert(!parse_render_mode("dcomp extra").has_value());      // trailing junk

    // Token round-trip.
    assert(std::strcmp(render_mode_token(true),  "game_swapchain") == 0);
    assert(std::strcmp(render_mode_token(false), "dcomp") == 0);
    assert(*parse_render_mode(render_mode_token(true))  == true);
    assert(*parse_render_mode(render_mode_token(false)) == false);

    std::printf("render_mode_parse: all tests passed\n");
    return 0;
}
