#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farevermod {

// Install MinHook hooks on the D3D12/DXGI entry points we need to render
// our overlay:
//   IDXGISwapChain3::Present          (vtable slot 8)
//   IDXGISwapChain3::ResizeBuffers    (vtable slot 13)
//   ID3D12CommandQueue::ExecuteCommandLists (vtable slot 10)
//
// The Present hook is where overlay.cpp injects our ImGui frame. The
// ExecuteCommandLists hook captures the game's actual command queue
// (we can't render to a swap chain without it). The ResizeBuffers hook
// gives the overlay a chance to recreate its back-buffer-sized
// resources after window resize / fullscreen toggle.
//
// Idempotent: calling install() twice is a no-op.
bool d3d12_hook_install();
void d3d12_hook_uninstall();

// Set by ExecuteCommandLists hook the first time the game submits work.
// overlay.cpp polls this to know when it can lazy-init ImGui.
ID3D12CommandQueue* d3d12_captured_queue();

}  // namespace farevermod
