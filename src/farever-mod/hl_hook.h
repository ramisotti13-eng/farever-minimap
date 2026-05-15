#pragma once

#include "libhl.h"

namespace farever {

// Install the MinHook trampoline on hl_alloc_obj. The detour fans the
// event out to whichever module(s) registered for this type tag.
//
// In phase 1 (scaffold) this is a no-op stub — we just want the proxy
// + libhl resolution path proven. Phase 2 fills the hook in.
bool hl_hook_install(const LibHL& libhl);
void hl_hook_uninstall();

}  // namespace farever
