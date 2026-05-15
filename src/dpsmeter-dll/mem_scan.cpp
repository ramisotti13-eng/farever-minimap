// Implementation of the shared memory primitives. SEH is contained to
// this translation unit; everything exported has plain C++ signatures.

#include "mem_scan.h"

#include <windows.h>

namespace dpsmeter {

bool mem_scan_u64(std::uintptr_t base, std::size_t size,
                  std::uint64_t target, ScanBuf* out) {
    out->count    = 0;
    out->overflow = 0;
    __try {
        const std::uint64_t* p =
            reinterpret_cast<const std::uint64_t*>(base);
        std::size_t n = size / 8;
        for (std::size_t i = 0; i < n; ++i) {
            if (p[i] == target) {
                if (out->count < kScanCap) {
                    out->hits[out->count++] = base + i * 8;
                } else {
                    out->overflow++;
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool mem_scan_bytes(std::uintptr_t base, std::size_t size,
                    const std::uint8_t* needle, std::size_t needle_len,
                    ScanBuf* out) {
    out->count    = 0;
    out->overflow = 0;
    if (needle_len == 0 || size < needle_len) return true;
    __try {
        const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(base);
        std::size_t end = size - needle_len + 1;
        std::uint8_t first = needle[0];
        for (std::size_t i = 0; i < end; ++i) {
            if (p[i] != first) continue;
            std::size_t k = 1;
            for (; k < needle_len; ++k) {
                if (p[i + k] != needle[k]) break;
            }
            if (k == needle_len) {
                if (out->count < kScanCap) {
                    out->hits[out->count++] = base + i;
                } else {
                    out->overflow++;
                }
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool mem_read_u8(std::uintptr_t addr, std::uint8_t* out) {
    __try { *out = *reinterpret_cast<const std::uint8_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool mem_read_u32(std::uintptr_t addr, std::uint32_t* out) {
    __try { *out = *reinterpret_cast<const std::uint32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool mem_read_i32(std::uintptr_t addr, std::int32_t* out) {
    __try { *out = *reinterpret_cast<const std::int32_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool mem_read_u64(std::uintptr_t addr, std::uint64_t* out) {
    __try { *out = *reinterpret_cast<const std::uint64_t*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool mem_read_f64(std::uintptr_t addr, double* out) {
    __try { *out = *reinterpret_cast<const double*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool mem_read_bytes(std::uintptr_t addr, void* dst, std::size_t n) {
    __try {
        const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(addr);
        std::uint8_t* d = reinterpret_cast<std::uint8_t*>(dst);
        for (std::size_t i = 0; i < n; ++i) d[i] = src[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<Region> mem_collect_regions() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto cur = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    auto end = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<Region> out;
    out.reserve(4096);
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi)) == 0)
            break;
        auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        auto size = static_cast<std::size_t>(mbi.RegionSize);
        if (size == 0) { cur += 4096; continue; }
        cur = base + size;
        if (mbi.State != MEM_COMMIT) continue;
        if (size < (64 * 1024)) continue;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writable) == 0) continue;
        if (size > (1ULL << 30)) continue;
        out.push_back({base, size});
    }
    return out;
}

}  // namespace dpsmeter
