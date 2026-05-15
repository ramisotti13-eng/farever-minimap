// Scaffold-stage overlay. Doesn't render anything yet — just logs
// what's happening so we can verify the inject + hook chain.
//
// Next iteration will lift the ImGui-DX12 init machinery from
// minimap-dll/overlay.cpp and render the live DPS table.

#include "overlay.h"
#include "log.h"

#include <atomic>

namespace dpsmeter {
namespace {

std::atomic<uint64_t> g_present_count{0};
std::atomic<bool>     g_logged_first_queue{false};

}  // namespace

void overlay_on_present(IDXGISwapChain3* swap_chain,
                        ID3D12CommandQueue* captured_queue) {
    uint64_t n = g_present_count.fetch_add(1) + 1;
    // Log first frame + every 600 frames (~10 s at 60 Hz) so we can see
    // the hook is alive without flooding the log.
    if (n == 1) {
        logf("overlay: first Present, swap_chain=%p queue=%p",
             static_cast<void*>(swap_chain),
             static_cast<void*>(captured_queue));
    } else if (n % 600 == 0) {
        logf("overlay: %llu Present calls", static_cast<unsigned long long>(n));
    }
    if (!g_logged_first_queue.load() && captured_queue) {
        if (!g_logged_first_queue.exchange(true)) {
            logf("overlay: command queue captured at frame %llu",
                 static_cast<unsigned long long>(n));
        }
    }
}

void overlay_on_resize(IDXGISwapChain3*, UINT buffer_count, UINT width,
                       UINT height) {
    logf("overlay: ResizeBuffers count=%u %ux%u", buffer_count, width, height);
}

void overlay_after_resize(IDXGISwapChain3*) {
    // Nothing to recreate yet — no overlay resources.
}

void overlay_shutdown() {
    logf("overlay: shutdown after %llu frames",
         static_cast<unsigned long long>(g_present_count.load()));
}

}  // namespace dpsmeter
