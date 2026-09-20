// Port of src/minimap-dll/pois.cpp; namespace farever, otherwise the
// same JSON-array parser + atlas/style tables. See the minimap-dll
// version for the JSON-shape rationale and atlas-cell mapping.

#include "pois.h"
#include "activity_icons.h"   // #107: learned atlas cells win over the table
#include "log.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace farever {
namespace {

std::vector<PoiRow> g_pois;

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
        if (p < end) ++p;
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
        if (std::isalpha(static_cast<unsigned char>(c))) {
            while (p < end &&
                   std::isalpha(static_cast<unsigned char>(*p))) ++p;
            return;
        }
        double tmp; parse_number(tmp);
    }
};

void copy_into(char* dst, std::size_t n, const std::string& s) {
    std::size_t w = std::min(s.size(), n - 1);
    std::memcpy(dst, s.data(), w);
    dst[w] = '\0';
}

}  // namespace

bool pois_load(const wchar_t* path) {
    g_pois.clear();
    std::ifstream f(path);
    if (!f) { logf("pois: %ls not found", path); return false; }
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    Parser ps{s.data(), s.data() + s.size()};

    if (!ps.consume('[')) {
        logf("pois: expected '[' at start of JSON");
        return false;
    }
    while (!ps.peek(']')) {
        if (!ps.consume('{')) {
            logf("pois: expected '{' inside array");
            return false;
        }
        PoiRow r{};
        r.kind[0] = r.subkind[0] = r.name[0] = r.id[0] = '\0';
        r.x = r.y = r.z = 0.0f;

        while (!ps.peek('}')) {
            std::string key;
            if (!ps.parse_string(key)) break;
            if (!ps.consume(':'))     break;
            if (key == "kind") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.kind, sizeof(r.kind), v); }
                else ps.skip_value();
            } else if (key == "subkind") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.subkind, sizeof(r.subkind), v); }
                else ps.skip_value();
            } else if (key == "name") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.name, sizeof(r.name), v); }
                else ps.skip_value();
            } else if (key == "id") {
                if (ps.peek('"')) { std::string v; ps.parse_string(v); copy_into(r.id, sizeof(r.id), v); }
                else ps.skip_value();
            } else if (key == "x") {
                double v; if (ps.parse_number(v)) r.x = static_cast<float>(v); else ps.skip_value();
            } else if (key == "y") {
                double v; if (ps.parse_number(v)) r.y = static_cast<float>(v); else ps.skip_value();
            } else if (key == "z") {
                double v; if (ps.parse_number(v)) r.z = static_cast<float>(v); else ps.skip_value();
            } else {
                ps.skip_value();
            }
            if (!ps.consume(',')) break;
        }
        if (!ps.consume('}')) {
            logf("pois: expected '}' at end of object");
            return false;
        }
        r.cat = poi_category(r.kind);   // resolve kind -> category once
        g_pois.push_back(r);
        if (!ps.consume(',')) break;
    }
    logf("pois: loaded %zu rows from %ls", g_pois.size(), path);
    return true;
}

const std::vector<PoiRow>& pois_get() { return g_pois; }

struct AtlasIdx { int col; int row; };

AtlasIdx atlas_index_for(const PoiRow& p) {
    auto eq = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    if (eq(p.kind, "obelisk"))  return { 4, 0 };
    if (eq(p.kind, "respawn"))  return { 5, 0 };
    if (eq(p.kind, "dungeon"))  return { 2, 0 };
    if (eq(p.kind, "merchant")) return { 7, 0 };
    if (eq(p.kind, "activity")) {
        // #107: the game's own answer, learned from live st.Activity records,
        // wins over the table below. That table was hand-assembled by matching
        // icons to names and got several wrong (FightStone drew the treasure
        // chest, ChestOrb the wagon); it now only covers subkinds we have not
        // seen live yet, and it stops mattering after a few minutes of play.
        int col = -1, row = -1;
        if (activity_icon_cell(p.subkind, &col, &row)) return { col, row };
        if (eq(p.subkind, "WorldElite"))      return { 0, 0 };
        if (eq(p.subkind, "FightStone"))      return { 2, 1 };
        if (eq(p.subkind, "ChestOrb"))        return { 1, 0 };
        if (eq(p.subkind, "Ascension"))       return { 1, 1 };
        if (eq(p.subkind, "MountRush"))       return { 0, 1 };
        if (eq(p.subkind, "TimerCollectRun")) return { 0, 1 };
        if (eq(p.subkind, "WorldCamp"))       return { 7, 0 };
        if (eq(p.subkind, "WorldPlant"))      return { 7, 0 };
    }
    return { -1, -1 };
}

// data/icons/activities.png is 1024x384: an 8x3 grid of 128 px cells.
constexpr int   kAtlasCols = 8;
constexpr int   kAtlasRows = 3;
constexpr float kAtlasCell = 128.0f;

bool pois_atlas_uv(const PoiRow& p, ImVec2& uv0, ImVec2& uv1) {
    AtlasIdx ix = atlas_index_for(p);
    if (ix.col < 0) return false;
    // #107: a learned cell comes from game data, so treat it as untrusted
    // input - an out-of-grid index would sample the wrong part of the atlas
    // (or past it). Falling through to the shape marker is the safe answer.
    if (ix.col >= kAtlasCols || ix.row < 0 || ix.row >= kAtlasRows) return false;
    const float cw = kAtlasCell / (kAtlasCols * kAtlasCell);
    const float ch = kAtlasCell / (kAtlasRows * kAtlasCell);
    uv0 = ImVec2(ix.col * cw, ix.row * ch);
    uv1 = ImVec2(uv0.x + cw,  uv0.y + ch);
    return true;
}

void pois_draw_atlas(ImDrawList* dl, ImTextureID tex, ImVec2 pos,
                     const ImVec2& uv0, const ImVec2& uv1, float size) {
    ImVec2 a(pos.x - size, pos.y - size);
    ImVec2 b(pos.x + size, pos.y + size);
    dl->AddImage(tex, a, b, uv0, uv1);
}

PoiCat poi_category(const char* kind) {
    auto eq = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    if (eq(kind, "obelisk"))  return PoiCat::Obelisk;
    if (eq(kind, "respawn"))  return PoiCat::Respawn;
    if (eq(kind, "dungeon"))  return PoiCat::Dungeon;
    if (eq(kind, "merchant")) return PoiCat::Merchant;
    if (eq(kind, "activity")) return PoiCat::Activity;
    if (eq(kind, "chest"))    return PoiCat::Chest;
    if (eq(kind, "red_orb"))  return PoiCat::RedOrb;
    if (eq(kind, "plant"))    return PoiCat::Plant;
    if (eq(kind, "ore"))      return PoiCat::Ore;
    return PoiCat::Other;
}

PoiStyle pois_style(const PoiRow& p) {
    switch (p.cat) {
        case PoiCat::Obelisk:  return {IM_COL32(120, 220, 255, 255), 3};
        case PoiCat::Respawn:  return {IM_COL32(120, 220, 120, 255), 4};
        case PoiCat::Merchant: return {IM_COL32(255, 200,  80, 255), 0};
        case PoiCat::RedOrb:   return {IM_COL32(255,  60,  60, 255), 0};  // circle
        case PoiCat::Chest:    return {IM_COL32(255, 200,  60, 255), 1};  // square (gold)
        case PoiCat::Plant:    return {IM_COL32( 90, 200,  90, 255), 3};  // diamond (green)
        case PoiCat::Ore:      return {IM_COL32(180, 140,  80, 255), 2};  // triangle (brown)
        default: break;   // Activity (subkind below), Dungeon/Other -> gray
    }
    auto eq = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    if (p.cat == PoiCat::Activity) {
        if (eq(p.subkind, "WorldElite"))      return {IM_COL32(255,  80,  80, 255), 2};
        if (eq(p.subkind, "FightStone"))      return {IM_COL32(220, 100, 100, 255), 0};
        if (eq(p.subkind, "ChestOrb"))        return {IM_COL32(255, 220, 100, 255), 1};
        if (eq(p.subkind, "WorldCamp"))       return {IM_COL32(200, 100,  50, 255), 0};
        if (eq(p.subkind, "WorldPlant"))      return {IM_COL32( 80, 200,  80, 255), 0};
        if (eq(p.subkind, "Ascension"))       return {IM_COL32(220, 120, 255, 255), 3};
        if (eq(p.subkind, "MountRush"))       return {IM_COL32(180, 140,  80, 255), 0};
        if (eq(p.subkind, "TimerCollectRun")) return {IM_COL32(200, 200,  80, 255), 0};
    }
    return {IM_COL32(180, 180, 180, 255), 0};
}

void pois_draw_marker(ImDrawList* dl, ImVec2 pos, PoiStyle st, float size) {
    constexpr ImU32 outline = IM_COL32(0, 0, 0, 230);
    switch (st.shape) {
        case 1: {
            ImVec2 a(pos.x - size, pos.y - size);
            ImVec2 b(pos.x + size, pos.y + size);
            dl->AddRectFilled(a, b, st.color);
            dl->AddRect(a, b, outline, 0.0f, 0, 1.0f);
            break;
        }
        case 2: {
            ImVec2 a(pos.x,                pos.y - size);
            ImVec2 b(pos.x - size * 0.9f,  pos.y + size * 0.7f);
            ImVec2 c(pos.x + size * 0.9f,  pos.y + size * 0.7f);
            dl->AddTriangleFilled(a, b, c, st.color);
            dl->AddTriangle(a, b, c, outline, 1.0f);
            break;
        }
        case 3: {
            ImVec2 a(pos.x,        pos.y - size);
            ImVec2 b(pos.x + size, pos.y);
            ImVec2 c(pos.x,        pos.y + size);
            ImVec2 d(pos.x - size, pos.y);
            dl->AddQuadFilled(a, b, c, d, st.color);
            dl->AddQuad(a, b, c, d, outline, 1.0f);
            break;
        }
        case 4: {
            float t = 2.0f;
            dl->AddLine(ImVec2(pos.x - size, pos.y),
                        ImVec2(pos.x + size, pos.y), st.color, t);
            dl->AddLine(ImVec2(pos.x, pos.y - size),
                        ImVec2(pos.x, pos.y + size), st.color, t);
            dl->AddLine(ImVec2(pos.x - size, pos.y),
                        ImVec2(pos.x + size, pos.y), outline, 0.5f);
            dl->AddLine(ImVec2(pos.x, pos.y - size),
                        ImVec2(pos.x, pos.y + size), outline, 0.5f);
            break;
        }
        case 0:
        default: {
            dl->AddCircleFilled(pos, size, st.color, 12);
            dl->AddCircle(pos, size, outline, 12, 1.0f);
            break;
        }
    }
}

// Per-kind collectible glyphs. Sized so `size` is roughly the icon's
// outer radius - same convention as pois_draw_marker.
namespace {

ImU32 fade_to_outline(ImU32 fill) {
    // Reuse the fill's alpha for the outline so dimmed (done) POIs
    // still look coherent.
    std::uint32_t a = (fill >> 24) & 0xff;
    return IM_COL32(0, 0, 0, a > 230 ? 200 : a);
}

void draw_chest_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 fill) {
    ImU32 outline = fade_to_outline(fill);
    // Body: wide bottom, slightly taller lid on top
    float w = s * 1.1f, hUp = s * 0.55f, hDn = s * 0.40f;
    ImVec2 tl(c.x - w, c.y - hUp);
    ImVec2 br(c.x + w, c.y + hDn);
    dl->AddRectFilled(tl, br, fill, 0.5f);
    dl->AddRect    (tl, br, outline, 0.5f, 0, 1.0f);
    // Lid seam
    dl->AddLine({tl.x, c.y - hUp * 0.1f},
                {br.x, c.y - hUp * 0.1f}, outline, 1.0f);
    // Lock plate (darker than fill)
    std::uint32_t aA = (fill >> 24) & 0xff;
    ImU32 lock = IM_COL32(40, 28, 12, aA);
    dl->AddRectFilled({c.x - 1.6f, c.y - 1.6f},
                      {c.x + 1.6f, c.y + 1.8f}, lock);
}

void draw_orb_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 fill) {
    ImU32 outline = fade_to_outline(fill);
    dl->AddCircleFilled(c, s, fill, 18);
    // Highlight (upper-left dot)
    std::uint32_t a = (fill >> 24) & 0xff;
    ImU32 hi = IM_COL32(255, 220, 220, (a * 200) / 255);
    dl->AddCircleFilled({c.x - s * 0.35f, c.y - s * 0.35f},
                        s * 0.30f, hi, 10);
    dl->AddCircle(c, s, outline, 18, 1.0f);
}

void draw_plant_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 fill) {
    ImU32 outline = fade_to_outline(fill);
    // Two side leaves + a small center sprout
    dl->AddQuadFilled(
        {c.x,              c.y + s * 0.25f},
        {c.x - s * 0.9f,   c.y - s * 0.1f},
        {c.x - s * 1.1f,   c.y + s * 0.4f},
        {c.x - s * 0.35f,  c.y + s * 0.55f},
        fill);
    dl->AddQuadFilled(
        {c.x,              c.y + s * 0.25f},
        {c.x + s * 0.9f,   c.y - s * 0.1f},
        {c.x + s * 1.1f,   c.y + s * 0.4f},
        {c.x + s * 0.35f,  c.y + s * 0.55f},
        fill);
    dl->AddTriangleFilled(
        {c.x,             c.y - s * 1.0f},
        {c.x - s * 0.45f, c.y + s * 0.1f},
        {c.x + s * 0.45f, c.y + s * 0.1f},
        fill);
    // Tiny stem
    dl->AddLine({c.x, c.y + s * 0.55f},
                {c.x, c.y + s * 1.0f}, outline, 1.5f);
}

void draw_ore_glyph(ImDrawList* dl, ImVec2 c, float s, ImU32 fill) {
    ImU32 outline = fade_to_outline(fill);
    auto diamond = [&](ImVec2 ctr, float r) {
        ImVec2 a(ctr.x,         ctr.y - r);
        ImVec2 b(ctr.x + r,     ctr.y);
        ImVec2 d(ctr.x,         ctr.y + r);
        ImVec2 e(ctr.x - r,     ctr.y);
        dl->AddQuadFilled(a, b, d, e, fill);
        dl->AddQuad      (a, b, d, e, outline, 1.0f);
    };
    diamond({c.x - s * 0.45f, c.y + s * 0.25f}, s * 0.55f);
    diamond({c.x + s * 0.45f, c.y + s * 0.25f}, s * 0.50f);
    diamond({c.x,             c.y - s * 0.30f}, s * 0.60f);
}

}  // anonymous

bool pois_draw_collectible(ImDrawList* dl, ImVec2 center, float size,
                           const char* kind, ImU32 fill) {
    bool is_collectible =
        std::strcmp(kind, "chest")   == 0 ||
        std::strcmp(kind, "red_orb") == 0 ||
        std::strcmp(kind, "plant")   == 0 ||
        std::strcmp(kind, "ore")     == 0;
    if (!is_collectible) return false;

    // Backing disc + ring - gives every collectible icon a readable
    // contrast against the terrain (plants on grass are the worst
    // offender without this). Alpha tracks the fill so dimmed
    // ("done") icons fade their background to match.
    std::uint32_t fa  = (fill >> 24) & 0xff;
    std::uint32_t bga = (fa * 175) / 255;
    std::uint32_t rga = (fa * 230) / 255;
    ImU32 bg   = IM_COL32(18, 14, 8,    bga);
    ImU32 ring = IM_COL32(255, 220, 150, rga);
    float r = size * 1.35f;
    dl->AddCircleFilled(center, r,        bg,   18);
    dl->AddCircle      (center, r + 0.5f, ring, 18, 1.2f);

    if (std::strcmp(kind, "chest")   == 0) draw_chest_glyph(dl, center, size, fill);
    if (std::strcmp(kind, "red_orb") == 0) draw_orb_glyph  (dl, center, size, fill);
    if (std::strcmp(kind, "plant")   == 0) draw_plant_glyph(dl, center, size, fill);
    if (std::strcmp(kind, "ore")     == 0) draw_ore_glyph  (dl, center, size, fill);
    return true;
}

}  // namespace farever
