#pragma once

#include <d3d12.h>
#include <cstdint>
#include <vector>

namespace farever {

struct LoadedTexture {
    ID3D12Resource*             resource = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    UINT                        width    = 0;
    UINT                        height   = 0;
};

// PNG/WIC → D3D12 texture upload at SRV heap `slot`. Returns false on
// any failure (file missing, decode error, allocation failure). On
// success the caller owns the resource; release with release_texture.
bool load_texture_from_file(ID3D12Device* device,
                            ID3D12DescriptorHeap* srv_heap, UINT slot,
                            const wchar_t* path, LoadedTexture* out);

// Upload already-decoded RGBA8 pixels to SRV heap `slot` (the upload
// half of load_texture_from_file).
bool load_texture_from_pixels(ID3D12Device* device,
                              ID3D12DescriptorHeap* srv_heap, UINT slot,
                              const std::vector<uint8_t>& pixels,
                              UINT w, UINT h, LoadedTexture* out);

// Start decoding the minimap mosaic PNG on a background thread now, so
// the multi-second WIC decode overlaps the game's loading screen instead
// of stalling the overlay's first frame. Decode needs no D3D12 device.
// Idempotent (only the first call starts a thread).
void textures_preload_mosaic(const wchar_t* path);

// Consume the pre-decoded mosaic: blocks until the background decode is
// done, then moves the pixels into out_pixels. Returns false if no
// preload was started or the decode failed (caller decodes synchronously
// as a fallback).
bool textures_take_mosaic(std::vector<uint8_t>* out_pixels,
                          UINT* out_w, UINT* out_h);

void release_texture(LoadedTexture* tex);

}  // namespace farever
