// Standalone unit test for the pure compass-heading selector (#65).
// Build (MSVC, from the repo root, in a VS developer prompt):
//   cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_compass_heading.cpp /Fe:test_compass_heading.exe && test_compass_heading.exe'
#include "compass_heading.h"
#include <cassert>
#include <cstdio>

using namespace farever;

int main() {
    // follow_camera on + camera valid -> camera yaw.
    assert(compass_heading_ref(true, true, 1.25, 0.5) == 1.25f);
    // follow_camera on but no valid camera -> hero facing (fallback).
    assert(compass_heading_ref(true, false, 1.25, 0.5) == 0.5f);
    // follow_camera off -> hero facing even when a camera is valid.
    assert(compass_heading_ref(false, true, 1.25, 0.5) == 0.5f);
    // follow_camera off + no camera -> hero facing.
    assert(compass_heading_ref(false, false, 9.0, -2.0) == -2.0f);
    // unbounded camera yaw is passed through verbatim (strip's project()
    // normalises it later) -> no clamping here.
    assert(compass_heading_ref(true, true, 9.2170, 0.0) == 9.2170f);

    std::printf("compass_heading: all tests passed\n");
    return 0;
}
