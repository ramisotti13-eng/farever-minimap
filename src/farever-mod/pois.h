#pragma once

#include <cstdint>
#include <vector>

#include "imgui.h"

namespace farever {

// Filter / style category, precomputed once from `kind` at load so the
// per-frame render loop never strcmps the kind string.
enum class PoiCat : std::uint8_t {
    Obelisk, Respawn, Dungeon, Merchant, Activity,
    Chest, RedOrb, Plant, Ore, Other
};
PoiCat poi_category(const char* kind);

// One POI row loaded from data/pois_<world>.json. Fixed-size strings
// so we don't allocate per-POI on every render.
struct PoiRow {
    char   kind[24];
    char   subkind[24];
    char   name[64];
    char   id[80];     // unique identifier from the prefab tree
    float  x;
    float  y;
    float  z;
    PoiCat cat = PoiCat::Other;   // derived from kind at load
};

bool pois_load(const wchar_t* path);
const std::vector<PoiRow>& pois_get();

struct PoiStyle {
    ImU32 color;
    int   shape;  // 0=circle 1=square 2=triangle 3=diamond 4=cross
};

PoiStyle pois_style(const PoiRow& p);
void pois_draw_marker(ImDrawList* dl, ImVec2 pos, PoiStyle style,
                      float size);

// Returns true and fills uv0/uv1 in [0,1] with the icon's region
// inside the activities atlas. False = no icon mapped → caller falls
// back to pois_draw_marker.
bool pois_atlas_uv(const PoiRow& p, ImVec2& uv0, ImVec2& uv1);

void pois_draw_atlas(ImDrawList* dl, ImTextureID atlas_srv, ImVec2 pos,
                     const ImVec2& uv0, const ImVec2& uv1, float size);

// Per-collectible-kind glyph drawers. Used for kinds the game's POI
// atlas doesn't cover (chest / red_orb / plant / ore). `fill` carries
// both color and alpha - the caller pre-dims it for "done" POIs.
// Returns true if a glyph was drawn, false if the kind isn't one of
// the collectible categories (so the caller can fall back to
// pois_draw_marker).
bool pois_draw_collectible(ImDrawList* dl, ImVec2 center, float size,
                           const char* kind, ImU32 fill);

}  // namespace farever
