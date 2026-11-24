#pragma once

#include <dxgiformat.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace x2w_draw_bmp {

inline bool supported(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// D3D12 copy footprints include row padding. BMP input here starts at the footprint offset.
inline bool write(FILE* file, const unsigned char* pixels, UINT width, UINT height,
                  UINT row_pitch, DXGI_FORMAT format)
{
    const std::uint64_t stride = std::uint64_t{width} * 4;
    constexpr DWORD header_bytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    if (!file || !pixels || !supported(format) || !width || !height || stride > row_pitch ||
        stride * height > MAXDWORD - header_bytes) return false;

    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfSize = static_cast<DWORD>(header_bytes + stride * height);
    header.bfOffBits = header_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biSizeImage = header.bfSize - header_bytes;
    if (std::fwrite(&header, sizeof(header), 1, file) != 1 ||
        std::fwrite(&info, sizeof(info), 1, file) != 1) return false;

    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::vector<unsigned char> row(static_cast<size_t>(stride));
    for (UINT y = height; y > 0; --y) {
        const auto* source = pixels + size_t{y - 1} * row_pitch;
        for (size_t x = 0; x < row.size(); x += 4) {
            row[x] = source[x + (rgba ? 2 : 0)];
            row[x + 1] = source[x + 1];
            row[x + 2] = source[x + (rgba ? 0 : 2)];
            row[x + 3] = 255;
        }
        if (std::fwrite(row.data(), 1, row.size(), file) != row.size()) return false;
    }
    return true;
}
}
