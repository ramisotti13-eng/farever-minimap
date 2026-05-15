// Phase 1 stub. Phase 2 will MH_CreateHook on libhl.hl_alloc_obj and
// dispatch object-birth events into the DPS + minimap modules. For
// now this just logs that the install was requested so the smoke test
// can confirm the scaffold reaches the right point in the boot order.

#include "hl_hook.h"
#include "log.h"

namespace farever {

bool hl_hook_install(const LibHL& libhl) {
    logf("hl_hook: install requested, hl_alloc_obj=%p "
         "(phase-1 stub — real hook lands in next commit)",
         libhl.hl_alloc_obj);
    return true;
}

void hl_hook_uninstall() {
    logf("hl_hook: uninstall (phase-1 stub)");
}

}  // namespace farever
