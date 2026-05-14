#pragma once

#include <d3d12.h>

namespace farevermod {

struct LoadedTexture {
    ID3D12Resource*             resource = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    UINT                        width    = 0;
    UINT                        height   = 0;
};

// Load a PNG (any WIC-supported format) into a D3D12 texture, allocate
// an SRV at `slot` of `srv_heap` (which must be SHADER_VISIBLE), and
// synchronously upload via a transient queue. Returns false on any
// failure. On success, populates `out` and the caller owns the resource.
bool load_texture_from_file(ID3D12Device* device,
                            ID3D12DescriptorHeap* srv_heap, UINT slot,
                            const wchar_t* path, LoadedTexture* out);

void release_texture(LoadedTexture* tex);

}  // namespace farevermod
