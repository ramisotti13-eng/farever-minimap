#pragma once

// Pure, header-only waypoint model + JSON parse/serialize. No Windows or
// ImGui dependencies so tools/test_waypoints_json.cpp can compile it
// standalone (same split as anticheat_match.h). The store wrapper
// (waypoints.{h,cpp}) adds file IO + threading on top.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace farever {

struct Waypoint {
    int     id;
    char    name[64];
    float   x, y, z;
    uint8_t color;     // index into kColorNames, 0 = cyan (default)
    uint8_t icon;      // index into kIconNames,  0 = pin  (default)
    uint8_t _pad[2];
};

// Bounds file size + the per-frame render loop.
constexpr std::size_t kMaxWaypoints = 256;

constexpr const char* kColorNames[8] = {
    "cyan", "red", "orange", "yellow", "green", "blue", "magenta", "white"
};
constexpr const char* kIconNames[6] = {
    "pin", "flag", "star", "diamond", "circle", "exclamation"
};

// Returns 0..7 on match, -1 on unknown / nullptr.
inline int color_index(const char* s) {
    if (!s) return -1;
    for (int i = 0; i < 8; ++i)
        if (std::strcmp(s, kColorNames[i]) == 0) return i;
    return -1;
}

// Returns 0..5 on match, -1 on unknown / nullptr. "!" is an alias for
// "exclamation" (matches the Lua API in plugins.cpp).
inline int icon_index(const char* s) {
    if (!s) return -1;
    if (std::strcmp(s, "!") == 0) return 5;
    for (int i = 0; i < 6; ++i)
        if (std::strcmp(s, kIconNames[i]) == 0) return i;
    return -1;
}

namespace wpjson {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end &&
               (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool peek(char c)    { skip_ws(); return p < end && *p == c; }
    bool consume(char c) { skip_ws(); if (peek(c)) { ++p; return true; } return false; }

    bool parse_string(std::string& out) {
        skip_ws();
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                char e = *p++;
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    default:  out.push_back(e);    break;
                }
            } else {
                out.push_back(*p++);
            }
        }
        if (p >= end) return false;   // unterminated string -> structural fail
        ++p;
        return true;
    }

    bool parse_number(double& out) {
        skip_ws();
        char* endp = nullptr;
        out = std::strtod(p, &endp);
        if (endp == p) return false;
        p = endp;
        return true;
    }

    void skip_value() {
        skip_ws();
        if (p >= end) return;
        char c = *p;
        if (c == '"') { std::string s; parse_string(s); return; }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{' ? '}' : ']');
            int depth = 0;
            while (p < end) {
                if (*p == '"') { std::string s; parse_string(s); continue; }
                if (*p == open)  ++depth;
                if (*p == close) { --depth; if (depth == 0) { ++p; return; } }
                ++p;
            }
            return;
        }
        double tmp; parse_number(tmp);
    }
};

inline void copy_into(char* dst, std::size_t n, const std::string& s) {
    std::size_t w = s.size() < n - 1 ? s.size() : n - 1;
    std::memcpy(dst, s.data(), w);
    dst[w] = '\0';
}

}  // namespace wpjson

// Parse the { "waypoints": [ {...} ], "primary": N } document. Any
// structural failure returns whatever complete objects were read so far
// (empty on a bad top-level). Never throws. out_primary (optional) gets
// the primary-waypoint id, 0 if none / missing.
inline std::vector<Waypoint> waypoints_parse(const std::string& text,
                                              int* out_primary = nullptr) {
    std::vector<Waypoint> out;
    if (out_primary) *out_primary = 0;
    wpjson::Parser ps{text.data(), text.data() + text.size()};
    if (!ps.consume('{')) return out;

    // Find the "waypoints" key; also capture "primary" en route.
    bool found = false;
    while (!ps.peek('}')) {
        std::string key;
        if (!ps.parse_string(key)) return out;
        if (!ps.consume(':'))      return out;
        if (key == "waypoints") { found = true; break; }
        if (key == "primary" && out_primary) {
            double v;
            if (ps.parse_number(v)) *out_primary = (int)v;
            else                    ps.skip_value();
        } else {
            ps.skip_value();
        }
        if (!ps.consume(',')) return out;
    }
    if (!found) return out;
    if (!ps.consume('[')) return out;

    while (!ps.peek(']')) {
        if (!ps.consume('{')) break;
        Waypoint w{};
        w.id = 0; w.name[0] = '\0'; w.x = w.y = w.z = 0.0f;
        bool object_ok = true;
        while (!ps.peek('}')) {
            std::string k;
            if (!ps.parse_string(k)) { object_ok = false; break; }
            if (!ps.consume(':'))    { object_ok = false; break; }
            if (k == "id") {
                double v; if (ps.parse_number(v)) w.id = (int)v; else ps.skip_value();
            } else if (k == "name") {
                if (ps.peek('"')) { std::string v; if (ps.parse_string(v)) wpjson::copy_into(w.name, sizeof(w.name), v); else { object_ok = false; break; } }
                else ps.skip_value();
            } else if (k == "x") {
                double v; if (ps.parse_number(v)) w.x = (float)v; else ps.skip_value();
            } else if (k == "y") {
                double v; if (ps.parse_number(v)) w.y = (float)v; else ps.skip_value();
            } else if (k == "z") {
                double v; if (ps.parse_number(v)) w.z = (float)v; else ps.skip_value();
            } else if (k == "color") {
                if (ps.peek('"')) {
                    std::string v;
                    if (ps.parse_string(v)) {
                        int ci = color_index(v.c_str());
                        w.color = (uint8_t)(ci >= 0 ? ci : 0);
                    } else { object_ok = false; break; }
                } else ps.skip_value();
            } else if (k == "icon") {
                if (ps.peek('"')) {
                    std::string v;
                    if (ps.parse_string(v)) {
                        int ii = icon_index(v.c_str());
                        w.icon = (uint8_t)(ii >= 0 ? ii : 0);
                    } else { object_ok = false; break; }
                } else ps.skip_value();
            } else {
                ps.skip_value();
            }
            if (!ps.consume(',')) break;
        }
        if (!object_ok) break;          // truncated/garbage object -> stop, keep earlier
        if (!ps.consume('}')) break;    // no closing brace -> drop this partial object
        out.push_back(w);
        if (out.size() >= kMaxWaypoints) break;
        if (!ps.consume(',')) break;
    }
    return out;
}

inline std::string waypoints_serialize(const std::vector<Waypoint>& v,
                                        int primary = 0) {
    std::string s = "{\n";
    if (primary != 0) {
        char hbuf[64];
        std::snprintf(hbuf, sizeof(hbuf), "  \"primary\": %d,\n", primary);
        s += hbuf;
    }
    s += "  \"waypoints\": [\n";
    for (std::size_t i = 0; i < v.size(); ++i) {
        const Waypoint& w = v[i];
        // Escape " and \ in the name.
        std::string nm;
        for (const char* c = w.name; *c; ++c) {
            if (*c == '"' || *c == '\\') nm.push_back('\\');
            nm.push_back(*c);
        }
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "    { \"id\": %d, \"name\": \"%s\", \"x\": %g, \"y\": %g, \"z\": %g, "
            "\"color\": \"%s\", \"icon\": \"%s\" }%s\n",
            w.id, nm.c_str(),
            (double)w.x, (double)w.y, (double)w.z,
            kColorNames[w.color < 8 ? w.color : 0],
            kIconNames [w.icon  < 6 ? w.icon  : 0],
            (i + 1 < v.size()) ? "," : "");
        s += buf;
    }
    s += "  ]\n}\n";
    return s;
}

}  // namespace farever
