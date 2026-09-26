#include "retro/DisplayMath.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace retro {
namespace {

int32_t RoundToInt(double v) {
    return static_cast<int32_t>(std::lround(v));
}

Size Reduce(Size s) {
    const int32_t g = std::gcd(s.w, s.h);
    return g > 0 ? Size{s.w / g, s.h / g} : s;
}

Rect CenteredIn(const Rect& area, Size size) {
    const int32_t left = area.left + (area.Width() - size.w) / 2;
    const int32_t top = area.top + (area.Height() - size.h) / 2;
    return {left, top, left + size.w, top + size.h};
}

// Largest size with aspect `aspect` that fits in `avail` (never zero-sized).
Size FitAspect(Size aspect, Size avail) {
    if (static_cast<int64_t>(avail.w) * aspect.h <= static_cast<int64_t>(avail.h) * aspect.w) {
        return {avail.w, std::max(1, RoundToInt(static_cast<double>(avail.w) * aspect.h / aspect.w))};
    }
    return {std::max(1, RoundToInt(static_cast<double>(avail.h) * aspect.w / aspect.h)), avail.h};
}

// Maps [0, from) onto [0, to) proportionally (64-bit to avoid overflow).
int32_t Scale(int32_t v, int32_t from, int32_t to) {
    if (from <= 0) return 0;
    return static_cast<int32_t>(static_cast<int64_t>(v) * to / from);
}

}  // namespace

// --- Display modes -------------------------------------------------------------

bool IsPlausibleMode(const DisplayMode& m) {
    const bool depthOk = m.bitsPerPixel == 4 || m.bitsPerPixel == 8 || m.bitsPerPixel == 15 ||
                         m.bitsPerPixel == 16 || m.bitsPerPixel == 24 || m.bitsPerPixel == 32;
    return m.width >= 160 && m.width <= 7680 && m.height >= 120 && m.height <= 4320 && depthOk &&
           m.refreshHz <= 500;
}

bool ResolveModeRequest(const ModeRequest& r, const DisplayMode& current, DisplayMode& out) {
    out = current;
    if (r.hasWidth) out.width = r.width;
    if (r.hasHeight) out.height = r.height;
    if (r.hasBitsPerPixel) out.bitsPerPixel = r.bitsPerPixel;
    // 0 and 1 mean "hardware default" in DEVMODE.
    if (r.hasRefresh) out.refreshHz = r.refreshHz <= 1 ? current.refreshHz : r.refreshHz;
    return IsPlausibleMode(out);
}

std::vector<DisplayMode> ClassicModeList(Size maxSize) {
    static constexpr Size kSizes[] = {
        {320, 200},  {320, 240},  {400, 300},   {512, 384},   {640, 400},   {640, 480},
        {800, 600},  {1024, 768}, {1152, 864},  {1280, 960},  {1280, 1024}, {1600, 1200},
    };
    static constexpr uint32_t kDepths[] = {8, 16, 32};

    std::vector<DisplayMode> modes;
    for (uint32_t bpp : kDepths) {
        for (Size s : kSizes) {
            if (s.w <= maxSize.w && s.h <= maxSize.h) {
                modes.push_back({static_cast<uint32_t>(s.w), static_cast<uint32_t>(s.h), bpp, 60});
            }
        }
    }
    return modes;  // kSizes is already ordered by width, then height
}

// --- Viewport planning -------------------------------------------------------------

Size DisplayAspectFor(Size mode, const ScalingOptions& options) {
    if (mode.w <= 0 || mode.h <= 0) return {4, 3};
    const bool doubleScan16x10 = mode.h <= 400 && mode.w * 10 == mode.h * 16;
    if (options.classicAspect && doubleScan16x10) return {4, 3};
    return Reduce(mode);
}

Viewport PlanViewport(Size virt, Rect area, const ScalingOptions& options) {
    const Size avail = area.Extent();
    if (virt.w <= 0 || virt.h <= 0 || avail.w <= 0 || avail.h <= 0) return {area, 0};

    const Size aspect = DisplayAspectFor(virt, options);
    const bool squarePixels =
        static_cast<int64_t>(virt.w) * aspect.h == static_cast<int64_t>(virt.h) * aspect.w;

    Size size{};
    int32_t integerScale = 0;
    if (options.integerScaling) {
        if (squarePixels) {
            const int32_t k = std::min(avail.w / virt.w, avail.h / virt.h);
            if (k >= 1) {
                size = {virt.w * k, virt.h * k};
                integerScale = k;
            }
        } else {
            // Non-square pixels: scale lines by a whole number, stretch width.
            for (int32_t k = avail.h / virt.h; k >= 1; --k) {
                const int32_t h = virt.h * k;
                const int32_t w = RoundToInt(static_cast<double>(h) * aspect.w / aspect.h);
                if (w <= avail.w) {
                    size = {w, h};
                    integerScale = k;
                    break;
                }
            }
        }
    }
    if (integerScale == 0) size = FitAspect(aspect, avail);

    return {CenteredIn(area, size), integerScale};
}

WindowLayout PlanFullscreenLayout(Size virt, Rect monitor, const ScalingOptions& options) {
    const Viewport vp = PlanViewport(virt, monitor, options);
    return {monitor, monitor, vp.rect, vp.integerScale};
}

WindowLayout PlanWindowedLayout(Size virt, Rect workArea, Insets frame,
                                const ScalingOptions& options) {
    const Rect avail{workArea.left + frame.left, workArea.top + frame.top,
                     workArea.right - frame.right, workArea.bottom - frame.bottom};
    const Viewport vp = PlanViewport(virt, avail, options);

    // Centre the whole window (frame included) in the work area.
    const Size outer{vp.rect.Width() + frame.left + frame.right,
                     vp.rect.Height() + frame.top + frame.bottom};
    const Rect window = CenteredIn(workArea, outer);
    const Rect client{window.left + frame.left, window.top + frame.top,
                      window.right - frame.right, window.bottom - frame.bottom};
    return {window, client, client, vp.integerScale};
}

// --- Coordinate mapping -----------------------------------------------------------

Point ViewportMap::ToReal(Point v) const {
    return {real_.left + Scale(v.x, virt_.w, real_.Width()),
            real_.top + Scale(v.y, virt_.h, real_.Height())};
}

namespace {

// Middle of the real pixels that ToVirtual maps back to virtual pixel v:
// [ceil(v*R/V), ceil((v+1)*R/V) - 1]. With downscaling the span can be empty;
// then its first pixel is the best available.
int32_t CenterOfSpan(int32_t v, int32_t virtSize, int32_t realSize) {
    if (virtSize <= 0 || v < 0) return 0;
    auto ceilDiv = [](int64_t a, int64_t b) { return (a + b - 1) / b; };
    const int64_t lo = ceilDiv(static_cast<int64_t>(v) * realSize, virtSize);
    const int64_t hi = ceilDiv(static_cast<int64_t>(v + 1) * realSize, virtSize) - 1;
    const int64_t center = hi >= lo ? (lo + hi) / 2 : lo;
    return static_cast<int32_t>(std::min<int64_t>(center, realSize - 1));
}

}  // namespace

Point ViewportMap::ToRealCenter(Point v) const {
    return {real_.left + CenterOfSpan(v.x, virt_.w, real_.Width()),
            real_.top + CenterOfSpan(v.y, virt_.h, real_.Height())};
}

Rect ViewportMap::ToReal(const Rect& v) const {
    const Point tl = ToReal(Point{v.left, v.top});
    const Point br = ToReal(Point{v.right, v.bottom});
    return {tl.x, tl.y, br.x, br.y};
}

Point ViewportMap::ToVirtual(Point r) const {
    if (virt_.w <= 0 || virt_.h <= 0) return {};
    const int32_t x = Scale(r.x - real_.left, real_.Width(), virt_.w);
    const int32_t y = Scale(r.y - real_.top, real_.Height(), virt_.h);
    return {std::clamp(x, 0, virt_.w - 1), std::clamp(y, 0, virt_.h - 1)};
}

double ViewportMap::ScaleX() const {
    return virt_.w > 0 ? static_cast<double>(real_.Width()) / virt_.w : 1.0;
}

double ViewportMap::ScaleY() const {
    return virt_.h > 0 ? static_cast<double>(real_.Height()) / virt_.h : 1.0;
}

// --- Window classification -----------------------------------------------------------

bool CoversScreen(const Rect& window, Size screen) {
    return screen.w > 0 && screen.h > 0 && window.left <= 0 && window.top <= 0 &&
           window.right >= screen.w && window.bottom >= screen.h;
}

bool LooksLikeFullscreenWindow(uint32_t style, uint32_t exStyle, const Rect& window, Size screen) {
    if (style & WS_CHILD) return false;
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    return CoversScreen(window, screen);
}

uint32_t SandboxStyle(uint32_t style, bool windowed) {
    constexpr uint32_t kFrame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
                                WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME;
    uint32_t s = style & ~(kFrame | WS_POPUP | WS_CHILD | WS_MAXIMIZE);
    if (windowed) {
        // Fixed size: integer scaling decides the size, so no resize frame.
        s |= WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    } else {
        s |= WS_POPUP;
    }
    return s;
}

uint32_t SandboxExStyle(uint32_t exStyle, bool /*windowed*/) {
    // Topmost fullscreen windows trap alt-tab and cover other monitors'
    // popups; a monitor-sized window is already treated as fullscreen.
    constexpr uint32_t kStrip = WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                                WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE | WS_EX_TOOLWINDOW;
    return exStyle & ~kStrip;
}

}  // namespace retro
