// Worker-thread client.GameCamera yaw reader. See camera_state.h. All game
// reads go through the SEH-guarded mem_read_* accessors and a type-tag check
// guards the cached pointer against Boehm-GC slot reuse. Productionised from
// the camera_probe diagnostic spike (#65).

#include "camera_state.h"
#include "hl_hook.h"
#include "mem_scan.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace farever {
namespace {

// client.GameCamera field offset (tools/classes_v017.json). The orbit azimuth
// is declared on client.BaseCamera; curDirection is the game-smoothed yaw the
// camera actually renders at (direction@208 is the un-smoothed target).
constexpr std::size_t OFF_CAM_CURDIR = 256;   // f64 smoothed azimuth (yaw)

std::atomic<std::uintptr_t> g_camera{0};
std::atomic<bool>           g_valid{false};
// std::atomic<double> is not guaranteed lock-free everywhere; store the IEEE
// bit pattern as a u64 so publish/consume are plain atomic integer ops.
std::atomic<std::uint64_t>  g_yaw_bits{0};

void on_camera_alloc(std::uintptr_t obj) {
    if (!obj) return;
    g_camera.store(obj, std::memory_order_release);   // newest alloc wins
}

}  // namespace

void camera_state_start() {
    hl_hook_register(L"client.GameCamera", on_camera_alloc);
    logf("camera_state: watcher registered (client.GameCamera)");
}

void camera_state_tick() {
    std::uintptr_t cam = g_camera.load(std::memory_order_acquire);
    if (!cam || !mem_is_userland(cam)) {
        g_valid.store(false, std::memory_order_release);
        return;
    }

    // Type-tag guard: the GC reuses dead slots without unmapping, so a stale
    // pointer can read cleanly but be garbage. Compare the type tag at +0
    // against the learned hl_type for client.GameCamera.
    static std::uintptr_t s_type = 0;
    if (s_type == 0) s_type = hl_hook_get_type(L"client.GameCamera");
    if (s_type) {
        std::uint64_t got = 0;
        if (!mem_read_u64(cam, &got) ||
            static_cast<std::uintptr_t>(got) != s_type) {
            g_camera.store(0, std::memory_order_release);
            g_valid.store(false, std::memory_order_release);
            return;
        }
    }

    double yaw = 0.0;
    if (!mem_read_bytes(cam + OFF_CAM_CURDIR, &yaw, sizeof(yaw))) {
        g_valid.store(false, std::memory_order_release);
        return;
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &yaw, sizeof(bits));
    g_yaw_bits.store(bits, std::memory_order_release);
    g_valid.store(true, std::memory_order_release);
}

bool camera_state_yaw(double& out) {
    if (!g_valid.load(std::memory_order_acquire)) return false;
    std::uint64_t bits = g_yaw_bits.load(std::memory_order_acquire);
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

}  // namespace farever
