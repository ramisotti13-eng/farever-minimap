#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace farever {

// One record from the CDB atlas TSV. `sheet` is the source sheet name
// ("item", "unit", "activity", ...); `id` is the Castle DB record id;
// the rest are localized display strings (may be empty).
struct CdbRecord {
    std::string sheet;
    std::string id;
    std::string name;
    std::string desc;
    std::string flavor;
    // For items only: coarse category ("weapon" / "armor" / "accessory"
    // / "glider" / "mount" / "other") + slot string ("Sword (1H)",
    // "Chest", "Necklace", ...). Populated offline by
    // tools/extract_cdb_xml.py; empty for non-item rows.
    std::string category;
    std::string slot;
};

// Scan ``data\cdb_atlas_*.tsv`` next to the DLL and populate the list
// returned by ``cdb_atlas_available_languages``. Safe to call once at
// module init; subsequent calls are no-ops.
void cdb_atlas_init();

// Load ``data\cdb_atlas_<lang>.tsv``. ``lang`` is one of the codes in
// ``cdb_atlas_available_languages()``; pass "" to use the preferred
// default ("en" if shipped, otherwise the first available). Calls are
// idempotent for the same language and hot-swap the in-memory records
// on a change.
void cdb_atlas_load(const char* lang = "");

// Language codes detected at init, in directory-listing order. Each
// matches a shipped ``cdb_atlas_<code>.tsv``.
const std::vector<std::string>& cdb_atlas_available_languages();

// Currently-loaded language code (e.g. "de"). Empty until a load has
// succeeded.
const std::string& cdb_atlas_current_language();

// All records (read-only view, stable until the next ``cdb_atlas_load``).
const std::vector<CdbRecord>& cdb_atlas_all();

// Records filtered by `sheet`. Returns a fresh vector of pointers into
// cdb_atlas_all(); cheap because the underlying storage doesn't move
// during a render frame.
std::vector<const CdbRecord*> cdb_atlas_by_sheet(const char* sheet);

// Lookup by sheet + id. Returns nullptr if not found.
const CdbRecord* cdb_atlas_find(const char* sheet, const char* id);

// Whether at least one TSV was loaded successfully (records > 0).
bool cdb_atlas_loaded();

}  // namespace farever
