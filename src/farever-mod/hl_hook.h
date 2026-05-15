#pragma once

#include "libhl.h"

#include <cstdint>

namespace farever {

// Callback signature for hl_alloc_obj watchers. `obj` is the freshly
// allocated object pointer (return value of hl_alloc_obj). The
// hl_type is implicit — by the time a callback fires, the dispatcher
// has already matched it against the class name the watcher
// registered for.
//
// IMPORTANT: callbacks run on whatever HashLink thread does the
// allocation (main + GC threads). Keep them cheap and lock-light.
using AllocCallback = void(*)(std::uintptr_t obj);

// Install MinHook on hl_alloc_obj. Idempotent.
bool hl_hook_install(const LibHL& libhl);
void hl_hook_uninstall();

// Register a watcher for a Haxe class. The dispatcher reads the class
// name from `hl_type.obj.name` on first allocation per type, matches
// it against registered names, and caches the hl_type* → watcher map
// so the steady-state path is one hash lookup. Register watchers
// BEFORE the hook installs (or accept a few allocations missed until
// the cache learns).
void hl_hook_register(const wchar_t* class_name, AllocCallback cb);

}  // namespace farever
