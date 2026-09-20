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
PFN_hl_alloc_obj g_orig   = nullptr;
// v0.4.15: stash the target address of hl_alloc_obj so we can later
// MH_DisableHook just that one (not MH_ALL_HOOKS, which would also
// kill the D3D12 hooks installed by d3d12_hook.cpp).
void*            g_target = nullptr;

struct Watcher {
    std::wstring   class_name;
    AllocCallback  cb;
    std::uintptr_t type_ptr = 0;   // filled in on first allocation
};

// Per-type dispatch list. Multiple watchers can register for the same
// class name; all matching callbacks fire (in registration order).
struct Dispatch {
    std::vector<AllocCallback> cbs;
};

std::mutex                                   g_mu;
std::vector<Watcher>                         g_watchers;        // append-only, never reordered
std::unordered_map<std::uintptr_t, Dispatch> g_type_to_disp;    // empty cbs = no match
std::atomic<bool>                            g_installed{false};

// Tail handler list. Registered once at module init, then read lock-free
// from hook_alloc_obj. Fixed cap so we can avoid taking g_mu on the hot
// path.
constexpr int kMaxTailHandlers = 4;
std::atomic<TailHandler>                     g_tail_handlers[kMaxTailHandlers]{};
std::atomic<int>                             g_tail_handler_count{0};

// Read the class name from hl_type.obj.name without faulting. The
// pointer is owned by libhl and lives for the process lifetime, so
// once we've read it we can use it indefinitely. Returns nullptr if
// the type doesn't carry a name (anonymous hl_type variants - those
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
// Windows - direct wcscmp works. SEH wrapper because the name buffer
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
    // Snapshot the per-type callback list under lock, then call them
    // outside the lock - callbacks can take their own mutexes without
    // risking inversion against hl_hook's.
    std::vector<AllocCallback> to_call;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_type_to_disp.find(type_ptr);
        if (it != g_type_to_disp.end()) {
            if (it->second.cbs.empty()) return;   // negatively cached
            to_call = it->second.cbs;
        } else {
            // Learn this type.
            const wchar_t* name = read_class_name(type_ptr);
            Dispatch d;
            if (name) {
                for (std::size_t i = 0; i < g_watchers.size(); ++i) {
                    if (wstr_equals(name, g_watchers[i].class_name.c_str())) {
                        d.cbs.push_back(g_watchers[i].cb);
                        g_watchers[i].type_ptr = type_ptr;
                    }
                }
            }
            if (!d.cbs.empty()) {
                logf("hl_hook: cached '%ls' -> hl_type* 0x%llx (%zu cb)",
                     name ? name : L"<null>",
                     static_cast<unsigned long long>(type_ptr),
                     d.cbs.size());
            }
            to_call = d.cbs;
            g_type_to_disp.emplace(type_ptr, std::move(d));
        }
    }
    for (AllocCallback cb : to_call) cb(obj);
}

void run_tail_handlers() {
    int count = g_tail_handler_count.load(std::memory_order_acquire);
    if (count > kMaxTailHandlers) count = kMaxTailHandlers;
    for (int i = 0; i < count; ++i) {
        TailHandler h = g_tail_handlers[i].load(std::memory_order_acquire);
        if (h) h();
    }
}

std::uintptr_t hook_alloc_obj(std::uintptr_t type_ptr) {
    std::uintptr_t result = g_orig ? g_orig(type_ptr) : 0;
    dispatch(type_ptr, result);
    // Tail handlers fire on every alloc, on the thread that called
    // hl_alloc_obj - see hl_hook.h for the hxbit-safety argument.
    run_tail_handlers();
    return result;
}

}  // namespace

void hl_hook_register(const wchar_t* class_name, AllocCallback cb) {
    if (!class_name || !cb) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_watchers.push_back({class_name, cb, 0});
    // Invalidate cache entries so already-learned types pick up this
    // new watcher on next alloc. Cheap; called once per module init.
    g_type_to_disp.clear();
    logf("hl_hook: registered watcher for '%ls'", class_name);
}

std::uintptr_t hl_hook_get_type(const wchar_t* class_name) {
    if (!class_name) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& w : g_watchers) {
        if (w.class_name == class_name) return w.type_ptr;
    }
    return 0;
}

bool hl_hook_class_name(std::uintptr_t obj, char* out, std::size_t cap) {
    if (!obj || !out || cap == 0) return false;
    out[0] = 0;
    __try {
        std::uintptr_t type_ptr =
            *reinterpret_cast<const std::uintptr_t*>(obj);
        const wchar_t* name = read_class_name(type_ptr);   // SEH-guarded inside
        if (!name) return false;
        std::size_t i = 0;
        for (; name[i] && i + 1 < cap; ++i)
            out[i] = static_cast<char>(name[i] & 0x7F);
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

void hl_hook_register_tail(TailHandler fn) {
    if (!fn) return;
    int idx = g_tail_handler_count.fetch_add(1, std::memory_order_acq_rel);
    if (idx >= kMaxTailHandlers) {
        logf("hl_hook: too many tail handlers (cap=%d), dropping",
             kMaxTailHandlers);
        // Reverse the increment so subsequent registrations get a sane count.
        g_tail_handler_count.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    g_tail_handlers[idx].store(fn, std::memory_order_release);
    logf("hl_hook: tail handler registered (slot=%d)", idx);
}

bool hl_hook_install(const LibHL& libhl) {
    if (g_installed.exchange(true)) return true;
    if (!libhl.hl_alloc_obj) {
        logf("hl_hook: refusing install - hl_alloc_obj not resolved");
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
    g_target = libhl.hl_alloc_obj;
    logf("hl_hook: hl_alloc_obj hooked, %zu watcher(s) registered",
         g_watchers.size());
    return true;
}

void hl_hook_uninstall() {
    if (!g_installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    // Don't MH_Uninitialize - other mods may still be using MinHook.
    g_orig = nullptr;
    logf("hl_hook: hl_alloc_obj unhooked");
}

// v0.4.15 surgical disable: targets only the hl_alloc_obj trampoline
// so the D3D12 hooks installed by d3d12_hook.cpp keep working. Used
// by the anticrash mode after the Hero lock is stable. After this
// returns, hl_alloc_obj calls from the game bypass us entirely - zero
// per-allocation overhead, but no more watcher dispatch. Idempotent.
//
// v0.4.15.1: keep g_target and g_orig populated after disable so the
// self-heal path can re-enable without re-resolving them.
void hl_hook_disable_alloc() {
    if (!g_installed.exchange(false)) return;
    if (g_target) {
        MH_STATUS st = MH_DisableHook(g_target);
        logf("hl_hook: hl_alloc_obj disabled (anticrash mode), MH=%d",
             (int)st);
    } else {
        logf("hl_hook: hl_alloc_obj disable skipped (no target cached)");
    }
    // Intentionally NOT nulling g_target / g_orig - re-enable needs them.
}

// v0.4.15.1: re-arm the trampoline after a previous disable. The
// MinHook patch is still in place (MH_DisableHook only stops trampoline
// execution, doesn't unpatch). Just MH_EnableHook flips it back on.
// Watchers from before are still registered. Returns true on success.
bool hl_hook_re_enable_alloc() {
    if (g_installed.load()) return true;
    if (!g_target) {
        logf("hl_hook: re-enable refused - no cached target");
        return false;
    }
    MH_STATUS st = MH_EnableHook(g_target);
    if (st != MH_OK) {
        logf("hl_hook: re-enable failed, MH=%d", (int)st);
        return false;
    }
    g_installed.store(true);
    logf("hl_hook: hl_alloc_obj re-enabled (anticrash self-heal)");
    return true;
}

}  // namespace farever
