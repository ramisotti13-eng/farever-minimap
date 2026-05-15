#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace dpsmeter {

// MinHook-based D3D12 vtable hook. See minimap-dll/d3d12_hook.h for the
// detailed write-up; this file is functionally identical.
//
// Idempotent: calling install() twice is a no-op.
bool d3d12_hook_install();
void d3d12_hook_uninstall();

ID3D12CommandQueue* d3d12_captured_queue();

}  // namespace dpsmeter
