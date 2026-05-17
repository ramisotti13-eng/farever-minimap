#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// Hook-side callbacks invoked from d3d12_hook.cpp.
void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* captured_queue);
void overlay_on_resize(IDXGISwapChain3* swap_chain, UINT buffer_count,
                       UINT width, UINT height);
void overlay_after_resize(IDXGISwapChain3* swap_chain);
void overlay_shutdown();

// Issue #13: backend pause for the DPS pipeline. When true, the
// DamageDisplay alloc-hook callback returns immediately and
// damage_tick short-circuits. Hotkey-toggleable from overlay.cpp;
// queried by damage.cpp on the hot path.
bool overlay_is_dps_tracking_paused();

// v0.4.13 kill switch (issues #12 / #16 bisection).
// Set once at module init from FAREVER_NO_OVERLAY=1. Makes
// overlay_on_present / overlay_on_resize / overlay_after_resize
// no-ops -- no ImGui init, no D3D submission to the game queue.
// Returns true iff the switch is engaged.
void overlay_kill();
bool overlay_killed();

// v0.4.14 diagnostic-box state. Called once from dllmain after
// reading the kill switches. When no_hl_tick is set, the overlay
// draws a tiny status box that bypasses the hero-lock gate so the
// user can visually confirm the mod is alive (issue #16 boot2 was
// abandoned because the user thought the mod failed to load).
void overlay_set_kill_switch_state(bool no_overlay, bool no_hl_tick);

}  // namespace farever
