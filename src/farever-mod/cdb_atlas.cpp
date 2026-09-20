// Loads the static CDB catalog (items, units, activities, ...) from
// one of the per-language ``data\cdb_atlas_<lang>.tsv`` files produced
// offline by tools/extract_cdb_xml.py. Each TSV is one Castle DB
// localization (de, es, fr, ja, ko, pl, pt-BR, ru, zh) plus a
// synthetic "en" fallback derived from CDB IDs.
//
// One load at a time. The Atlas UI's language picker calls
// ``cdb_atlas_load(code)`` to swap; the records vector is replaced
// atomically under a mutex so the next render frame sees the new
// language end-to-end.

#include "cdb_atlas.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace farever {
namespace {

std::vector<CdbRecord>   g_records;
std::vector<std::string> g_languages;
std::string              g_current_lang;
std::mutex               g_mu;

constexpr int kTsvCols = 7;

std::wstring my_dll_dir() {
    HMODULE hmod = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&my_dll_dir),
        &hmod);
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(hmod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(buf);
    auto pos = s.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L".";
    s.resize(pos);
    return s;
}

void split_tabs(const std::string& line, std::string out[kTsvCols]) {
    std::size_t start = 0;
    int field = 0;
    for (std::size_t i = 0; i <= line.size() && field < kTsvCols; ++i) {
        if (i == line.size() || line[i] == '\t') {
            out[field++] = line.substr(start, i - start);
            start = i + 1;
        }
    }
    while (field < kTsvCols) out[field++] = std::string();
}

// Convert "cdb_atlas_pt-BR.tsv" into the code "pt-BR". Returns empty
// for the legacy ``cdb_atlas.tsv`` filename, since we no longer
// treat that as a language entry.
std::string lang_from_filename(const std::wstring& name) {
    const std::wstring prefix = L"cdb_atlas_";
    const std::wstring suffix = L".tsv";
    if (name.size() <= prefix.size() + suffix.size()) return {};
    if (name.compare(0, prefix.size(), prefix) != 0) return {};
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return {};
    std::wstring code = name.substr(prefix.size(),
                                    name.size() - prefix.size() - suffix.size());
    std::string out;
    out.reserve(code.size());
    for (wchar_t c : code) {
        if (c < 128) out.push_back(static_cast<char>(c));
    }
    return out;
}

void scan_languages_locked() {
    g_languages.clear();
    std::wstring pattern = my_dll_dir() + L"\\data\\cdb_atlas_*.tsv";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string lang = lang_from_filename(fd.cFileName);
        if (!lang.empty()) g_languages.push_back(std::move(lang));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_languages.begin(), g_languages.end());
}

std::wstring tsv_path(const std::string& lang) {
    std::wstring p = my_dll_dir() + L"\\data\\cdb_atlas_";
    for (char c : lang) p.push_back(static_cast<wchar_t>(c));
    p += L".tsv";
    return p;
}

bool load_records_from(const std::string& lang,
                       std::vector<CdbRecord>* out) {
    std::wstring path = tsv_path(lang);
    std::ifstream f(path);
    if (!f) {
        logf("cdb_atlas: %ls not found", path.c_str());
        return false;
    }
    out->clear();
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) { first = false; continue; }
        std::string fields[kTsvCols];
        split_tabs(line, fields);
        if (fields[0].empty() || fields[1].empty()) continue;
        CdbRecord rec;
        rec.sheet    = std::move(fields[0]);
        rec.id       = std::move(fields[1]);
        rec.name     = std::move(fields[2]);
        rec.desc     = std::move(fields[3]);
        rec.flavor   = std::move(fields[4]);
        rec.category = std::move(fields[5]);
        rec.slot     = std::move(fields[6]);
        out->push_back(std::move(rec));
    }
    logf("cdb_atlas: loaded %zu records (lang=%s)",
         out->size(), lang.c_str());
    return !out->empty();
}

std::string pick_default_locked() {
    if (g_languages.empty()) return {};
    auto it = std::find(g_languages.begin(), g_languages.end(),
                        std::string("en"));
    if (it != g_languages.end()) return *it;
    return g_languages.front();
}

}  // namespace

void cdb_atlas_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_languages.empty()) return;
    scan_languages_locked();
    logf("cdb_atlas: detected %zu language(s)", g_languages.size());
}

void cdb_atlas_load(const char* lang) {
    std::string requested = (lang && *lang) ? std::string(lang) : std::string();

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_languages.empty()) scan_languages_locked();
    if (requested.empty())   requested = pick_default_locked();
    if (requested.empty())   { logf("cdb_atlas: no language tsv found"); return; }
    if (requested == g_current_lang && !g_records.empty()) return;

    std::vector<CdbRecord> fresh;
    if (load_records_from(requested, &fresh)) {
        g_records      = std::move(fresh);
        g_current_lang = std::move(requested);
        return;
    }
    // Last-resort fallback so a missing requested file doesn't leave
    // the atlas empty after init.
    if (g_records.empty()) {
        for (const std::string& alt : g_languages) {
            if (alt == requested) continue;
            if (load_records_from(alt, &fresh)) {
                g_records      = std::move(fresh);
                g_current_lang = alt;
                return;
            }
        }
    }
}

const std::vector<std::string>& cdb_atlas_available_languages() {
    return g_languages;
}

const std::string& cdb_atlas_current_language() {
    return g_current_lang;
}

const std::vector<CdbRecord>& cdb_atlas_all() {
    return g_records;
}

std::vector<const CdbRecord*> cdb_atlas_by_sheet(const char* sheet) {
    std::vector<const CdbRecord*> out;
    if (!sheet) return out;
    out.reserve(64);
    for (const CdbRecord& r : g_records) {
        if (r.sheet == sheet) out.push_back(&r);
    }
    return out;
}

const CdbRecord* cdb_atlas_find(const char* sheet, const char* id) {
    if (!sheet || !id) return nullptr;
    for (const CdbRecord& r : g_records) {
        if (r.sheet == sheet && r.id == id) return &r;
    }
    return nullptr;
}

bool cdb_atlas_loaded() { return !g_records.empty(); }

}  // namespace farever
