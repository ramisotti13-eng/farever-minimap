#pragma once

// Shared in-process memory primitives: SEH-wrapped reads, bulk
// region scans, region enumeration.
//
// All of these live in this TU because Windows SEH (__try/__except)
// cannot coexist with C++ destructor objects in the same function.
// Callers can freely use std::vector / std::string etc. — only this
// TU's helpers actually touch __try/__except blocks.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dpsmeter {

struct Region {
    std::uintptr_t base;
    std::size_t    size;
};

// HashLink heap is above 4 GB on Windows x64; upper bound is the
// architecturally-mandated 47-bit user-mode cap.
constexpr std::uintptr_t kUserlandLo = 0x0000000100000000ULL;
constexpr std::uintptr_t kUserlandHi = 0x00007FFFFFFFFFFFULL;

inline bool mem_is_userland(std::uintptr_t v) {
    return v >= kUserlandLo && v <= kUserlandHi;
}

// One-shot snapshot of MEM_COMMIT readable+writable regions, filtered
// to a useful size range (>= 64 KiB, <= 1 GiB). Mirrors the policy in
// minimap-dll/hero_scan.cpp.
std::vector<Region> mem_collect_regions();

// Bulk scan output. Fixed C buffer so the SEH-wrapped scanner stays
// C++ destructor-free.
constexpr std::size_t kScanCap = 8192;
struct ScanBuf {
    std::uintptr_t hits[kScanCap];
    std::size_t    count;
    std::size_t    overflow;
};

// Find every 8-aligned u64 in [base, base+size) equal to `target`.
// Returns true if the whole region was scanned without faulting; false
// if a page fault interrupted (whatever was written stays valid).
bool mem_scan_u64(std::uintptr_t base, std::size_t size,
                  std::uint64_t target, ScanBuf* out);

// Find every byte offset where `needle` matches.
bool mem_scan_bytes(std::uintptr_t base, std::size_t size,
                    const std::uint8_t* needle, std::size_t needle_len,
                    ScanBuf* out);

// Typed single-value reads. Return true if the read didn't fault.
bool mem_read_u8 (std::uintptr_t addr, std::uint8_t*  out);
bool mem_read_u32(std::uintptr_t addr, std::uint32_t* out);
bool mem_read_i32(std::uintptr_t addr, std::int32_t*  out);
bool mem_read_u64(std::uintptr_t addr, std::uint64_t* out);
bool mem_read_f64(std::uintptr_t addr, double*        out);

// Bulk read of `n` bytes into a caller-provided buffer.
bool mem_read_bytes(std::uintptr_t addr, void* dst, std::size_t n);

}  // namespace dpsmeter
