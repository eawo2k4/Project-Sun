// Unit tests for retro/PixelConvert.h: legacy surface formats -> 32-bit BGRA.

#include <cstring>
#include <vector>

#include "Check.h"
#include "retro/PixelConvert.h"

using namespace retro;

namespace {

uint32_t Convert1(const PixelFormat& f, const void* pixel, const uint32_t* palette = nullptr) {
    uint8_t src[4] = {};
    std::memcpy(src, pixel, (f.bitsPerPixel + 7) / 8);
    uint32_t out = 0;
    CHECK(ConvertToBgra32(src, 4, f, palette, 1, 1, &out, 1));
    return out;
}

uint32_t From16(const PixelFormat& f, uint16_t v) { return Convert1(f, &v); }

void TestFormatsForDepth() {
    CHECK((FormatForDepth(8) == PixelFormat{8, true, 0, 0, 0}));
    CHECK((FormatForDepth(15) == PixelFormat{16, false, 0x7C00, 0x03E0, 0x001F}));
    CHECK((FormatForDepth(16) == PixelFormat{16, false, 0xF800, 0x07E0, 0x001F}));
    CHECK((FormatForDepth(24) == PixelFormat{24, false, 0xFF0000, 0xFF00, 0xFF}));
    CHECK((FormatForDepth(32) == PixelFormat{32, false, 0xFF0000, 0xFF00, 0xFF}));
    CHECK(FormatForDepth(12).bitsPerPixel == 0);
}

void TestRgb565() {
    const PixelFormat f = FormatForDepth(16);
    CHECK(From16(f, 0x0000) == 0xFF000000);
    CHECK(From16(f, 0xFFFF) == 0xFFFFFFFF);  // full channels expand to 255, not 248/252
    CHECK(From16(f, 0xF800) == 0xFFFF0000);
    CHECK(From16(f, 0x07E0) == 0xFF00FF00);
    CHECK(From16(f, 0x001F) == 0xFF0000FF);
    // Mid-grey: r=16/31, g=32/63, b=16/31 -> bit replication gives 132, 130, 132.
    CHECK(From16(f, 0x8410) == 0xFF848284);
}

void TestRgb555() {
    const PixelFormat f = FormatForDepth(15);
    CHECK(From16(f, 0x7FFF) == 0xFFFFFFFF);
    CHECK(From16(f, 0x7C00) == 0xFFFF0000);
    CHECK(From16(f, 0x03E0) == 0xFF00FF00);
    CHECK(From16(f, 0x8000) == 0xFF000000);  // the unused top bit is ignored
}

void TestChannelExpansionIsMonotonic() {
    // Every 5- and 6-bit level maps to a distinct, increasing 8-bit level.
    const PixelFormat f = FormatForDepth(16);
    uint32_t previous = 0;
    for (uint16_t r = 0; r < 32; ++r) {
        const uint32_t red = (From16(f, static_cast<uint16_t>(r << 11)) >> 16) & 0xFF;
        if (r > 0) CHECK(red > previous);
        previous = red;
    }
    CHECK(previous == 255);
    for (uint16_t g = 0; g < 64; ++g) {
        const uint32_t green = (From16(f, static_cast<uint16_t>(g << 5)) >> 8) & 0xFF;
        if (g > 0) CHECK(green > previous || g == 0);
        previous = green;
    }
    CHECK(previous == 255);
}

void TestRgb24And32() {
    const uint8_t bgr[3] = {0x11, 0x22, 0x33};  // little-endian: blue first
    CHECK(Convert1(FormatForDepth(24), bgr) == 0xFF332211);
    const uint32_t xrgb = 0x00ABCDEF;
    CHECK(Convert1(FormatForDepth(32), &xrgb) == 0xFFABCDEF);
    // A 32-bit format with other masks (XBGR) goes through the generic path.
    const PixelFormat xbgr{32, false, 0x0000FF, 0x00FF00, 0xFF0000};
    CHECK(Convert1(xbgr, &xrgb) == 0xFFEFCDAB);
}

void TestPalettized() {
    uint32_t palette[256];
    for (int i = 0; i < 256; ++i) palette[i] = PaletteToBgra(uint8_t(i), uint8_t(255 - i), 7);
    CHECK(PaletteToBgra(1, 2, 3) == 0xFF010203);

    // 3x2 image with a padded pitch of 8 bytes.
    const uint8_t src[16] = {0, 1, 2, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE,
                             253, 254, 255, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    uint32_t dst[2 * 4];
    std::memset(dst, 0x5A, sizeof(dst));
    CHECK(ConvertToBgra32(src, 8, FormatForDepth(8), palette, 3, 2, dst, 4));
    CHECK(dst[0] == palette[0] && dst[1] == palette[1] && dst[2] == palette[2]);
    CHECK(dst[4] == palette[253] && dst[5] == palette[254] && dst[6] == palette[255]);
    CHECK(dst[3] == 0x5A5A5A5A && dst[7] == 0x5A5A5A5A);  // destination padding untouched
}

void TestPitchAndSizes() {
    // 16-bit, 2x2 with a pitch of 6 bytes (one padding pixel per row).
    const uint16_t src[6] = {0xF800, 0x07E0, 0x1234, 0x001F, 0xFFFF, 0x1234};
    uint32_t dst[4];
    CHECK(ConvertToBgra32(reinterpret_cast<const uint8_t*>(src), 6, FormatForDepth(16), nullptr, 2,
                          2, dst, 2));
    CHECK(dst[0] == 0xFFFF0000 && dst[1] == 0xFF00FF00 && dst[2] == 0xFF0000FF &&
          dst[3] == 0xFFFFFFFF);
}

void TestRejectsBadInput() {
    uint8_t src[4] = {};
    uint32_t dst[1];
    CHECK(!ConvertToBgra32(src, 4, FormatForDepth(8), nullptr, 1, 1, dst, 1));  // no palette
    CHECK(!ConvertToBgra32(src, 4, PixelFormat{12, false, 0xF00, 0xF0, 0xF}, nullptr, 1, 1, dst, 1));
    CHECK(!ConvertToBgra32(nullptr, 4, FormatForDepth(32), nullptr, 1, 1, dst, 1));
    CHECK(!ConvertToBgra32(src, 4, FormatForDepth(32), nullptr, 0, 1, dst, 1));
}

void TestCrc32() {
    CHECK(Crc32("123456789", 9) == 0xCBF43926);  // the standard check value
    CHECK(Crc32("", 0) == 0);
    const std::vector<uint32_t> a(640 * 480, 0xFF000000), b(640 * 480, 0xFF000001);
    CHECK(Crc32(a.data(), a.size() * 4) != Crc32(b.data(), b.size() * 4));
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"FormatsForDepth", TestFormatsForDepth},
        {"Rgb565", TestRgb565},
        {"Rgb555", TestRgb555},
        {"ChannelExpansionIsMonotonic", TestChannelExpansionIsMonotonic},
        {"Rgb24And32", TestRgb24And32},
        {"Palettized", TestPalettized},
        {"PitchAndSizes", TestPitchAndSizes},
        {"RejectsBadInput", TestRejectsBadInput},
        {"Crc32", TestCrc32},
    };
    return test::RunAll(cases);
}
