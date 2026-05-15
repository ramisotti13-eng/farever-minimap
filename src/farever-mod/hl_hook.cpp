// hl_alloc_obj hook + type-learning dispatcher.
//
// The HashLink runtime exposes `vdynamic *hl_alloc_obj(hl_type *t)` as
// the allocator for every object instance. We trampoline it via
// MinHook, call the original to get the real object pointer, then
// route to whichever registered watcher cares about that class.
//
// Type learning: instead of scanning the heap to anchor canonical
// hl_type pointers, we read the class name (UTF-16 string at
// hl_type.obj.name) the first time each unseen type is allocated.
// One name comparison, then the type pointer is cached for the
// process lifetime. Steady-state cost per allocation is a single
// hash lookup under a mutex.

#include "hl_hook.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace farever {
namespace {

// HashLink struct layout we depend on:
//   hl_type:        kind:u32 @0, obj:*hl_type_obj @8
//   hl_type_obj:    name:*wchar_t @16
constexpr std::size_t OFF_TYPE_OBJ  = 8;
constexpr std::size_t OFF_OBJ_NAME  = 16;
constexpr std::uint32_t HOBJ        = 11;

using PFN_hl_alloc_obj = std::uintptr_t (*)(std::uintptr_t /*hl_type* t*/);
PFN_hl_alloc_obj g_orig = nullptr;

struct Watcher {
    std::wstring  class_name;
    AllocCallback cb;
};

std::mutex                              g_mu;
std::vector<Watcher>                    g_watchers;        // append-only, never reordered
std::unordered_map<std::uintptr_t, int> g_type_to_idx;     // -1 = no match
std::atomic<bool>                       g_installed{false};

// Read the class name from hl_type.obj.name without faulting. The
// pointer is owned by libhl and lives for the process lifetime, so
// once we've read it we can use it indefinitely. Returns nullptr if
// the type doesn't carry a name (anonymous hl_type variants — those
// aren't classes we'd watch for anyway).
const wchar_t* read_class_name(std::uintptr_t type_ptr) {
    std::uintptr_t obj  = 0;
    std::uintptr_t name = 0;
    __try {
        obj  = *reinterpret_cast<const std::uintptr_t*>(type_ptr + OFF_TYPE_OBJ);
        if (!obj) return nullptr;
        name = *reinterpret_cast<const std::uintptr_t*>(obj + OFF_OBJ_NAME);
        if (!name) return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return reinterpret_cast<const wchar_t*>(name);
}

// Compare a HashLink wchar string against a watcher's stored name.
// HashLink stores names UTF-16 NUL-terminated, same as wchar_t on
// Windows — direct wcscmp works. SEH wrapper because the name buffer
// could in theory be unmapped (it isn't in practice).
int wstr_equals(const wchar_t* a, const wchar_t* b) {
    __try {
        return wcscmp(a, b) == 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void dispatch(std::uintptr_t type_ptr, std::uintptr_t obj) {
    if (!obj) return;
    AllocCallback cb = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_type_to_idx.find(type_ptr);
        if (it != g_type_to_idx.end()) {
            int idx = it->second;
            if (idx < 0) return;                  // negatively cached
            cb = g_watchers[idx].cb;
        } else {
            // Learn this type.
            const wchar_t* name = read_class_name(type_ptr);
            int idx = -1;
            if (name) {
                for (std::size_t i = 0; i < g_watchers.size(); ++i) {
                    if (wstr_equals(name, g_watchers[i].class_name.c_str())) {
                        idx = static_cast<int>(i);
                        break;
                    }
                }
            }
            g_type_to_idx.emplace(type_ptr, idx);
            if (idx >= 0) {
                logf("hl_hook: cached '%ls' -> hl_type* 0x%llx",
                     name ? name : L"<null>",
                     static_cast<unsigned long long>(type_ptr));
                cb = g_watchers[idx].cb;
            }
        }
    }
    if (cb) cb(obj);
}

std::uintptr_t hook_alloc_obj(std::uintptr_t type_ptr) {
    std::uintptr_t result = g_orig ? g_orig(type_ptr) : 0;
    dispatch(type_ptr, result);
    return result;
}

}  // namespace

void hl_hook_register(const wchar_t* class_name, AllocCallback cb) {
    if (!class_name || !cb) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_watchers.push_back({class_name, cb});
    // Invalidate negative cache entries — a previously-learned type
    // might match this newly registered class. (Positive entries can
    // only become more inclusive, never wrong, so we leave them.)
    for (auto it = g_type_to_idx.begin(); it != g_type_to_idx.end(); ) {
        if (it->second < 0) it = g_type_to_idx.erase(it); else ++it;
    }
    logf("hl_hook: registered watcher for '%ls'", class_name);
}

bool hl_hook_install(const LibHL& libhl) {
    if (g_installed.exchange(true)) return true;
    if (!libhl.hl_alloc_obj) {
        logf("hl_hook: refusing install — hl_alloc_obj not resolved");
        g_installed.store(false);
        return false;
    }
    if (MH_Initialize() != MH_OK) {
        // MinHook is also used by the legacy minimap-dll / dpsmeter-dll
        // (when they're loaded alongside us). The second MH_Initialize
        // returns MH_ERROR_ALREADY_INITIALIZED, which we treat as ok.
        // We'll still call MH_CreateHook below; that's the operation
        // that actually matters.
        logf("hl_hook: MH_Initialize returned non-OK (probably already "
             "initialised by another loaded mod, continuing)");
    }
    if (MH_CreateHook(libhl.hl_alloc_obj,
                      reinterpret_cast<void*>(&hook_alloc_obj),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK) {
        logf("hl_hook: MH_CreateHook(hl_alloc_obj) failed");
        g_installed.store(false);
        return false;
    }
    if (MH_EnableHook(libhl.hl_alloc_obj) != MH_OK) {
        logf("hl_hook: MH_EnableHook(hl_alloc_obj) failed");
        g_installed.store(false);
        return false;
    }
    logf("hl_hook: hl_alloc_obj hooked, %zu watcher(s) registered",
         g_watchers.size());
    return true;
}

void hl_hook_uninstall() {
    if (!g_installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    // Don't MH_Uninitialize — other mods may still be using MinHook.
    g_orig = nullptr;
    logf("hl_hook: hl_alloc_obj unhooked");
}

}  // namespace farever
