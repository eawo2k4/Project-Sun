#include "retro/PixelConvert.h"

#include <array>
#include <bit>
#include <cstring>

namespace retro {
namespace {

// Extracts the channel selected by `mask` from `pixel` and expands it to 8
// bits by replicating its top bits (so 5-bit 31 becomes 255, not 248).
struct Channel {
    uint32_t mask = 0;
    int shift = 0;
    int bits = 0;

    explicit Channel(uint32_t m) : mask(m) {
        if (m) {
            shift = std::countr_zero(m);
            bits = std::popcount(m);
        }
    }

    uint32_t Expand(uint32_t pixel) const {
        if (!bits) return 0;
        uint32_t v = (pixel & mask) >> shift;
        if (bits >= 8) return v >> (bits - 8);
        uint32_t out = v << (8 - bits);
        for (int filled = bits; filled < 8; filled += bits) out |= out >> bits;
        return out & 0xFF;
    }
};

}  // namespace

PixelFormat FormatForDepth(uint32_t depth) {
    switch (depth) {
    case 8: return {8, true, 0, 0, 0};
    case 15: return {16, false, 0x7C00, 0x03E0, 0x001F};
    case 16: return {16, false, 0xF800, 0x07E0, 0x001F};
    case 24: return {24, false, 0xFF0000, 0x00FF00, 0x0000FF};
    case 32: return {32, false, 0xFF0000, 0x00FF00, 0x0000FF};
    default: return {};
    }
}

bool ConvertToBgra32(const uint8_t* src, int32_t srcPitch, const PixelFormat& f,
                     const uint32_t* palette, int32_t width, int32_t height, uint32_t* dst,
                     int32_t dstPitch) {
    if (!src || !dst || width <= 0 || height <= 0) return false;

    if (f.palettized) {
        if (f.bitsPerPixel != 8 || !palette) return false;
        for (int32_t y = 0; y < height; ++y) {
            const uint8_t* s = src + static_cast<ptrdiff_t>(y) * srcPitch;
            uint32_t* d = dst + static_cast<ptrdiff_t>(y) * dstPitch;
            for (int32_t x = 0; x < width; ++x) d[x] = palette[s[x]];
        }
        return true;
    }

    const Channel r(f.rMask), g(f.gMask), b(f.bMask);
    const bool fast32 = f.bitsPerPixel == 32 && f.rMask == 0xFF0000 && f.gMask == 0xFF00 &&
                        f.bMask == 0xFF;
    for (int32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<ptrdiff_t>(y) * srcPitch;
        uint32_t* d = dst + static_cast<ptrdiff_t>(y) * dstPitch;
        switch (f.bitsPerPixel) {
        case 16:
            for (int32_t x = 0; x < width; ++x) {
                uint16_t p;
                std::memcpy(&p, s + x * 2, 2);
                d[x] = 0xFF000000u | (r.Expand(p) << 16) | (g.Expand(p) << 8) | b.Expand(p);
            }
            break;
        case 24:
            for (int32_t x = 0; x < width; ++x) {
                const uint32_t p = s[x * 3] | (s[x * 3 + 1] << 8) | (s[x * 3 + 2] << 16);
                d[x] = 0xFF000000u | (r.Expand(p) << 16) | (g.Expand(p) << 8) | b.Expand(p);
            }
            break;
        case 32:
            if (fast32) {
                for (int32_t x = 0; x < width; ++x) {
                    uint32_t p;
                    std::memcpy(&p, s + x * 4, 4);
                    d[x] = 0xFF000000u | p;
                }
            } else {
                for (int32_t x = 0; x < width; ++x) {
                    uint32_t p;
                    std::memcpy(&p, s + x * 4, 4);
                    d[x] = 0xFF000000u | (r.Expand(p) << 16) | (g.Expand(p) << 8) | b.Expand(p);
                }
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

uint32_t Crc32(const void* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace retro
