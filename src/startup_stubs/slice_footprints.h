#pragma once

#include <d3d12.h>
#include <cstdint>

namespace x2w_startup {
// GetCopyableFootprints orders a single NV12 image as Y followed by UV.
// Keep this layout identical in the XG writer and the desktop draw uploader.
struct SliceFootprints {
    static constexpr UINT max_mips = 15;
    UINT planes;
    UINT count;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed[max_mips * 2]{};
    UINT rows[max_mips * 2]{};
    UINT64 row_bytes[max_mips * 2]{};
    UINT64 size = UINT64_MAX;

    static bool supports(const D3D12_RESOURCE_DESC& desc)
    {
        return desc.Format != DXGI_FORMAT_NV12 ||
               (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
                desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && desc.Width && desc.Height &&
                (desc.Width & 1) == 0 && (desc.Height & 1) == 0);
    }

    SliceFootprints(ID3D12Device* device, D3D12_RESOURCE_DESC desc, UINT mips)
        : planes(desc.Format == DXGI_FORMAT_NV12 ? 2u : 1u), count(mips * planes)
    {
        if (device && supports(desc) && mips && mips <= max_mips)
            device->GetCopyableFootprints(&desc, 0, count, 0, placed, rows, row_bytes, &size);
    }

    UINT64 bytes(UINT i) const
    {
        return UINT64{placed[i].Footprint.RowPitch} * (UINT64{placed[i].Footprint.Depth} * rows[i] - 1) + row_bytes[i];
    }

    void upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, ID3D12Resource* upload_buffer,
                UINT mips, UINT slices, UINT slice, UINT i, UINT64 offset) const
    {
        D3D12_TEXTURE_COPY_LOCATION from{upload_buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        from.PlacedFootprint = {offset, placed[i].Footprint};
        D3D12_TEXTURE_COPY_LOCATION to{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        to.SubresourceIndex = i % mips + slice * mips + (i / mips) * slices * mips;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
};
}  // namespace x2w_startup
