#pragma once

#include "libhl.h"

namespace farever {

// v0.5.8 architectural change: replaces the D3D12 Present hook on the
// game's swap chain as the HashLink tick driver. Instead of riding the
// game's render thread, we spawn our own thread, register it with the
// HL GC via hl_register_thread, and drive damage_tick / hero_state_tick
// / target_state_tick at ~60 Hz with explicit hl_blocking discipline.
//
// Why: the v0.4 -> v0.5 bisection identified vtable patches on the
// game's swap chain as the cause of the recurring DX12Driver.present
// access violations (issues #41, #42, #43 on the post-game-patch
// builds). v0.5 dropped the submit path but kept the Present hook as
// a tick driver - and the bisection memo also showed that "patch
// presence alone is enough" to trigger the crash on some setups.
//
// The memory note feedback_hashlink_pump_thread warned against
// background threads, but that prior attempt did not use hl_register_thread
// / hl_blocking correctly. With the proper libhl-embedded threading
// pattern (register at start, blocking(true) while sleeping,
// blocking(false) while reading) the GC sees our thread as a first-
// class participant and HL reads from it are safe.
bool hl_pump_start(const LibHL& libhl);
void hl_pump_stop();

// True while the worker thread is registered + ticking. The D3D12
// Present hook queries this to skip its own tick calls when the worker
// is driving them - otherwise we'd tick twice per frame and possibly
// race on state.
bool hl_pump_is_active();

// Win32 thread id of the running worker, or 0 if not running. Used by
// tick-path code that does hxbit-unsafe calls (hl_hi64get against the
// NetworkSerializer.refs map; hl_dyn_getp on objects whose fields hxbit
// is mid-deserialising). Those calls compare GetCurrentThreadId() to
// this value and bail when matched. See [[hxbit-uid-resolution]] +
// [[feedback-hashlink-pump-thread]] for the reentrancy hazard.
unsigned long hl_pump_worker_tid();

}  // namespace farever
