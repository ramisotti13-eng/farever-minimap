// Kiero-style D3D12 vtable hook. Same pattern as the dpsmeter-dll /
// minimap-dll implementations — discover Present / ResizeBuffers /
// ExecuteCommandLists from a throwaway swap chain, then MinHook them.
//
// In this unified mod the Present detour drives the damage pump (so
// the heap reads happen on a thread the HashLink GC already knows
// about — see feedback_hashlink_pump_thread.md for why this matters).

#include "d3d12_hook.h"
#include "log.h"
#include "damage.h"

#include <windows.h>
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>

namespace farever {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3* self, UINT sync_interval, UINT flags);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3* self, UINT buffer_count, UINT width, UINT height,
    DXGI_FORMAT new_format, UINT swap_chain_flags);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue* self, UINT num_command_lists,
    ID3D12CommandList* const* command_lists);

PresentFn               g_orig_present              = nullptr;
ResizeBuffersFn         g_orig_resize_buffers       = nullptr;
ExecuteCommandListsFn   g_orig_execute_command_lists = nullptr;

std::atomic<ID3D12CommandQueue*> g_captured_queue{nullptr};
std::atomic<bool> g_installed{false};

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain3* self,
                                       UINT sync_interval, UINT flags) {
    // Present is called once per frame on the game's render thread,
    // which HashLink has already registered with its GC. This is the
    // safe place to drain the alloc-hook event queue and read object
    // fields — anywhere else the cross-thread reads race with hxbit's
    // deserialiser and crash the engine.
    damage_tick();
    return g_orig_present(self, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hook_resize_buffers(
    IDXGISwapChain3* self, UINT buffer_count, UINT width, UINT height,
    DXGI_FORMAT new_format, UINT swap_chain_flags) {
    return g_orig_resize_buffers(self, buffer_count, width, height,
                                 new_format, swap_chain_flags);
}

void STDMETHODCALLTYPE hook_execute_command_lists(
    ID3D12CommandQueue* self, UINT num_command_lists,
    ID3D12CommandList* const* command_lists) {
    if (g_captured_queue.load() == nullptr) {
        D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_captured_queue.store(self);
            logf("d3d12: captured DIRECT queue %p",
                 static_cast<void*>(self));
        }
    }
    g_orig_execute_command_lists(self, num_command_lists, command_lists);
}

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

bool discover_vtable_pointers(void** out_present, void** out_resize_buffers,
                              void** out_execute_command_lists) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"farever_mod_dummy";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom) { logf("d3d12: RegisterClassExW failed"); return false; }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    bool ok = false;
    do {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                      reinterpret_cast<void**>(&factory)))) break;

        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     __uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(&device)))) break;

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(
                &queue_desc, __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(&queue)))) break;

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = 100; scd.Height = 100;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> swap_chain;
        if (FAILED(factory->CreateSwapChainForHwnd(
                queue.p, hwnd, &scd, nullptr, nullptr, &swap_chain))) break;

        void** swap_chain_vt = *reinterpret_cast<void***>(swap_chain.p);
        void** queue_vt      = *reinterpret_cast<void***>(queue.p);

        *out_present              = swap_chain_vt[8];
        *out_resize_buffers       = swap_chain_vt[13];
        *out_execute_command_lists = queue_vt[10];
        ok = true;
    } while (false);

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

bool d3d12_hook_install() {
    if (g_installed.exchange(true)) return true;

    void* present_ptr        = nullptr;
    void* resize_buffers_ptr = nullptr;
    void* exec_ptr           = nullptr;
    if (!discover_vtable_pointers(&present_ptr, &resize_buffers_ptr, &exec_ptr)) {
        g_installed.store(false);
        return false;
    }
    logf("d3d12: vtable Present=%p ResizeBuffers=%p ExecuteCommandLists=%p",
         present_ptr, resize_buffers_ptr, exec_ptr);

    // MinHook was likely already initialised by hl_hook. That's fine —
    // MH_Initialize returns MH_ERROR_ALREADY_INITIALIZED which we
    // treat as success.
    if (MH_Initialize() != MH_OK) {
        // ignore, continue
    }

    auto install_one = [](void* target, void* detour, void** orig,
                          const char* name) -> bool {
        if (MH_CreateHook(target, detour, orig) != MH_OK) {
            logf("d3d12: MH_CreateHook(%s) failed", name);
            return false;
        }
        if (MH_EnableHook(target) != MH_OK) {
            logf("d3d12: MH_EnableHook(%s) failed", name);
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= install_one(present_ptr, reinterpret_cast<void*>(&hook_present),
                      reinterpret_cast<void**>(&g_orig_present), "Present");
    ok &= install_one(resize_buffers_ptr,
                      reinterpret_cast<void*>(&hook_resize_buffers),
                      reinterpret_cast<void**>(&g_orig_resize_buffers),
                      "ResizeBuffers");
    ok &= install_one(exec_ptr,
                      reinterpret_cast<void*>(&hook_execute_command_lists),
                      reinterpret_cast<void**>(&g_orig_execute_command_lists),
                      "ExecuteCommandLists");

    if (!ok) {
        d3d12_hook_uninstall();
        return false;
    }
    log_line("d3d12: hooks installed");
    return true;
}

void d3d12_hook_uninstall() {
    if (!g_installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    // Don't MH_Uninitialize — hl_hook still uses MinHook.
    g_orig_present              = nullptr;
    g_orig_resize_buffers       = nullptr;
    g_orig_execute_command_lists = nullptr;
    g_captured_queue.store(nullptr);
    log_line("d3d12: hooks uninstalled");
}

ID3D12CommandQueue* d3d12_captured_queue() {
    return g_captured_queue.load();
}

}  // namespace farever
