#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace farever {

// Snapshot of an Item's static CDB data, captured at runtime from an
// st.Item allocation. Same record can be referenced by id from the
// Atlas UI to overlay icon + stats on the cdb_atlas catalog.
struct ItemCapture {
    std::string id;             // dedupe key, e.g. "Sword_Boomerang"
    std::string atlas;          // basename, e.g. "atlas_weapon_Sword1H_96PX.png"
    int  gfx_x      = 0;        // cell index inside the atlas
    int  gfx_y      = 0;
    int  gfx_size   = 96;       // cell edge length in px
    int  gfx_width  = 1;        // cell count horizontally
    int  gfx_height = 1;
    int  level      = 0;
    int  ilvl       = 0;
    int  sell_price = 0;
    std::string rarity_id;      // "Common" / "Uncommon" / "Rare" / ...
};

void item_capture_start(const LibHL& libhl);
void item_capture_stop();
void item_capture_tick();

// Lookup by item id. Returns nullptr if we haven't seen it yet.
const ItemCapture* item_capture_find(const char* item_id);

// All captures so far. Stable references; safe to retain.
std::size_t item_capture_count();

// Load captures from data/item_captures.tsv (if present). Idempotent.
void item_capture_load();

}  // namespace farever
