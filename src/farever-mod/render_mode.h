#pragma once

#include <windows.h>

// Render-backend chooser (#226). Resolves which backend to use at boot and
// owns the persisted choice in data/render_mode.txt. The choice is applied
// on the NEXT launch: when render_mode_resolve() shows the first-run dialog
// it returns Defer, and dllmain then skips the render/hook path for that one
// session (so a boot-time modal never races the game-swapchain queue capture
// that broke the overlay in issue #60).

namespace farever {

enum class RenderModeResolution {
    Defer,             // first-run dialog shown (or dismissed): render nothing
                       // this session, the user restarts.
    UseDcomp,          // run the DirectComposition backend this session.
    UseGameSwapchain,  // run the game-swapchain backend this session.
};

// Boot path. Precedence:
//   1. data/force_dcomp.flag present     -> UseDcomp (power-user override).
//   2. data/render_mode.txt has a choice -> that backend.
//   3. otherwise (first run / unparseable) -> show the chooser, save the pick,
//      return Defer.
RenderModeResolution render_mode_resolve();

// Persist a backend choice to data/render_mode.txt. Used by the overlay
// settings toggle. The running backend does not change (it is boot-bound);
// the value takes effect on the next launch.
void render_mode_save(bool game_swapchain);

}  // namespace farever
