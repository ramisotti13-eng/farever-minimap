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

// v0.4.15 anticrash diagnostic. Same diag box gets a third line so
// the user can see whether anticrash mode is armed (waiting for
// lock-stable countdown) or disarmed (hook removed, DPS dead).
void overlay_set_anticrash_state(bool anticrash);

// v0.4.17 Option B: tells overlay_init to skip its wndproc subclass.
// Must be called BEFORE the first overlay_on_present. When true,
// overlay renders into a separately-owned top-level window whose
// own wndproc forwards mouse input to the game; the legacy subclass
// would just eat clicks before they reach our forwarding logic.
void overlay_set_standalone_window(bool on);

// v0.4.17 Option B: override the HWND that overlay_init binds to.
// Composition swap chains have no associated window, so
// swap_chain->GetDesc().OutputWindow returns NULL, ImGui-Win32 then
// can't compute IO.DisplaySize and the GUI renders empty. Pass our
// overlay-window HWND here before the first overlay_on_present so
// ImGui sees the right window for sizing / input plumbing.
void overlay_set_window_hwnd(void* hwnd);

// v0.5.2: force the cached "cursor is over an interactive widget"
// state. Used by overlay_window when toggling F7 to hidden so the
// game's wndproc subclass stops eating mouse clicks immediately
// (otherwise the last frame's stale value could keep blocking
// until next render).
void overlay_set_wants_real_input(bool on);

// v0.5.2: virtual-key code currently bound to the overlay show/hide
// toggle (default F7, user-rebindable in keybinds.json). Polled by
// overlay_window's render thread via GetAsyncKeyState; reflects the
// last keybinds_maybe_reload() result.
unsigned overlay_get_toggle_overlay_key();

}  // namespace farever
