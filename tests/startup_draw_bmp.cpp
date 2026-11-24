#include "../src/startup_stubs/draw_bmp.h"

#include <array>
#include <cstring>

// A single nonblack texel falls between the old 4x sample points. Row padding is
// nonzero so accidentally interpreting the aligned pitch as image width is visible.
static bool check(DXGI_FORMAT format, bool bgra)
{
    constexpr UINT width = 7, height = 3, pitch = 256;
    std::array<unsigned char, pitch * height> rgba;
    rgba.fill(0xee);
    for (UINT y = 0; y < height; ++y) std::memset(rgba.data() + y * pitch, 0, width * 4);
    rgba[5 * 4] = bgra ? 19 : 231;
    rgba[5 * 4 + 1] = 73;
    rgba[5 * 4 + 2] = bgra ? 231 : 19;
    FILE* output = std::tmpfile();
    if (!output) return false;
    bool ok = x2w_draw_bmp::write(output, rgba.data(), width, height, pitch, format);
    std::rewind(output);
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    ok = ok && std::fread(&header, sizeof(header), 1, output) == 1 &&
         std::fread(&info, sizeof(info), 1, output) == 1 && header.bfType == 0x4d42 &&
         header.bfOffBits == 54 && header.bfSize == 54 + width * height * 4 &&
         info.biWidth == width && info.biHeight == height && info.biBitCount == 32;
    for (UINT y = 0; y < height && ok; ++y) {
        for (UINT x = 0; x < width && ok; ++x) {
            std::array<unsigned char, 4> pixel{};
            const std::array<unsigned char, 4> expected = y == height - 1 && x == 5
                ? std::array<unsigned char, 4>{19, 73, 231, 255}
                : std::array<unsigned char, 4>{0, 0, 0, 255};
            ok = std::fread(pixel.data(), 1, pixel.size(), output) == pixel.size() && pixel == expected;
        }
    }
    ok = ok && std::fgetc(output) == EOF &&
         !x2w_draw_bmp::write(output, rgba.data(), width, height, 16, format) &&
         !x2w_draw_bmp::write(output, rgba.data(), width, height, pitch, DXGI_FORMAT_R32_FLOAT);
    std::fclose(output);
    return ok;
}

int main()
{
    return check(DXGI_FORMAT_R8G8B8A8_UNORM, false) && check(DXGI_FORMAT_B8G8R8A8_UNORM, true) &&
           check(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false) && check(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true) ? 0 : 1;
}
