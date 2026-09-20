#pragma once

namespace farever {

// v0.4.17 Option B: our overlay no longer rides on the game's swap
// chain. Instead we own a top-level WS_EX_LAYERED window, our own
// D3D12 device + swapchain + queue, and our own render thread. The
// window follows the game's window rect each frame so the user sees
// the overlay sitting on top of the game.
//
// Why: the no_d3d12 bisection on v0.4.15.2 proved that vtable hooks
// on the game's swap chain (Present / ResizeBuffers /
// ExecuteCommandLists) are the recurring `DX12Driver.present`
// access-violation trigger. Removing all three made the game stable.
// Option B keeps only a minimal Present hook for HashLink tick
// driving (see d3d12_hook.cpp) and puts the entire render path on a
// separately-owned window so the game's swap chain is not patched
// for any rendering purpose.
//
// Returns true if the window + device + swapchain + render thread
// came up successfully. The render thread runs until
// overlay_window_stop() or process exit.
bool overlay_window_start();
void overlay_window_stop();

}  // namespace farever
