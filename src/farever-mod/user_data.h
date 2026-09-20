#pragma once

#include <string>
#include <vector>

namespace farever {

// %LOCALAPPDATA%\farever-minimap, created on first call, result cached.
// Precedence: FAREVER_DATA_DIR env override (verbatim) > LocalAppData known
// folder > legacy <game>\data fallback (degrade, never crash).
std::wstring user_data_dir();

// user_data_dir() + L"\\" + relative.
std::wstring user_data_path(const wchar_t* relative);

// One-time copy of the writable files from <game>\data into user_data_dir().
// Idempotent: guarded by a `.migrated` marker file, and each file is copied
// fail-if-exists so a newer destination is never clobbered. Call once at
// worker start, before any module reads its persisted state.
void user_data_migrate();

// --- exposed for testing -----------------------------------------------
// Fixed basenames the mod writes (excludes per-character files).
const std::vector<std::wstring>& writable_data_names();
// Per-character file globs the mod writes.
const std::vector<std::wstring>& writable_data_globs();
// Core copy step: copies the writable names + glob matches from src_dir to
// dst_dir (fail-if-exists). Returns the count of files newly copied.
int user_data_migrate_from(const std::wstring& src_dir,
                           const std::wstring& dst_dir);

}  // namespace farever
