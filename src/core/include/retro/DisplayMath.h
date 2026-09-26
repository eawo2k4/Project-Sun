#pragma once

// Pure geometry behind the display sandbox: virtual display modes, 4:3
// viewport planning with integer scaling, coordinate mapping between the
// game's virtual screen and the real monitor, and window classification.
// No OS calls happen here except reading style bit constants, so everything
// is unit-testable.

#include <cstdint>
#include <vector>

namespace retro {

struct Size {
    int32_t w = 0;
    int32_t h = 0;
    bool operator==(const Size&) const = default;
};

struct Point {
    int32_t x = 0;
    int32_t y = 0;
    bool operator==(const Point&) const = default;
};

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t Width() const { return right - left; }
    int32_t Height() const { return bottom - top; }
    Size Extent() const { return {Width(), Height()}; }
    bool Contains(const Rect& r) const {
        return r.left >= left && r.top >= top && r.right <= right && r.bottom <= bottom;
    }
    bool operator==(const Rect&) const = default;
};

struct Insets {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

// --- Display modes -------------------------------------------------------------

struct DisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 0;
    uint32_t refreshHz = 0;
    bool operator==(const DisplayMode&) const = default;
};

// A ChangeDisplaySettings request: DEVMODE only carries the fields named in
// dmFields; everything else keeps its current value.
struct ModeRequest {
    bool hasWidth = false;
    bool hasHeight = false;
    bool hasBitsPerPixel = false;
    bool hasRefresh = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 0;
    uint32_t refreshHz = 0;
};

// Anything a vintage game could reasonably ask a 1990s/2000s display for.
bool IsPlausibleMode(const DisplayMode& mode);

// Merges `request` over `current`; false (DISP_CHANGE_BADMODE) if the result
// isn't plausible.
bool ResolveModeRequest(const ModeRequest& request, const DisplayMode& current, DisplayMode& out);

// Classic modes offered through EnumDisplaySettings: 8/16/32 bpp at 60 Hz for
// the usual CRT-era resolutions that fit in `maxSize`, sorted the way Windows
// enumerates (by depth, then width, then height). Modern drivers no longer
// list 8/16-bit modes, and games refuse to start when they can't find one.
std::vector<DisplayMode> ClassicModeList(Size maxSize);

// --- Viewport planning -------------------------------------------------------------

struct ScalingOptions {
    bool integerScaling = true;  // prefer whole-number scale factors (crisp pixels)
    bool classicAspect = true;   // show 320x200 / 640x400 at 4:3, as on a CRT
};

// Display aspect ratio (reduced w:h) a mode should be shown at. Square pixels
// for everything except the double-scan 16:10 modes (320x200, 640x400) that
// CRTs stretched to 4:3.
Size DisplayAspectFor(Size mode, const ScalingOptions& options);

struct Viewport {
    Rect rect;                 // where the virtual screen lands, in real coordinates
    int32_t integerScale = 0;  // whole-number factor used, 0 if fractional
};

// Largest presentation of `virt` inside `area`, centred. Square-pixel modes
// get the largest integer scale that fits (4x for 640x480 on 4K); aspect-
// corrected modes get an integer *vertical* scale; anything that doesn't fit
// at 1x falls back to a fractional aspect-correct fit.
Viewport PlanViewport(Size virt, Rect area, const ScalingOptions& options);

struct WindowLayout {
    Rect window;    // outer window rectangle (screen coordinates)
    Rect client;    // client area (screen coordinates)
    Rect viewport;  // virtual screen inside the client area
    int32_t integerScale = 0;
};

// Borderless fullscreen: the window covers the whole monitor (so Windows
// treats it as fullscreen and hides the taskbar) and the viewport is
// letterboxed/pillarboxed inside the client area.
WindowLayout PlanFullscreenLayout(Size virt, Rect monitor, const ScalingOptions& options);

// Windowed: the client area is exactly the scaled viewport, the frame (from
// AdjustWindowRectEx) is added around it and the window is centred in the
// work area.
WindowLayout PlanWindowedLayout(Size virt, Rect workArea, Insets frame,
                                const ScalingOptions& options);

// --- Coordinate mapping -----------------------------------------------------------

// Maps the game's virtual screen (0,0)-(w,h) to the real viewport rectangle.
// Because a sandboxed window always sits at the virtual origin, virtual
// screen coordinates and the window's virtual client coordinates coincide.
class ViewportMap {
public:
    ViewportMap() = default;
    ViewportMap(Rect realViewport, Size virt) : real_(realViewport), virt_(virt) {}

    Point ToReal(Point virtualPt) const;  // top-left corner of the scaled pixel
    Rect ToReal(const Rect& virtualRect) const;

    // Centre of the scaled pixel. Use for SetCursorPos: ToVirtual(ToRealCenter(p))
    // == p exactly, even with fractional scaling, so games that re-centre the
    // mouse and read the delta back don't drift.
    Point ToRealCenter(Point virtualPt) const;

    // Real screen point -> virtual, clamped to the virtual screen so the
    // letterbox bars map onto the nearest edge.
    Point ToVirtual(Point realPt) const;

    double ScaleX() const;
    double ScaleY() const;

    const Rect& RealViewport() const { return real_; }
    Size VirtualSize() const { return virt_; }

private:
    Rect real_;
    Size virt_;
};

// --- Window classification -----------------------------------------------------------

// True if `window` covers the whole screen (0,0)-(screen.w,screen.h).
bool CoversScreen(const Rect& window, Size screen);

// Heuristic for "this is the game's fullscreen window": a top-level, non-tool
// window covering the (virtual) screen.
bool LooksLikeFullscreenWindow(uint32_t style, uint32_t exStyle, const Rect& window, Size screen);

// Window styles for a sandboxed window: borderless popup for fullscreen, or a
// fixed-size captioned window for --windowed. Keeps the game's other bits
// (visible, disabled, clip flags, ...).
uint32_t SandboxStyle(uint32_t style, bool windowed);
uint32_t SandboxExStyle(uint32_t exStyle, bool windowed);

}  // namespace retro
