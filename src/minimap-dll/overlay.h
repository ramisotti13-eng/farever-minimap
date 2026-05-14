#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farevermod {

// Called by the D3D12 Present hook every frame.
// `queue` is the game's captured DIRECT command queue (may be null until
// the first ExecuteCommandLists call, in which case overlay no-ops).
void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* queue);

// Called before ResizeBuffers is invoked on the game side. Release any
// back-buffer-sized resources.
void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT width, UINT height);

// Called immediately after ResizeBuffers returns. Recreate back-buffer
// resources off the new swap chain.
void overlay_after_resize(IDXGISwapChain3* swap_chain);

// Called from DllMain DETACH. Tear down ImGui + DX12 resources.
void overlay_shutdown();

}  // namespace farevermod
