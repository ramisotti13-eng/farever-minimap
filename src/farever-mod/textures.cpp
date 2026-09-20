// PNG -> D3D12 texture, with synchronous upload via a transient queue.
// Port of src/minimap-dll/textures.cpp; namespace farever, otherwise
// unchanged. See that file's comments for the per-step rationale.

#include "textures.h"
#include "log.h"

#include <windows.h>
#include <wincodec.h>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <string>

#pragma comment(lib, "windowscodecs.lib")

namespace farever {
namespace {

struct ComScope {
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) CoUninitialize(); }
};

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

bool decode_to_rgba(const wchar_t* path, std::vector<uint8_t>* out_pixels,
                    UINT* out_w, UINT* out_h) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        logf("textures: CoCreateInstance(WICImagingFactory) failed");
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder))) {
        logf("textures: CreateDecoderFromFilename failed for %ls", path);
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        logf("textures: GetFrame failed");
        return false;
    }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h))) {
        logf("textures: frame->GetSize failed");
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) {
        logf("textures: CreateFormatConverter failed");
        return false;
    }
    if (FAILED(converter->Initialize(
            frame, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeMedianCut))) {
        logf("textures: converter->Initialize failed");
        return false;
    }
    const UINT stride = w * 4;
    out_pixels->resize(static_cast<size_t>(stride) * h);
    if (FAILED(converter->CopyPixels(nullptr, stride,
                                     static_cast<UINT>(out_pixels->size()),
                                     out_pixels->data()))) {
        logf("textures: CopyPixels failed");
        return false;
    }
    *out_w = w;
    *out_h = h;
    return true;
}

UINT64 align_up(UINT64 value, UINT64 align) {
    return (value + align - 1) & ~(align - 1);
}

bool upload_rgba8(ID3D12Device* device,
                  const std::vector<uint8_t>& pixels, UINT w, UINT h,
                  ID3D12Resource** out_resource) {
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width              = w;
    tex_desc.Height             = h;
    tex_desc.DepthOrArraySize   = 1;
    tex_desc.MipLevels          = 1;
    tex_desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count   = 1;
    tex_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ID3D12Resource* dst = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&dst)))) {
        logf("textures: CreateCommittedResource(DEFAULT) failed");
        return false;
    }

    const UINT64 src_row_pitch = static_cast<UINT64>(w) * 4;
    const UINT64 dst_row_pitch = align_up(
        src_row_pitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 upload_size = dst_row_pitch * h;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width              = upload_size;
    buf_desc.Height             = 1;
    buf_desc.DepthOrArraySize   = 1;
    buf_desc.MipLevels          = 1;
    buf_desc.Format             = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count   = 1;
    buf_desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload)))) {
        logf("textures: CreateCommittedResource(UPLOAD) failed");
        dst->Release();
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) {
        logf("textures: upload->Map failed");
        dst->Release();
        return false;
    }
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped) + y * dst_row_pitch,
                    pixels.data() + y * src_row_pitch,
                    src_row_pitch);
    }
    upload->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    ComPtr<ID3D12CommandQueue> q;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&q)))) {
        logf("textures: CreateCommandQueue failed");
        dst->Release();
        return false;
    }
    ComPtr<ID3D12CommandAllocator> alloc;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) {
        logf("textures: CreateCommandAllocator failed");
        dst->Release();
        return false;
    }
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.p, nullptr,
            IID_PPV_ARGS(&cl)))) {
        logf("textures: CreateCommandList failed");
        dst->Release();
        return false;
    }
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&fence)))) {
        logf("textures: CreateFence failed");
        dst->Release();
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload.p;
    src_loc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width    = w;
    src_loc.PlacedFootprint.Footprint.Height   = h;
    src_loc.PlacedFootprint.Footprint.Depth    = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(dst_row_pitch);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource         = dst;
    dst_loc.Type              = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex  = 0;

    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = dst;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);

    cl->Close();
    ID3D12CommandList* lists[] = {cl.p};
    q->ExecuteCommandLists(1, lists);
    q->Signal(fence.p, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, event);
        // Bounded wait, never an infinite one.
        // The texture is small (a single 96/256px atlas) and on the
        // GPU's own queue, so 2 s is comfortably above worst-case GPU
        // queue depth. If the GPU is genuinely stuck, we'd rather drop
        // this load and surface a black icon than freeze Present.
        DWORD wr = WaitForSingleObject(event, 2000);
        if (wr != WAIT_OBJECT_0) {
            logf("textures: fence wait timed out (wr=%lu) - dropping load",
                 static_cast<unsigned long>(wr));
            CloseHandle(event);
            dst->Release();
            return false;
        }
    }
    CloseHandle(event);

    *out_resource = dst;
    return true;
}

}  // namespace

bool load_texture_from_pixels(ID3D12Device* device,
                              ID3D12DescriptorHeap* srv_heap, UINT slot,
                              const std::vector<uint8_t>& pixels,
                              UINT w, UINT h, LoadedTexture* out) {
    ID3D12Resource* resource = nullptr;
    if (!upload_rgba8(device, pixels, w, h, &resource)) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;

    const UINT inc = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += slot * inc;
    gpu.ptr += slot * inc;
    device->CreateShaderResourceView(resource, &srv, cpu);

    out->resource = resource;
    out->srv_gpu  = gpu;
    out->width    = w;
    out->height   = h;
    logf("textures: SRV at slot %u (gpu=0x%llx)",
         slot, static_cast<unsigned long long>(gpu.ptr));
    return true;
}

bool load_texture_from_file(ID3D12Device* device,
                            ID3D12DescriptorHeap* srv_heap, UINT slot,
                            const wchar_t* path, LoadedTexture* out) {
    static ComScope com;

    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!decode_to_rgba(path, &pixels, &w, &h)) return false;
    logf("textures: decoded %ls: %u x %u (%zu bytes)",
         path, w, h, pixels.size());
    return load_texture_from_pixels(device, srv_heap, slot, pixels, w, h, out);
}

// --- minimap mosaic background pre-decode ---------------------------
struct MosaicPreload {
    std::thread          thread;
    std::wstring         path;
    std::vector<uint8_t> pixels;
    UINT                 w = 0, h = 0;
    bool                 ok = false;
    std::atomic<bool>    started{false};
};
MosaicPreload g_mosaic_preload;

void textures_preload_mosaic(const wchar_t* path) {
    bool expected = false;
    if (!g_mosaic_preload.started.compare_exchange_strong(expected, true))
        return;   // only the first call starts the decode
    g_mosaic_preload.path = path;
    g_mosaic_preload.thread = std::thread([] {
        ComScope com;   // WIC needs COM initialised on this thread
        MosaicPreload& m = g_mosaic_preload;
        m.ok = decode_to_rgba(m.path.c_str(), &m.pixels, &m.w, &m.h);
        if (m.ok)
            logf("textures: mosaic pre-decoded %ls: %u x %u (%zu bytes)",
                 m.path.c_str(), m.w, m.h, m.pixels.size());
        else
            logf("textures: mosaic pre-decode failed for %ls",
                 m.path.c_str());
    });
}

bool textures_take_mosaic(std::vector<uint8_t>* out_pixels,
                          UINT* out_w, UINT* out_h) {
    if (!g_mosaic_preload.started.load()) return false;
    if (g_mosaic_preload.thread.joinable()) g_mosaic_preload.thread.join();
    if (!g_mosaic_preload.ok) return false;
    *out_pixels = std::move(g_mosaic_preload.pixels);
    *out_w = g_mosaic_preload.w;
    *out_h = g_mosaic_preload.h;
    g_mosaic_preload.pixels.clear();
    g_mosaic_preload.pixels.shrink_to_fit();
    return true;
}

void release_texture(LoadedTexture* tex) {
    if (tex->resource) {
        tex->resource->Release();
        tex->resource = nullptr;
    }
    tex->srv_gpu.ptr = 0;
    tex->width = tex->height = 0;
}

}  // namespace farever
