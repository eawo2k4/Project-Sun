#pragma once

// Converts vintage surface formats (8-bit palettized, 15/16-bit RGB,
// 24/32-bit RGB) to 32-bit BGRA for presentation on a modern desktop.
// Pure functions: unit-tested, and used by the DirectDraw presenter.

#include <cstddef>
#include <cstdint>

namespace retro {

struct PixelFormat {
    uint32_t bitsPerPixel = 0;  // 8, 16, 24 or 32 (15-bit 555 is stored in 16)
    bool palettized = false;    // 8-bit indexed
    uint32_t rMask = 0;
    uint32_t gMask = 0;
    uint32_t bMask = 0;

    bool operator==(const PixelFormat&) const = default;
};

// The format a display mode of `depth` bits implies: 8 -> palettized,
// 15 -> RGB555, 16 -> RGB565, 24 -> RGB888, 32 -> XRGB8888.
PixelFormat FormatForDepth(uint32_t depth);

// Palette entry (PALETTEENTRY order: red, green, blue, flags) -> BGRA dword.
constexpr uint32_t PaletteToBgra(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xFF000000u | (static_cast<uint32_t>(red) << 16) | (static_cast<uint32_t>(green) << 8) |
           blue;
}

// Converts a w x h image. `srcPitch` is in bytes, `dstPitch` in pixels.
// `palette` (256 BGRA entries) is required for palettized formats. Returns
// false for unsupported formats.
bool ConvertToBgra32(const uint8_t* src, int32_t srcPitch, const PixelFormat& format,
                     const uint32_t* palette, int32_t width, int32_t height, uint32_t* dst,
                     int32_t dstPitch);

// CRC-32 (IEEE 802.3), used to fingerprint presented frames in diagnostics.
uint32_t Crc32(const void* data, size_t size);

}  // namespace retro
