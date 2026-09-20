#pragma once

// Shared in-process memory primitives: SEH-wrapped reads, bulk
// region scans, region enumeration.
//
// All of these live in this TU because Windows SEH (__try/__except)
// cannot coexist with C++ destructor objects in the same function.
// Callers can freely use std::vector / std::string etc. - only this
// TU's helpers actually touch __try/__except blocks.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace farever {

struct Region {
    std::uintptr_t base;
    std::size_t    size;
};

// Plausible-userland-pointer window. The HashLink GC heap usually maps
// well above 4 GB on Windows x64, but issue #48 showed a system where
// ent.Hero allocations land at ~3.7 GB (just under the old 4 GB floor),
// so every hero was rejected and the overlay never locked / rendered.
// The floor only needs to exclude the null page + low reserved region;
// the real validation is done by the hero-lock predicates and type-tag
// checks, and all reads are SEH-guarded. Lowering it costs high-heap
// systems nothing (their addresses sit far above any low floor).
// Upper bound is the architecturally-mandated 47-bit user-mode cap.
constexpr std::uintptr_t kUserlandLo = 0x0000000000010000ULL;  // 64 KiB
constexpr std::uintptr_t kUserlandHi = 0x00007FFFFFFFFFFFULL;

inline bool mem_is_userland(std::uintptr_t v) {
    return v >= kUserlandLo && v <= kUserlandHi;
}

// One-shot snapshot of MEM_COMMIT readable+writable regions, filtered
// to a useful size range (>= 64 KiB, <= 1 GiB).
std::vector<Region> mem_collect_regions();

// Bulk scan output. Fixed C buffer so the SEH-wrapped scanner stays
// C++ destructor-free.
constexpr std::size_t kScanCap = 8192;
struct ScanBuf {
    std::uintptr_t hits[kScanCap];
    std::size_t    count;
    std::size_t    overflow;
};

bool mem_scan_u64(std::uintptr_t base, std::size_t size,
                  std::uint64_t target, ScanBuf* out);

bool mem_scan_bytes(std::uintptr_t base, std::size_t size,
                    const std::uint8_t* needle, std::size_t needle_len,
                    ScanBuf* out);

bool mem_read_u8 (std::uintptr_t addr, std::uint8_t*  out);
bool mem_read_u32(std::uintptr_t addr, std::uint32_t* out);
bool mem_read_i32(std::uintptr_t addr, std::int32_t*  out);
bool mem_read_u64(std::uintptr_t addr, std::uint64_t* out);
bool mem_read_f64(std::uintptr_t addr, double*        out);
bool mem_read_bytes(std::uintptr_t addr, void* dst, std::size_t n);

}  // namespace farever
