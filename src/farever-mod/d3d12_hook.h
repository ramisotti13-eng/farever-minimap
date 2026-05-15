#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// MinHook-based D3D12 vtable hook. Captures the game's swap chain
// command queue via ExecuteCommandLists, drives our render-thread
// pump on every Present, and (later) hosts the ImGui overlay.
//
// Idempotent: calling install() twice is a no-op.
bool d3d12_hook_install();
void d3d12_hook_uninstall();

ID3D12CommandQueue* d3d12_captured_queue();

}  // namespace farever
