// Kiero-style D3D12 vtable hook.
//
// Strategy: spin up a *throwaway* D3D12 device + swap chain on a hidden
// window, read the function pointers out of their vtables, tear it all
// down again, then point MinHook at those same pointers. The vtable
// slots are stable across processes for the same Windows / driver
// version because they come from system DLLs (dxgi.dll, d3d12.dll).

#include "d3d12_hook.h"
#include "log.h"
#include "overlay.h"

#include <windows.h>
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>

namespace farevermod {
namespace {

// --- Signatures of the methods we hook --------------------------------------

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3* self, UINT sync_interval, UINT flags);

using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3* self, UINT buffer_count, UINT width, UINT height,
    DXGI_FORMAT new_format, UINT swap_chain_flags);

using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue* self, UINT num_command_lists,
    ID3D12CommandList* const* command_lists);

// Trampolines back to the game's originals.
PresentFn               g_orig_present              = nullptr;
ResizeBuffersFn         g_orig_resize_buffers       = nullptr;
ExecuteCommandListsFn   g_orig_execute_command_lists = nullptr;

std::atomic<ID3D12CommandQueue*> g_captured_queue{nullptr};
std::atomic<bool> g_installed{false};

// --- Hook bodies ------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain3* self,
                                       UINT sync_interval, UINT flags) {
    // overlay.cpp lazy-inits on first usable call and draws every frame.
    overlay_on_present(self, g_captured_queue.load());
    return g_orig_present(self, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hook_resize_buffers(
    IDXGISwapChain3* self, UINT buffer_count, UINT width, UINT height,
    DXGI_FORMAT new_format, UINT swap_chain_flags) {
    overlay_on_resize(self, buffer_count, width, height);
    HRESULT hr = g_orig_resize_buffers(self, buffer_count, width, height,
                                       new_format, swap_chain_flags);
    overlay_after_resize(self);
    return hr;
}

void STDMETHODCALLTYPE hook_execute_command_lists(
    ID3D12CommandQueue* self, UINT num_command_lists,
    ID3D12CommandList* const* command_lists) {
    // Capture the first DIRECT-type queue we see; that's the one the game
    // uses to submit work to the swap chain, and the same one ImGui will
    // need to submit its draw commands on.
    if (g_captured_queue.load() == nullptr) {
        D3D12_COMMAND_QUEUE_DESC desc = self->GetDesc();
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_captured_queue.store(self);
            logf("d3d12: captured DIRECT queue %p", static_cast<void*>(self));
        }
    }
    g_orig_execute_command_lists(self, num_command_lists, command_lists);
}

// --- vtable discovery -------------------------------------------------------

// Wrap a COM pointer so we don't forget to Release().
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
    // Hidden message-only window. We never show it, so style doesn't
    // matter beyond "not invalid".
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"farevermod_dummy";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        logf("d3d12: RegisterClassExW failed (%lu)", GetLastError());
        return false;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        logf("d3d12: CreateWindow failed (%lu)", GetLastError());
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    bool ok = false;
    do {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                      reinterpret_cast<void**>(&factory)))) {
            logf("d3d12: CreateDXGIFactory1 failed");
            break;
        }

        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                     __uuidof(ID3D12Device),
                                     reinterpret_cast<void**>(&device)))) {
            logf("d3d12: D3D12CreateDevice failed");
            break;
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(
                &queue_desc, __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(&queue)))) {
            logf("d3d12: CreateCommandQueue failed");
            break;
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width       = 100;
        scd.Height      = 100;
        scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;

        ComPtr<IDXGISwapChain1> swap_chain;
        if (FAILED(factory->CreateSwapChainForHwnd(
                queue.p, hwnd, &scd, nullptr, nullptr, &swap_chain))) {
            logf("d3d12: CreateSwapChainForHwnd failed");
            break;
        }

        // The vtable layout we care about (Present at slot 8, ResizeBuffers
        // at slot 13) belongs to IDXGISwapChain, which IDXGISwapChain1
        // inherits unchanged, so we can read directly off swap_chain.
        void** swap_chain_vt =
            *reinterpret_cast<void***>(swap_chain.p);
        void** queue_vt =
            *reinterpret_cast<void***>(queue.p);

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

// --- Public API -------------------------------------------------------------

bool d3d12_hook_install() {
    if (g_installed.exchange(true)) {
        return true;  // already installed
    }

    void* present_ptr        = nullptr;
    void* resize_buffers_ptr = nullptr;
    void* exec_ptr           = nullptr;
    if (!discover_vtable_pointers(&present_ptr, &resize_buffers_ptr,
                                  &exec_ptr)) {
        g_installed.store(false);
        return false;
    }
    logf("d3d12: vtable Present=%p ResizeBuffers=%p ExecuteCommandLists=%p",
         present_ptr, resize_buffers_ptr, exec_ptr);

    if (MH_Initialize() != MH_OK) {
        logf("d3d12: MH_Initialize failed");
        g_installed.store(false);
        return false;
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
    if (!g_installed.exchange(false)) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_orig_present              = nullptr;
    g_orig_resize_buffers       = nullptr;
    g_orig_execute_command_lists = nullptr;
    g_captured_queue.store(nullptr);
    log_line("d3d12: hooks uninstalled");
}

ID3D12CommandQueue* d3d12_captured_queue() {
    return g_captured_queue.load();
}

}  // namespace farevermod
