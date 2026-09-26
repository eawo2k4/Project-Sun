// Unit tests for the display sandbox geometry (retro/DisplayMath.h).

#include <windows.h>

#include <algorithm>
#include <cstdlib>

#include "Check.h"
#include "retro/DisplayMath.h"

using namespace retro;

namespace {

const ScalingOptions kDefault{};
const Rect k4K{0, 0, 3840, 2160};
const Rect k1080{0, 0, 1920, 1080};

// --- Modes -------------------------------------------------------------------------

void TestModeRequests() {
    const DisplayMode desktop{3840, 2160, 32, 144};
    DisplayMode out;

    ModeRequest full;
    full.hasWidth = full.hasHeight = full.hasBitsPerPixel = true;
    full.width = 640;
    full.height = 480;
    full.bitsPerPixel = 16;
    CHECK(ResolveModeRequest(full, desktop, out));
    CHECK((out == DisplayMode{640, 480, 16, 144}));  // refresh not requested: kept

    // Depth-only change keeps the resolution.
    ModeRequest depth;
    depth.hasBitsPerPixel = true;
    depth.bitsPerPixel = 8;
    CHECK(ResolveModeRequest(depth, DisplayMode{800, 600, 16, 60}, out));
    CHECK((out == DisplayMode{800, 600, 8, 60}));

    // Refresh 0/1 means "default".
    ModeRequest refresh = full;
    refresh.hasRefresh = true;
    refresh.refreshHz = 1;
    CHECK(ResolveModeRequest(refresh, desktop, out) && out.refreshHz == 144);
    refresh.refreshHz = 75;
    CHECK(ResolveModeRequest(refresh, desktop, out) && out.refreshHz == 75);

    // Nonsense is rejected (DISP_CHANGE_BADMODE).
    ModeRequest bad = full;
    bad.bitsPerPixel = 12;
    CHECK(!ResolveModeRequest(bad, desktop, out));
    bad = full;
    bad.width = 13;
    CHECK(!ResolveModeRequest(bad, desktop, out));
    bad = full;
    bad.width = 100000;
    CHECK(!ResolveModeRequest(bad, desktop, out));
}

void TestClassicModeList() {
    const auto modes = ClassicModeList({3840, 2160});
    auto has = [&](uint32_t w, uint32_t h, uint32_t bpp) {
        return std::find(modes.begin(), modes.end(), DisplayMode{w, h, bpp, 60}) != modes.end();
    };
    CHECK(has(640, 480, 8) && has(640, 480, 16) && has(640, 480, 32));
    CHECK(has(800, 600, 16) && has(1024, 768, 16) && has(320, 200, 8) && has(1600, 1200, 32));

    // Sorted by depth, then width, then height (Windows' own order).
    for (size_t i = 1; i < modes.size(); ++i) {
        const DisplayMode& a = modes[i - 1];
        const DisplayMode& b = modes[i];
        const bool ordered =
            a.bitsPerPixel < b.bitsPerPixel ||
            (a.bitsPerPixel == b.bitsPerPixel &&
             (a.width < b.width || (a.width == b.width && a.height < b.height)));
        CHECK(ordered);
    }

    // Only modes that fit the monitor.
    const auto small = ClassicModeList({1024, 768});
    for (const DisplayMode& m : small) CHECK(m.width <= 1024 && m.height <= 768);
    CHECK(std::find(small.begin(), small.end(), DisplayMode{1024, 768, 16, 60}) != small.end());
}

// --- Viewport planning -------------------------------------------------------------

void TestAspect() {
    CHECK((DisplayAspectFor({640, 480}, kDefault) == Size{4, 3}));
    CHECK((DisplayAspectFor({1280, 1024}, kDefault) == Size{5, 4}));   // square pixels
    CHECK((DisplayAspectFor({320, 200}, kDefault) == Size{4, 3}));     // CRT double-scan
    CHECK((DisplayAspectFor({640, 400}, kDefault) == Size{4, 3}));
    CHECK((DisplayAspectFor({1680, 1050}, kDefault) == Size{8, 5}));   // real widescreen
    ScalingOptions raw;
    raw.classicAspect = false;
    CHECK((DisplayAspectFor({320, 200}, raw) == Size{8, 5}));
}

void TestIntegerViewports() {
    // 640x480 on 4K: 4x, pillarboxed and letterboxed.
    Viewport vp = PlanViewport({640, 480}, k4K, kDefault);
    CHECK(vp.integerScale == 4);
    CHECK((vp.rect == Rect{640, 120, 3200, 2040}));

    // 800x600 on 1080p: only 1x fits.
    vp = PlanViewport({800, 600}, k1080, kDefault);
    CHECK(vp.integerScale == 1);
    CHECK((vp.rect == Rect{560, 240, 1360, 840}));

    // 320x240 on 1080p: 4x.
    vp = PlanViewport({320, 240}, k1080, kDefault);
    CHECK(vp.integerScale == 4 && vp.rect.Width() == 1280 && vp.rect.Height() == 960);

    // Monitor to the left of the primary (negative coordinates).
    vp = PlanViewport({640, 480}, Rect{-1920, 0, 0, 1080}, kDefault);
    CHECK((vp.rect == Rect{-1600, 60, -320, 1020}));
}

void TestFractionalViewports() {
    // 1600x1200 doesn't fit 1080 lines: fractional fit, aspect kept.
    Viewport vp = PlanViewport({1600, 1200}, k1080, kDefault);
    CHECK(vp.integerScale == 0);
    CHECK(vp.rect.Height() == 1080 && vp.rect.Width() == 1440);

    // 1280x1024 (5:4, square pixels) still fits at 1x.
    vp = PlanViewport({1280, 1024}, k1080, kDefault);
    CHECK(vp.integerScale == 1 && vp.rect.Width() == 1280 && vp.rect.Height() == 1024);

    // Integer scaling off: fill the height at 4:3.
    ScalingOptions fill;
    fill.integerScaling = false;
    vp = PlanViewport({640, 480}, k1080, fill);
    CHECK(vp.integerScale == 0);
    CHECK((vp.rect == Rect{240, 0, 1680, 1080}));

    // Portrait monitor: fit the width.
    vp = PlanViewport({640, 480}, Rect{0, 0, 1080, 1920}, fill);
    CHECK(vp.rect.Width() == 1080 && vp.rect.Height() == 810);
}

void TestAspectCorrectedViewports() {
    // 320x200 at 4:3 with whole-number line scaling.
    Viewport vp = PlanViewport({320, 200}, k1080, kDefault);
    CHECK(vp.integerScale == 5);
    CHECK(vp.rect.Height() == 1000 && vp.rect.Width() == 1333);

    vp = PlanViewport({320, 200}, k4K, kDefault);
    CHECK(vp.integerScale == 10);
    CHECK(vp.rect.Height() == 2000 && vp.rect.Width() == 2667);

    // Never exceeds the area, whatever the inputs.
    const Size modes[] = {{320, 200}, {640, 400}, {640, 480}, {800, 600}, {1024, 768},
                          {1280, 1024}, {1600, 1200}, {1680, 1050}};
    const Rect areas[] = {k4K, k1080, {0, 0, 1366, 768}, {0, 0, 1280, 1024}, {100, 50, 900, 650}};
    for (Size m : modes) {
        for (const Rect& a : areas) {
            for (bool integer : {true, false}) {
                ScalingOptions o;
                o.integerScaling = integer;
                const Viewport v = PlanViewport(m, a, o);
                CHECK(a.Contains(v.rect));
                CHECK(v.rect.Width() > 0 && v.rect.Height() > 0);
                // Centred (within a pixel of rounding).
                CHECK(std::abs((v.rect.left - a.left) - (a.right - v.rect.right)) <= 1);
                CHECK(std::abs((v.rect.top - a.top) - (a.bottom - v.rect.bottom)) <= 1);
            }
        }
    }
}

void TestLayouts() {
    const WindowLayout fs = PlanFullscreenLayout({640, 480}, k4K, kDefault);
    CHECK(fs.window == k4K && fs.client == k4K);
    CHECK((fs.viewport == Rect{640, 120, 3200, 2040}));

    // Windowed on a 1080p work area with a typical frame.
    const Rect work{0, 0, 1920, 1032};
    const Insets frame{8, 31, 8, 8};
    const WindowLayout w = PlanWindowedLayout({640, 480}, work, frame, kDefault);
    CHECK(w.integerScale == 2);
    CHECK(w.client.Width() == 1280 && w.client.Height() == 960);
    CHECK(w.viewport == w.client);
    CHECK(w.window.left == w.client.left - 8 && w.window.top == w.client.top - 31);
    CHECK(work.Contains(w.window));
    CHECK(std::abs((w.window.left - work.left) - (work.right - w.window.right)) <= 1);
}

// --- Coordinate mapping ---------------------------------------------------------------

void TestViewportMap() {
    const ViewportMap map({640, 120, 3200, 2040}, {640, 480});
    CHECK((map.ToReal(Point{0, 0}) == Point{640, 120}));
    CHECK((map.ToReal(Point{320, 240}) == Point{1920, 1080}));
    CHECK((map.ToReal(Rect{0, 0, 640, 480}) == Rect{640, 120, 3200, 2040}));
    CHECK((map.ToVirtual({640, 120}) == Point{0, 0}));
    CHECK((map.ToVirtual({3199, 2039}) == Point{639, 479}));
    CHECK((map.ToVirtual({1923, 1083}) == Point{320, 240}));
    CHECK(map.ScaleX() == 4.0 && map.ScaleY() == 4.0);

    // Letterbox bars clamp to the nearest edge.
    CHECK((map.ToVirtual({0, 0}) == Point{0, 0}));
    CHECK((map.ToVirtual({3839, 2159}) == Point{639, 479}));
    CHECK((map.ToVirtual({100, 1080}) == Point{0, 240}));
}

void TestCursorRoundTrip() {
    // SetCursorPos(ToRealCenter(p)) then GetCursorPos -> ToVirtual must give p
    // back exactly, including awkward fractional scales and downscaling.
    const Rect viewports[] = {{640, 120, 3200, 2040},  // 4x
                              {240, 0, 1680, 1080},    // 2.25x
                              {0, 0, 704, 528},        // 1.1x
                              {10, 10, 330, 250}};     // 0.5x (downscale)
    for (const Rect& vp : viewports) {
        const ViewportMap map(vp, {640, 480});
        const bool upscale = vp.Width() >= 640;
        for (int32_t x = 0; x < 640; ++x) {
            const Point p{x, x * 3 / 4};
            const Point back = map.ToVirtual(map.ToRealCenter(p));
            if (upscale) {
                CHECK(back == p);
            } else {
                CHECK(std::abs(back.x - p.x) <= 1 && std::abs(back.y - p.y) <= 1);
            }
            const Point r = map.ToRealCenter(p);
            CHECK(r.x >= vp.left && r.x < vp.right && r.y >= vp.top && r.y < vp.bottom);
        }
    }
}

// --- Window classification ---------------------------------------------------------------

void TestClassification() {
    const Size screen{640, 480};
    CHECK(CoversScreen({0, 0, 640, 480}, screen));
    CHECK(CoversScreen({-4, -4, 644, 484}, screen));    // oversized popups count
    CHECK(CoversScreen({0, 0, 3840, 2160}, screen));    // sized from the real screen earlier
    CHECK(!CoversScreen({0, 0, 639, 480}, screen));
    CHECK(!CoversScreen({10, 0, 650, 480}, screen));
    CHECK(!CoversScreen({0, 0, 640, 480}, {0, 0}));

    CHECK(LooksLikeFullscreenWindow(WS_POPUP | WS_VISIBLE, 0, {0, 0, 640, 480}, screen));
    CHECK(LooksLikeFullscreenWindow(WS_OVERLAPPEDWINDOW, WS_EX_TOPMOST, {0, 0, 640, 480}, screen));
    CHECK(!LooksLikeFullscreenWindow(WS_CHILD, 0, {0, 0, 640, 480}, screen));
    CHECK(!LooksLikeFullscreenWindow(WS_POPUP, WS_EX_TOOLWINDOW, {0, 0, 640, 480}, screen));
    CHECK(!LooksLikeFullscreenWindow(WS_OVERLAPPEDWINDOW, 0, {100, 100, 740, 580}, screen));
}

void TestSandboxStyles() {
    const uint32_t game = WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_MAXIMIZE;

    const uint32_t fs = SandboxStyle(game, false);
    CHECK(fs & WS_POPUP);
    CHECK(!(fs & (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_BORDER | WS_MAXIMIZE)));
    CHECK((fs & WS_VISIBLE) && (fs & WS_CLIPCHILDREN));  // game's own bits kept

    const uint32_t win = SandboxStyle(WS_POPUP | WS_VISIBLE, true);
    CHECK(!(win & WS_POPUP));
    CHECK((win & WS_CAPTION) == WS_CAPTION && (win & WS_SYSMENU) && (win & WS_MINIMIZEBOX));
    CHECK(!(win & (WS_THICKFRAME | WS_MAXIMIZEBOX)));  // size is fixed by the scaler
    CHECK(win & WS_VISIBLE);

    const uint32_t ex = SandboxExStyle(WS_EX_TOPMOST | WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES, false);
    CHECK(!(ex & (WS_EX_TOPMOST | WS_EX_CLIENTEDGE)));
    CHECK(ex & WS_EX_ACCEPTFILES);

    // Idempotent: re-sandboxing a sandboxed style changes nothing.
    CHECK(SandboxStyle(fs, false) == fs);
    CHECK(SandboxStyle(win, true) == win);
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"ModeRequests", TestModeRequests},
        {"ClassicModeList", TestClassicModeList},
        {"Aspect", TestAspect},
        {"IntegerViewports", TestIntegerViewports},
        {"FractionalViewports", TestFractionalViewports},
        {"AspectCorrectedViewports", TestAspectCorrectedViewports},
        {"Layouts", TestLayouts},
        {"ViewportMap", TestViewportMap},
        {"CursorRoundTrip", TestCursorRoundTrip},
        {"Classification", TestClassification},
        {"SandboxStyles", TestSandboxStyles},
    };
    return test::RunAll(cases);
}
