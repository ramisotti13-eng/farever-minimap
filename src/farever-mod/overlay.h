#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

namespace farever {

// Render backend, selected once at boot (ImGui-DX12 binds a single
// device for the process lifetime, so this can't change at runtime).
//   Dcomp         - own composition swap chain + DirectComposition visual
//                   on the game HWND (v0.6 default; robust but the desktop
//                   compositor blends our full-screen surface every frame
//                   → ultrawide cost #50, no Proton, AMD MPO quirks).
//   GameSwapchain - render into the game's OWN swap chain via the Present
//                   hook (the v0.4.x path): no compositor blend, works
//                   under Proton/DXVK, dodges AMD MPO.
enum class RenderBackend { Dcomp, GameSwapchain };
RenderBackend overlay_backend();
void overlay_set_backend(RenderBackend b);   // call once before first present

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

// #65: switch the per-character UI-settings profile. `key` is the sanitized
// character name (same key the POI / boss-timer profiles use); empty/null is
// ignored. Seeds the character's ui_state file from the shared one on first
// use, then the render thread reloads the settings. Called from hl_pump.
void overlay_set_profile(const char* key);

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

// v0.5.3 issue #30: union bounding box of all ImGui content drawn in
// the most recent overlay_on_present, in pixel coordinates of the
// overlay swap chain. Used by overlay_window to feed dirty rects to
// IDXGISwapChain1::Present1 so DWM only re-composites the regions
// where our UI actually lives, not the entire 3440×1440 of empty
// transparent buffer. Returns false if no content was drawn (caller
// should fall back to a full Present).
bool overlay_get_dirty_rect(int* x, int* y, int* w, int* h);

// Compass strip read-state accessors, exposed to the Lua plugin API
// (farever.compass.*). All cheap atomic loads, safe to call from the
// render thread which is where plugins run.
bool  compass_is_visible();
float compass_radius_m();
bool  compass_cardinals_on();

// Plugin-added compass markers (farever.compass.add_marker /
// remove_marker). All-thread-on-render-thread, no locking. ttl_sec=0
// means until removed; >0 means auto-expire that many seconds after
// ImGui::GetTime() was called at add time. Returns a positive handle,
// or 0 if the store is full (kMaxCompassPluginMarkers).
int  compass_add_marker(float x, float y, float z, const char* name,
                        unsigned char color, unsigned char icon,
                        double ttl_sec);
bool compass_remove_marker(int handle);

// Community-plugin theming (Options "Match plugin theme", default on). The
// plugin host brackets each plugin's ImGui window with push/pop so plugin
// windows adopt the overlay's palette + shape without any plugin-side change.
// A plugin opts out with a `plugin_theme = false` Lua global. _enabled()
// reflects the user toggle; push and pop must be called as a matched pair.
void overlay_push_plugin_theme();
void overlay_pop_plugin_theme();
bool overlay_plugin_theme_enabled();

// #99: draw one cell of a shipped icon atlas as an ImGui image at the
// current cursor. `atlas` is a basename under data/atlases/UI/icons/,
// cell_x / cell_y are cell indices (not pixels), cell_size the cell edge
// in px and cell_w / cell_h how many cells the icon spans. `draw_px` is
// the on-screen edge length. Only valid inside an overlay frame, which is
// where plugin on_render runs. Returns false when the atlas is missing or
// could not be loaded, so callers can fall back to text.
bool overlay_draw_atlas_cell(const char* atlas, int cell_x, int cell_y,
                             int cell_size, int cell_w, int cell_h,
                             float draw_px);

}  // namespace farever
