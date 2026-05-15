// Resolve libhl.dll's exported allocator + helpers.
//
// The dinput8 proxy gets us loaded BEFORE libhl is in the process — so
// we can't GetProcAddress immediately. Wait until the module appears
// (polling every 50 ms; libhl loads early in startup so the wait is
// usually a handful of ms).

#include "libhl.h"
#include "log.h"

#include <windows.h>

#include <chrono>
#include <thread>

namespace farever {

bool libhl_wait_and_resolve(LibHL* out) {
    DWORD t_start = GetTickCount();
    HMODULE libhl = nullptr;
    for (int tries = 0; tries < 600; ++tries) {  // ~30 s ceiling
        libhl = GetModuleHandleW(L"libhl.dll");
        if (libhl) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!libhl) {
        logf("libhl: never appeared in this process after 30 s — giving up");
        return false;
    }
    DWORD t_visible = GetTickCount();
    logf("libhl: module base %p (after %lu ms)",
         static_cast<void*>(libhl), t_visible - t_start);

    out->libhl_base        = libhl;
    out->hl_alloc_obj      = reinterpret_cast<void*>(
        GetProcAddress(libhl, "hl_alloc_obj"));
    out->hl_alloc_dynamic  = reinterpret_cast<void*>(
        GetProcAddress(libhl, "hl_alloc_dynamic"));
    out->hl_alloc_dynobj   = reinterpret_cast<void*>(
        GetProcAddress(libhl, "hl_alloc_dynobj"));
    out->hl_alloc_array    = reinterpret_cast<void*>(
        GetProcAddress(libhl, "hl_alloc_array"));
    out->hl_gc_dump_memory = reinterpret_cast<void*>(
        GetProcAddress(libhl, "hl_gc_dump_memory"));

    logf("libhl: hl_alloc_obj=%p hl_alloc_dynamic=%p hl_alloc_dynobj=%p "
         "hl_alloc_array=%p hl_gc_dump_memory=%p",
         out->hl_alloc_obj, out->hl_alloc_dynamic, out->hl_alloc_dynobj,
         out->hl_alloc_array, out->hl_gc_dump_memory);

    if (!out->hl_alloc_obj) {
        logf("libhl: FATAL hl_alloc_obj not exported — wrong libhl version?");
        return false;
    }
    return true;
}

}  // namespace farever
