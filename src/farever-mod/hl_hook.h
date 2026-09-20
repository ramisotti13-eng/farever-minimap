#pragma once

#include "libhl.h"

#include <cstddef>
#include <cstdint>

namespace farever {

// Callback signature for hl_alloc_obj watchers. `obj` is the freshly
// allocated object pointer (return value of hl_alloc_obj). The
// hl_type is implicit - by the time a callback fires, the dispatcher
// has already matched it against the class name the watcher
// registered for.
//
// IMPORTANT: callbacks run on whatever HashLink thread does the
// allocation (main + GC threads). Keep them cheap and lock-light.
using AllocCallback = void(*)(std::uintptr_t obj);

// Install MinHook on hl_alloc_obj. Idempotent.
bool hl_hook_install(const LibHL& libhl);
void hl_hook_uninstall();

// v0.4.15: surgical disable of only the hl_alloc_obj hook (the
// regular hl_hook_uninstall calls MH_DisableHook(MH_ALL_HOOKS) which
// would also kill the D3D12 hooks). Used by the anticrash mode that
// removes our alloc-hook trampoline once the Hero lock is stable, so
// the game's HashLink allocator runs without any of our overhead.
// Idempotent. The trampoline is removed; future hl_alloc_obj calls
// from the game bypass us entirely.
void hl_hook_disable_alloc();

// v0.4.15.1: re-enable the hl_alloc_obj hook after a previous
// disable_alloc. Used by the anticrash self-heal path: when polling
// loses the lock (zone transition replaced both Hero AND Player),
// we re-arm the alloc-hook so the watcher catches the new Hero
// allocation, then disarm again 5 s after the next stable lock.
// The MinHook patch and watcher registrations are still in place
// from hl_hook_install; this just re-arms the trampoline.
bool hl_hook_re_enable_alloc();

// Register a watcher for a Haxe class. The dispatcher reads the class
// name from `hl_type.obj.name` on first allocation per type, matches
// it against registered names, and caches the hl_type* → watcher map
// so the steady-state path is one hash lookup. Register watchers
// BEFORE the hook installs (or accept a few allocations missed until
// the cache learns).
void hl_hook_register(const wchar_t* class_name, AllocCallback cb);

// Look up the hl_type* cached for a registered class. Returns 0 until
// at least one allocation of that class has flowed through dispatch.
// Used by hero_state to drive a heap rescan after a lock drop -
// dungeon exits can reuse an existing Hero without firing a fresh
// alloc, so the rescan is the only way to find it again.
std::uintptr_t hl_hook_get_type(const wchar_t* class_name);

// Read the HashLink class name of an OBJECT (the UTF-16 hl_type.obj.name) into
// an ASCII buffer. Returns false on a bad pointer / nameless type. SEH-guarded;
// safe to call from the worker thread on a possibly-stale pointer.
bool hl_hook_class_name(std::uintptr_t obj, char* out, std::size_t cap);

// Tail handler - fires after EVERY successful hl_alloc_obj, regardless
// of whether the type matched a watcher. Runs on whichever HashLink
// thread called hl_alloc_obj, synchronously nested inside the
// trampoline. That gives the handler a context where hxbit's
// NetworkSerializer.refs bucket walk (if any) is suspended below it on
// the same thread - so hxbit-unsafe primitives like hl_hi64get are
// safe to call here, where they crash from a background worker
// (see [[hxbit-uid-resolution]] + [[feedback-hashlink-pump-thread]]).
//
// Fixed cap on handlers. Registration is one-shot at module init.
// Cost per alloc is one atomic load + one indirect call per handler.
using TailHandler = void(*)();
void hl_hook_register_tail(TailHandler fn);

}  // namespace farever
