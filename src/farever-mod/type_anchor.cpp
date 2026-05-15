// Port of src/dpsmeter-dll/type_anchor.cpp — same algorithm, namespace
// `farever`. UTF-16 class-name search, then chase hl_type_obj.name and
// hl_type.obj back to the canonical HOBJ tag. Multi-worker scan inside.

#include "type_anchor.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace farever {
namespace {

constexpr std::uint32_t HOBJ = 11;
constexpr std::size_t OFF_TYPE_OBJ = 8;
constexpr std::size_t OFF_OBJ_NAME = 16;

unsigned int pick_worker_count() {
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw <= 2) return 1u;
    if (hw <= 4) return 2u;
    if (hw <= 8) return 3u;
    return 4u;
}

void parallel_scan_u64(const std::vector<Region>& regions,
                       std::uint64_t target,
                       std::vector<std::uintptr_t>& out_hits) {
    std::atomic<std::size_t> next_idx{0};
    std::mutex out_mu;
    const unsigned int n_workers = pick_worker_count();

    auto worker = [&]() {
        ScanBuf buf;
        std::vector<std::uintptr_t> local;
        local.reserve(64);
        while (true) {
            std::size_t idx =
                next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= regions.size()) break;
            const Region& r = regions[idx];
            if (!mem_scan_u64(r.base, r.size, target, &buf)) continue;
            for (std::size_t i = 0; i < buf.count; ++i) {
                local.push_back(buf.hits[i]);
            }
        }
        if (!local.empty()) {
            std::lock_guard<std::mutex> lk(out_mu);
            out_hits.insert(out_hits.end(), local.begin(), local.end());
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (unsigned int w = 0; w < n_workers; ++w) workers.emplace_back(worker);
    for (auto& t : workers) t.join();
}

void parallel_scan_bytes(const std::vector<Region>& regions,
                         const std::uint8_t* needle, std::size_t needle_len,
                         std::vector<std::uintptr_t>& out_hits) {
    std::atomic<std::size_t> next_idx{0};
    std::mutex out_mu;
    const unsigned int n_workers = pick_worker_count();

    auto worker = [&]() {
        ScanBuf buf;
        std::vector<std::uintptr_t> local;
        local.reserve(8);
        while (true) {
            std::size_t idx =
                next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= regions.size()) break;
            const Region& r = regions[idx];
            if (!mem_scan_bytes(r.base, r.size, needle, needle_len, &buf))
                continue;
            for (std::size_t i = 0; i < buf.count; ++i) {
                local.push_back(buf.hits[i]);
            }
        }
        if (!local.empty()) {
            std::lock_guard<std::mutex> lk(out_mu);
            out_hits.insert(out_hits.end(), local.begin(), local.end());
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (unsigned int w = 0; w < n_workers; ++w) workers.emplace_back(worker);
    for (auto& t : workers) t.join();
}

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

    auto needle = utf16_needle(class_name);
    std::vector<std::uintptr_t> name_hits;
    parallel_scan_bytes(regions, needle.data(), needle.size(), name_hits);
    DWORD t_str = GetTickCount();
    if (name_hits.empty()) {
        logf("type_anchor: '%ls' name string not resident (%lu ms)",
             class_name, t_str - t0);
        return false;
    }
    logf("type_anchor: %zu name string hits for '%ls' (%lu ms)",
         name_hits.size(), class_name, t_str - t0);

    for (std::uintptr_t name_addr : name_hits) {
        if (!mem_is_userland(name_addr)) continue;

        std::vector<std::uintptr_t> name_refs;
        parallel_scan_u64(regions, static_cast<std::uint64_t>(name_addr),
                          name_refs);
        for (std::uintptr_t r : name_refs) {
            if (r <= OFF_OBJ_NAME) continue;
            std::uintptr_t obj_ptr = r - OFF_OBJ_NAME;
            if (!mem_is_userland(obj_ptr)) continue;

            std::vector<std::uintptr_t> obj_refs;
            parallel_scan_u64(regions, static_cast<std::uint64_t>(obj_ptr),
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

}  // namespace farever
