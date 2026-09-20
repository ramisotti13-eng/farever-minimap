// Issue #44 - player codex read. See codex_state.h for the chain and
// the why. The full reverse-engineering trail lives in this file's git
// history (the phase-1 diagnostic builds); this is the production read.

#include "codex_state.h"
#include "type_anchor.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace farever {
namespace {

// hl_type / hl_type_obj.
constexpr std::size_t OFF_TYPE_OBJ = 8;

// data.$CodexData statics-holder offsets (hl.Class-derived).
constexpr std::size_t OFF_CD_NAME      = 32;   // __name__ (String)
constexpr std::size_t OFF_CD_UNITNODES = 88;   // haxe.ds.StringMap

// data.CodexNode field offsets. v018 shifted the value fields +8 (name@8 and
// fullPath@24 are below the shift and unchanged).
constexpr std::size_t OFF_CN_NAME        = 8;   // String (unchanged)
constexpr std::size_t OFF_CN_FULLPATH    = 24;  // String (unchanged)
// _progress@84 is the live current value (climbs toward maxProgress and
// equals it when completed). completionProgress@92 is a constant
// definitional threshold (8 for every monster), NOT the current count -
// reading it was the "20/20 shows as 8/20" bug (issue #44). [v018 +8]
constexpr std::size_t OFF_CN_PROGRESS    = 84;  // _progress (was 76)
constexpr std::size_t OFF_CN_MAXPROGRESS = 96;  // maxProgress (was 88)
constexpr std::size_t OFF_CN_COMPLETED   = 100; // completed (bool, was 92)

// hl.types.ArrayObj: length @8, varray @16, object data @+24, stride 8.
constexpr std::size_t OFF_ARRAYOBJ_LENGTH = 8;
constexpr std::size_t OFF_ARRAYOBJ_VARRAY = 16;
constexpr std::size_t OFF_VARRAY_DATA     = 24;

// haxe.ds.StringMap: h (hl_bytes_map) @8. bytes_map: entries array @24
// (interleaved {vbyte* key; value} stride 16), nentries (i32) @40.
constexpr std::size_t OFF_STRINGMAP_H  = 8;
constexpr std::size_t OFF_BM_ENTRIES   = 24;
constexpr std::size_t OFF_BM_NENTRIES  = 40;
constexpr std::size_t BM_CELL_STRIDE   = 16;
constexpr std::size_t BM_CELL_VALUE    = 8;

// Haxe String: bytes ptr @8, length (UTF-16 code units) @16.
constexpr std::size_t OFF_STR_BYTES = 8;
constexpr std::size_t OFF_STR_LEN   = 16;

constexpr std::int32_t kMaxEntries     = 100000;  // sanity bound
constexpr std::uint64_t kScanEveryTicks = 200;    // ~10 s @ 20 Hz

std::atomic<std::uintptr_t> g_codexdata_type{0};
std::atomic<std::uintptr_t> g_holder{0};   // cached statics-holder base
std::uint64_t               g_tick = 0;    // worker-thread only

std::uintptr_t read_ptr(std::uintptr_t base, std::size_t off) {
    std::uint64_t v = 0;
    if (!mem_read_u64(base + off, &v)) return 0;
    auto p = static_cast<std::uintptr_t>(v);
    return mem_is_userland(p) ? p : 0;
}

// Read a Haxe String (UTF-16) into a UTF-8 buffer.
//
// These are LOCALIZED display names, so on a non-English client they are full
// of non-ASCII characters. This used to squash everything above 0x7F to '?',
// which turned "Zurückgezogener Eber" into "Zur?ckgezogener Eber" for every
// German, French and Spanish player. ImGui renders UTF-8 and its default glyph
// range covers Latin-1, so transcoding properly is all that was needed.
// Surrogate pairs are folded to U+FFFD; the game's names stay in the BMP.
void read_haxe_string(std::uintptr_t str_ptr, char* out, std::size_t cap) {
    if (cap) out[0] = 0;
    if (!str_ptr || !mem_is_userland(str_ptr) || cap == 0) return;
    std::uint64_t bytes_u64 = 0;
    std::int32_t  length    = 0;
    if (!mem_read_u64(str_ptr + OFF_STR_BYTES, &bytes_u64)) return;
    auto bytes_ptr = static_cast<std::uintptr_t>(bytes_u64);
    if (!mem_is_userland(bytes_ptr)) return;
    if (!mem_read_i32(str_ptr + OFF_STR_LEN, &length)) return;
    if (length <= 0 || length > 1024) return;

    std::size_t w = 0;   // write cursor, in BYTES
    for (std::size_t i = 0; i < static_cast<std::size_t>(length); ++i) {
        std::uint8_t lo = 0, hi = 0;
        if (!mem_read_u8(bytes_ptr + i * 2, &lo)) break;
        mem_read_u8(bytes_ptr + i * 2 + 1, &hi);
        std::uint32_t c = static_cast<std::uint32_t>(lo) |
                          (static_cast<std::uint32_t>(hi) << 8);
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;   // lone surrogate

        // Stop cleanly rather than emitting half a sequence.
        std::size_t need = c < 0x80 ? 1 : (c < 0x800 ? 2 : 3);
        if (w + need >= cap) break;

        if (c < 0x80) {
            out[w++] = static_cast<char>(c);
        } else if (c < 0x800) {
            out[w++] = static_cast<char>(0xC0 | (c >> 6));
            out[w++] = static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out[w++] = static_cast<char>(0xE0 | (c >> 12));
            out[w++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out[w++] = static_cast<char>(0x80 | (c & 0x3F));
        }
        out[w] = 0;
    }
}

// Read a null-terminated UTF-16 (vbyte*) key into ASCII. Returns false
// if it isn't clean printable text (so non-key array slots are skipped).
bool read_wchar_key(std::uintptr_t wptr, char* out, std::size_t cap) {
    if (cap) out[0] = 0;
    if (!wptr || !mem_is_userland(wptr) || cap == 0) return false;
    for (std::size_t i = 0; i + 1 < cap; ++i) {
        std::uint8_t lo = 0, hi = 0;
        if (!mem_read_u8(wptr + i * 2, &lo) ||
            !mem_read_u8(wptr + i * 2 + 1, &hi))
            return false;
        std::uint16_t c = static_cast<std::uint16_t>(lo) |
                          (static_cast<std::uint16_t>(hi) << 8);
        if (c == 0) { out[i] = 0; return i > 0; }
        if (c < 0x20 || c > 0x7e) return false;
        out[i] = static_cast<char>(c);
    }
    out[cap - 1] = 0;
    return true;
}

// True if `base` is the data.CodexData statics holder (its __name__
// String contains "CodexData").
bool holder_valid(std::uintptr_t base) {
    if (!base || !mem_is_userland(base)) return false;
    char nm[64];
    read_haxe_string(read_ptr(base, OFF_CD_NAME), nm, sizeof(nm));
    return std::strstr(nm, "CodexData") != nullptr;
}

// Heap-scan for the statics holder. Returns its base, or 0.
std::uintptr_t find_holder() {
    std::uintptr_t T = g_codexdata_type.load(std::memory_order_acquire);
    if (!T) {
        if (!type_anchor_find(L"data.CodexData", &T) || !T) return 0;
        g_codexdata_type.store(T, std::memory_order_release);
        logf("codex: resolved data.CodexData type=0x%llx",
             (unsigned long long)T);
    }
    std::vector<Region> regions = mem_collect_regions();
    static ScanBuf buf;  // 64 KiB; worker-thread only.
    for (const auto& r : regions) {
        if (!mem_scan_u64(r.base, r.size, static_cast<std::uint64_t>(T), &buf))
            continue;
        for (std::size_t i = 0; i < buf.count; ++i) {
            // The holder keeps the type ptr in its __type__ slot (@+8),
            // so the object base is hit-8.
            std::uintptr_t cand = buf.hits[i] - 8;
            if (holder_valid(cand)) return cand;
        }
    }
    return 0;
}

}  // namespace

void codex_state_init(const LibHL& /*libhl*/) {
    logf("codex: player-codex reader armed (issue #44)");
}

void codex_state_tick() {
    std::uintptr_t base = g_holder.load(std::memory_order_acquire);
    if (base) {
        if (holder_valid(base)) return;        // cached + still good
        g_holder.store(0, std::memory_order_release);  // stale (reload?)
    }
    if ((g_tick++ % kScanEveryTicks) != 0) return;
    std::uintptr_t found = find_holder();
    if (found) {
        g_holder.store(found, std::memory_order_release);
        logf("codex: statics holder cached @0x%llx", (unsigned long long)found);
    }
}

bool codex_state_lookup(const char* kind, CodexEntry* out) {
    if (!kind || !*kind || !out) return false;
    std::uintptr_t base = g_holder.load(std::memory_order_acquire);
    if (!base || !holder_valid(base)) return false;

    std::uintptr_t sm = read_ptr(base, OFF_CD_UNITNODES);
    std::uintptr_t h  = sm ? read_ptr(sm, OFF_STRINGMAP_H) : 0;
    std::uintptr_t entries = h ? read_ptr(h, OFF_BM_ENTRIES) : 0;
    std::int32_t   nentries = 0;
    if (h) mem_read_i32(h + OFF_BM_NENTRIES, &nentries);
    if (!entries || nentries <= 0 || nentries > kMaxEntries) return false;

    char key[128];
    for (std::int32_t k = 0; k < nentries; ++k) {
        std::uintptr_t key_ptr = read_ptr(entries, k * BM_CELL_STRIDE);
        if (!key_ptr) continue;
        if (!read_wchar_key(key_ptr, key, sizeof(key))) continue;
        if (std::strcmp(key, kind) != 0) continue;

        // Matched. value is an ArrayObj of CodexNode (len 1 in practice).
        std::uintptr_t val = read_ptr(entries, k * BM_CELL_STRIDE + BM_CELL_VALUE);
        std::int32_t vlen = 0;
        std::uintptr_t varray = 0;
        if (val) {
            mem_read_i32(val + OFF_ARRAYOBJ_LENGTH, &vlen);
            varray = read_ptr(val, OFF_ARRAYOBJ_VARRAY);
        }
        if (!varray || vlen <= 0) return false;
        std::uintptr_t node = read_ptr(varray, OFF_VARRAY_DATA);  // [0]
        if (!node) return false;

        std::int32_t comp = 0, maxp = 0;
        std::uint8_t completed = 0;
        mem_read_i32(node + OFF_CN_PROGRESS,    &comp);
        mem_read_i32(node + OFF_CN_MAXPROGRESS, &maxp);
        mem_read_bytes(node + OFF_CN_COMPLETED, &completed, 1);

        out->completion = comp;
        out->max        = maxp;
        out->completed  = (completed & 1) != 0;
        out->state = out->completed ? "complete"
                                    : (comp > 0 ? "partial" : "unknown");
        read_haxe_string(read_ptr(node, OFF_CN_NAME),     out->name, sizeof(out->name));
        read_haxe_string(read_ptr(node, OFF_CN_FULLPATH), out->path, sizeof(out->path));
        return true;
    }
    return false;  // kind isn't a codex monster
}

// #105: the whole bestiary in one pass. codex_state_lookup already walks the
// entire StringMap comparing keys, so listing costs the same as a single miss;
// the difference is the caller gets everything instead of having to know the
// ids up front. Sorted by kind so the order is stable across calls, which a
// plain map walk is not (rehashing moves entries around).
//
// This is a full map walk, so it is not something to call every frame. A codex
// UI should refresh it on an interval or on a kill event.
std::vector<CodexEntry> codex_state_all() {
    std::vector<CodexEntry> out;
    std::uintptr_t base = g_holder.load(std::memory_order_acquire);
    if (!base || !holder_valid(base)) return out;

    std::uintptr_t sm = read_ptr(base, OFF_CD_UNITNODES);
    std::uintptr_t h  = sm ? read_ptr(sm, OFF_STRINGMAP_H) : 0;
    std::uintptr_t entries = h ? read_ptr(h, OFF_BM_ENTRIES) : 0;
    std::int32_t   nentries = 0;
    if (h) mem_read_i32(h + OFF_BM_NENTRIES, &nentries);
    if (!entries || nentries <= 0 || nentries > kMaxEntries) return out;

    out.reserve(static_cast<std::size_t>(nentries));
    char key[128];
    for (std::int32_t k = 0; k < nentries; ++k) {
        std::uintptr_t key_ptr = read_ptr(entries, k * BM_CELL_STRIDE);
        if (!key_ptr) continue;
        if (!read_wchar_key(key_ptr, key, sizeof(key))) continue;

        std::uintptr_t val = read_ptr(entries, k * BM_CELL_STRIDE + BM_CELL_VALUE);
        std::int32_t vlen = 0;
        std::uintptr_t varray = 0;
        if (val) {
            mem_read_i32(val + OFF_ARRAYOBJ_LENGTH, &vlen);
            varray = read_ptr(val, OFF_ARRAYOBJ_VARRAY);
        }
        if (!varray || vlen <= 0) continue;
        std::uintptr_t node = read_ptr(varray, OFF_VARRAY_DATA);   // [0]
        if (!node) continue;

        CodexEntry e;
        std::int32_t comp = 0, maxp = 0;
        std::uint8_t completed = 0;
        mem_read_i32(node + OFF_CN_PROGRESS,    &comp);
        mem_read_i32(node + OFF_CN_MAXPROGRESS, &maxp);
        mem_read_bytes(node + OFF_CN_COMPLETED, &completed, 1);
        e.completion = comp;
        e.max        = maxp;
        e.completed  = (completed & 1) != 0;
        e.state      = e.completed ? "complete"
                                   : (comp > 0 ? "partial" : "unknown");
        std::strncpy(e.kind, key, sizeof(e.kind) - 1);
        read_haxe_string(read_ptr(node, OFF_CN_NAME),     e.name, sizeof(e.name));
        read_haxe_string(read_ptr(node, OFF_CN_FULLPATH), e.path, sizeof(e.path));
        out.push_back(e);
    }

    std::sort(out.begin(), out.end(),
              [](const CodexEntry& a, const CodexEntry& b) {
                  return std::strcmp(a.kind, b.kind) < 0;
              });
    return out;
}

}  // namespace farever
