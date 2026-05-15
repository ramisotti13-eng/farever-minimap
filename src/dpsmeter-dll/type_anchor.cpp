// In-process port of tools/find_type_by_name.py — anchor on a Haxe
// class's UTF-16 name string, then chase back through hl_type_obj.name
// and hl_type.obj to the canonical type tag. See type_anchor.h for the
// step-by-step description.
//
// All raw memory access happens through mem_scan.h (which contains the
// SEH-wrapped helpers).

#include "type_anchor.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <cstdint>
#include <numeric>
#include <vector>

namespace dpsmeter {
namespace {

// HashLink hl_type kinds — only HOBJ matters for us.
constexpr std::uint32_t HOBJ = 11;

// Layout offsets we depend on (same as Python prototype).
constexpr std::size_t OFF_TYPE_OBJ = 8;   // hl_type.obj
constexpr std::size_t OFF_OBJ_NAME = 16;  // hl_type_obj.name

void scan_all_u64(const std::vector<Region>& regions, std::uint64_t target,
                  std::vector<std::uintptr_t>& out_hits) {
    ScanBuf buf;
    for (const Region& r : regions) {
        if (!mem_scan_u64(r.base, r.size, target, &buf)) continue;
        for (std::size_t i = 0; i < buf.count; ++i) {
            out_hits.push_back(buf.hits[i]);
        }
    }
}

void scan_all_bytes(const std::vector<Region>& regions,
                    const std::uint8_t* needle, std::size_t needle_len,
                    std::vector<std::uintptr_t>& out_hits) {
    ScanBuf buf;
    for (const Region& r : regions) {
        if (!mem_scan_bytes(r.base, r.size, needle, needle_len, &buf))
            continue;
        for (std::size_t i = 0; i < buf.count; ++i) {
            out_hits.push_back(buf.hits[i]);
        }
    }
}

// Convert a wchar_t* to UTF-16LE byte sequence ending in \0\0.
std::vector<std::uint8_t> utf16_needle(const wchar_t* s) {
    std::vector<std::uint8_t> out;
    while (*s) {
        std::uint16_t c = static_cast<std::uint16_t>(*s++);
        out.push_back(static_cast<std::uint8_t>(c & 0xFF));
        out.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFF));
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

}  // namespace

bool type_anchor_find(const wchar_t* class_name, uintptr_t* out_type_ptr) {
    if (!class_name || !out_type_ptr) return false;
    DWORD t0 = GetTickCount();

    auto regions = mem_collect_regions();
    std::size_t total_bytes = std::accumulate(
        regions.begin(), regions.end(), std::size_t{0},
        [](std::size_t a, const Region& r) { return a + r.size; });
    logf("type_anchor: %zu regions, %.0f MB",
         regions.size(), total_bytes / (1024.0 * 1024.0));

    // Step 1: UTF-16 string search.
    auto needle = utf16_needle(class_name);
    std::vector<std::uintptr_t> name_hits;
    scan_all_bytes(regions, needle.data(), needle.size(), name_hits);
    DWORD t_str = GetTickCount();
    if (name_hits.empty()) {
        logf("type_anchor: '%ls' name string not resident (%lu ms)",
             class_name, t_str - t0);
        return false;
    }
    logf("type_anchor: %zu name string hits for '%ls' (%lu ms)",
         name_hits.size(), class_name, t_str - t0);

    // Step 2 + 3: chase pointer refs to canonical HOBJ.
    for (std::uintptr_t name_addr : name_hits) {
        if (!mem_is_userland(name_addr)) continue;

        std::vector<std::uintptr_t> name_refs;
        scan_all_u64(regions, static_cast<std::uint64_t>(name_addr),
                     name_refs);
        for (std::uintptr_t r : name_refs) {
            if (r <= OFF_OBJ_NAME) continue;
            std::uintptr_t obj_ptr = r - OFF_OBJ_NAME;
            if (!mem_is_userland(obj_ptr)) continue;

            std::vector<std::uintptr_t> obj_refs;
            scan_all_u64(regions, static_cast<std::uint64_t>(obj_ptr),
                         obj_refs);
            for (std::uintptr_t tr : obj_refs) {
                if (tr <= OFF_TYPE_OBJ) continue;
                std::uintptr_t type_ptr = tr - OFF_TYPE_OBJ;
                if (!mem_is_userland(type_ptr)) continue;

                std::uint32_t kind = 0;
                if (!mem_read_u32(type_ptr, &kind)) continue;
                if (kind != HOBJ) continue;

                *out_type_ptr = type_ptr;
                DWORD t_end = GetTickCount();
                logf("type_anchor: '%ls' -> hl_type* = 0x%llx "
                     "(%lu ms total)",
                     class_name,
                     static_cast<unsigned long long>(type_ptr),
                     t_end - t0);
                return true;
            }
        }
    }

    DWORD t_end = GetTickCount();
    logf("type_anchor: '%ls' had %zu name hits but no canonical HOBJ "
         "(%lu ms)",
         class_name, name_hits.size(), t_end - t0);
    return false;
}

}  // namespace dpsmeter
