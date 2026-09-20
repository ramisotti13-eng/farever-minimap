#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// MinHook-based D3D12 vtable hook. The installed hook set depends on the
// render backend (overlay_backend(), resolved before install):
//   Dcomp         - Present only, as a pure HashLink tick driver; the
//                   overlay renders on its own thread/swapchain.
//   GameSwapchain - Present (tick + overlay submit) + ResizeBuffers
//                   (RTV recreate) + ExecuteCommandLists (capture the
//                   game's DIRECT queue to submit the overlay on).
//
// Idempotent: calling install() twice is a no-op.
bool d3d12_hook_install();
void d3d12_hook_uninstall();

// The game's captured DIRECT command queue. Non-null only in the
// GameSwapchain backend, once the first ExecuteCommandLists is seen.
ID3D12CommandQueue* d3d12_captured_queue();

}  // namespace farever
