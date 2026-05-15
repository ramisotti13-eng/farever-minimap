#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace dpsmeter {

// Scaffold-stage callbacks. For now these only log frame counters so we
// can verify the hook chain works end-to-end. The full ImGui overlay
// (damage table, encounter timer, etc.) will come once the damage scan
// is ported into the DLL.
void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* captured_queue);
void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT width, UINT height);
void overlay_after_resize(IDXGISwapChain3* swap_chain);
void overlay_shutdown();

}  // namespace dpsmeter
