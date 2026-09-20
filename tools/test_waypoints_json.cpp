// Standalone unit test for the pure waypoint JSON helpers.
// Build (MSVC, from repo root):
//   cmd /c '"...\vcvars64.bat" >nul && cl /EHsc /std:c++17 /I src\farever-mod tools\test_waypoints_json.cpp /Fe:test_wp.exe'
//   .\test_wp.exe
#include "waypoints_json.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace farever;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

int main() {
    // Round-trip: ids, a name with a space, signed/decimal floats.
    std::vector<Waypoint> in;
    in.push_back(Waypoint{1, "Farm spot", 123.5f, -42.0f, 8.0f});
    in.push_back(Waypoint{2, "Boss",      -1.25f, 0.0f, 0.0f});

    std::string text = waypoints_serialize(in);
    std::vector<Waypoint> out = waypoints_parse(text);

    assert(out.size() == 2);
    assert(out[0].id == 1);
    assert(std::strcmp(out[0].name, "Farm spot") == 0);
    assert(near(out[0].x, 123.5f) && near(out[0].y, -42.0f) && near(out[0].z, 8.0f));
    assert(out[1].id == 2);
    assert(std::strcmp(out[1].name, "Boss") == 0);
    assert(near(out[1].x, -1.25f));

    // Malformed -> empty, no crash.
    assert(waypoints_parse("").empty());
    assert(waypoints_parse("garbage").empty());
    assert(waypoints_parse("{ \"nope\": 1 }").empty());

    // Truncated final object is dropped, earlier complete ones survive.
    std::string trunc =
        "{ \"waypoints\": [ { \"id\": 7, \"name\": \"ok\", \"x\": 1, \"y\": 2, \"z\": 3 },"
        " { \"id\": 8, \"name\": \"part";
    std::vector<Waypoint> tw = waypoints_parse(trunc);
    assert(tw.size() == 1 && tw[0].id == 7);

    // Name longer than 63 chars is truncated to 63 + NUL.
    std::string longname(70, 'a');
    std::vector<Waypoint> one;
    one.push_back(Waypoint{1, "", 0, 0, 0});
    std::strncpy(one[0].name, longname.c_str(), sizeof(one[0].name) - 1);
    one[0].name[sizeof(one[0].name) - 1] = '\0';
    std::string lt = waypoints_serialize(one);
    std::vector<Waypoint> lp = waypoints_parse(lt);
    assert(lp.size() == 1);
    assert(std::strlen(lp[0].name) == 63);

    // Round-trip with style.
    std::vector<Waypoint> styled;
    styled.push_back(Waypoint{10, "Red flag",   1.0f, 2.0f, 3.0f, 1, 1, {0,0}});
    styled.push_back(Waypoint{11, "Orange dia", 4.0f, 5.0f, 6.0f, 2, 3, {0,0}});
    styled.push_back(Waypoint{12, "Default",    7.0f, 8.0f, 9.0f, 0, 0, {0,0}});
    std::string styled_text = waypoints_serialize(styled);
    std::vector<Waypoint> styled_out = waypoints_parse(styled_text);
    assert(styled_out.size() == 3);
    assert(styled_out[0].color == 1 && styled_out[0].icon == 1);
    assert(styled_out[1].color == 2 && styled_out[1].icon == 3);
    assert(styled_out[2].color == 0 && styled_out[2].icon == 0);

    // Backward-compat: a beta4-era file (no color/icon keys) loads as 0/0.
    std::string beta4 =
        "{ \"waypoints\": [ "
        "{ \"id\": 1, \"name\": \"old\", \"x\": 1, \"y\": 2, \"z\": 3 },"
        "{ \"id\": 2, \"name\": \"older\", \"x\": 4, \"y\": 5, \"z\": 6 } ] }";
    std::vector<Waypoint> b4 = waypoints_parse(beta4);
    assert(b4.size() == 2);
    assert(b4[0].color == 0 && b4[0].icon == 0);
    assert(b4[1].color == 0 && b4[1].icon == 0);
    assert(std::strcmp(b4[0].name, "old") == 0);

    // Unknown string values fall back to 0, neighbors still parse correctly.
    std::string bad =
        "{ \"waypoints\": [ "
        "{ \"id\": 1, \"name\": \"a\", \"x\": 0, \"y\": 0, \"z\": 0,"
        "  \"color\": \"fuchsia\", \"icon\": \"trapezoid\" },"
        "{ \"id\": 2, \"name\": \"b\", \"x\": 0, \"y\": 0, \"z\": 0,"
        "  \"color\": \"red\", \"icon\": \"flag\" } ] }";
    std::vector<Waypoint> bp = waypoints_parse(bad);
    assert(bp.size() == 2);
    assert(bp[0].color == 0 && bp[0].icon == 0);
    assert(bp[1].color == 1 && bp[1].icon == 1);

    // Name-index helpers: defaults map to 0 (cyan / pin = current behavior).
    assert(color_index("cyan")    == 0);
    assert(color_index("red")     == 1);
    assert(color_index("orange")  == 2);
    assert(color_index("yellow")  == 3);
    assert(color_index("green")   == 4);
    assert(color_index("blue")    == 5);
    assert(color_index("magenta") == 6);
    assert(color_index("white")   == 7);
    assert(color_index("fuchsia") == -1);
    assert(color_index(nullptr)   == -1);

    assert(icon_index("pin")         == 0);
    assert(icon_index("flag")        == 1);
    assert(icon_index("star")        == 2);
    assert(icon_index("diamond")     == 3);
    assert(icon_index("circle")      == 4);
    assert(icon_index("exclamation") == 5);
    assert(icon_index("!")           == 5);   // alias
    assert(icon_index("trapezoid")   == -1);
    assert(icon_index(nullptr)       == -1);

    std::printf("waypoints_json: all tests passed\n");
    return 0;
}
