// Display sandbox / frame pacing probes (see ShimProbe.cpp for the modes).
//
// Safety: these probes call ChangeDisplaySettings. If the sandbox weren't
// active that would change the real desktop resolution, so every probe first
// asks the shim (RetroShimIsModuleActive) and bails out otherwise, and every
// mode change passes CDS_FULLSCREEN so Windows would revert it on exit anyway.
// All test windows stay hidden: nothing flashes on screen, nothing takes focus,
// and the real cursor is never moved or clipped.

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <string>

#include "ProbeCommon.h"
#include "retro/DisplayMath.h"
#include "retro/FramePacing.h"

// Exported by opengl32.dll but not declared in the SDK headers.
extern "C" __declspec(dllimport) BOOL WINAPI wglSwapBuffers(HDC);

namespace probe {
namespace {

using retro::PlanViewport;
using retro::Rect;
using retro::Size;

struct WindowEvents {
    LPARAM lastSize = 0;
    int displayChanges = 0;
    WPARAM displayBpp = 0;
    LPARAM displaySize = 0;
} g_events;

LRESULT CALLBACK ProbeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) g_events.lastSize = lp;
    if (msg == WM_DISPLAYCHANGE) {
        ++g_events.displayChanges;
        g_events.displayBpp = wp;
        g_events.displaySize = lp;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND MakeWindow(DWORD style, int x, int y, int w, int h) {
    static const ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ProbeWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RetroShimProbe";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        return RegisterClassW(&wc);
    }();
    (void)cls;
    return CreateWindowExW(0, L"RetroShimProbe", L"probe", style & ~WS_VISIBLE, x, y, w, h,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool ModuleActive(const char* name) {
    using Fn = BOOL(WINAPI*)(const char*);
    const HMODULE shim = GetModuleHandleW(L"RetroShim.dll");
    const auto fn = shim ? reinterpret_cast<Fn>(GetProcAddress(shim, "RetroShimIsModuleActive"))
                         : nullptr;
    return fn && fn(name);
}

bool RequireDisplaySandbox() {
    if (ModuleActive("display")) return true;
    std::printf("display sandbox is not active: refusing to call ChangeDisplaySettings\n");
    return false;
}

LONG SetMode(DWORD width, DWORD height, DWORD bpp, DWORD flags = CDS_FULLSCREEN) {
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmBitsPerPel = bpp;
    return ChangeDisplaySettingsA(&dm, flags);
}

class DpiScope {
public:
    explicit DpiScope(DPI_AWARENESS_CONTEXT c) : prev_(SetThreadDpiAwarenessContext(c)) {}
    ~DpiScope() { SetThreadDpiAwarenessContext(prev_); }

private:
    DPI_AWARENESS_CONTEXT prev_;
};

Rect ToRect(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

// Real geometry, bypassing the virtualized GetWindowRect/GetClientRect: in
// the window's own DPI context, from GetWindowInfo (which isn't hooked).
struct RealGeometry {
    Rect client;
    Rect monitor;
    Rect work;
};

RealGeometry QueryReal(HWND hwnd) {
    DpiScope scope(GetWindowDpiAwarenessContext(hwnd));
    WINDOWINFO wi{};
    wi.cbSize = sizeof(wi);
    GetWindowInfo(hwnd, &wi);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    return {ToRect(wi.rcClient), ToRect(mi.rcMonitor), ToRect(mi.rcWork)};
}

bool VirtualRectIs(HWND hwnd, bool client, int w, int h) {
    RECT r{};
    if (client) GetClientRect(hwnd, &r); else GetWindowRect(hwnd, &r);
    return r.left == 0 && r.top == 0 && r.right == w && r.bottom == h;
}

bool Near(int a, int b) { return std::abs(a - b) <= 1; }

}  // namespace

int ProbeModule(const char* name, bool expectActive) {
    const bool active = ModuleActive(name);
    std::printf("module %s: %s (expected %s)\n", name, active ? "active" : "inactive",
                expectActive ? "active" : "inactive");
    return active == expectActive ? 0 : 1;
}

int ProbeDisplayModes() {
    if (!RequireDisplaySandbox()) return 3;

    HDC screen = GetDC(nullptr);
    const int desktopW = GetDeviceCaps(screen, DESKTOPHORZRES);
    const int desktopH = GetDeviceCaps(screen, DESKTOPVERTRES);
    ReleaseDC(nullptr, screen);
    const int metricW = GetSystemMetrics(SM_CXSCREEN);
    std::printf("real desktop %dx%d (SM_CXSCREEN %d)\n", desktopW, desktopH, metricW);

    HWND listener = MakeWindow(WS_OVERLAPPED, 20, 20, 200, 100);

    bool has640x480x16 = false, has800x600x8 = false;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &dm); ++i) {
        has640x480x16 |= dm.dmPelsWidth == 640 && dm.dmPelsHeight == 480 && dm.dmBitsPerPel == 16;
        has800x600x8 |= dm.dmPelsWidth == 800 && dm.dmPelsHeight == 600 && dm.dmBitsPerPel == 8;
    }
    Expect(has640x480x16 && has800x600x8, "EnumDisplaySettings lists classic 8/16-bit modes");

    Expect(SetMode(640, 480, 16, CDS_TEST) == DISP_CHANGE_SUCCESSFUL, "CDS_TEST 640x480x16 ok");
    Expect(SetMode(13, 7, 16, CDS_TEST) == DISP_CHANGE_BADMODE, "CDS_TEST 13x7 -> BADMODE");

    Expect(SetMode(640, 480, 16) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsA 640x480x16");
    screen = GetDC(nullptr);
    const bool desktopUntouched = GetDeviceCaps(screen, DESKTOPHORZRES) == desktopW &&
                                  GetDeviceCaps(screen, DESKTOPVERTRES) == desktopH;
    Expect(desktopUntouched, "real desktop resolution unchanged");
    Expect(GetDeviceCaps(screen, HORZRES) == 640 && GetDeviceCaps(screen, VERTRES) == 480 &&
               GetDeviceCaps(screen, BITSPIXEL) == 16,
           "GetDeviceCaps reports 640x480x16");
    ReleaseDC(nullptr, screen);
    if (!desktopUntouched) {
        ChangeDisplaySettingsA(nullptr, 0);  // belt and braces
        return 1;
    }

    Expect(GetSystemMetrics(SM_CXSCREEN) == 640 && GetSystemMetrics(SM_CYSCREEN) == 480,
           "GetSystemMetrics reports 640x480");
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    Expect(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur) && cur.dmPelsWidth == 640 &&
               cur.dmPelsHeight == 480 && cur.dmBitsPerPel == 16,
           "EnumDisplaySettings(CURRENT) reports 640x480x16");
    Expect(g_events.displayChanges == 1 && g_events.displayBpp == 16 &&
               g_events.displaySize == MAKELPARAM(640, 480),
           "WM_DISPLAYCHANGE sent to the game's windows");

    // Depth-only change through the Ex/Unicode entry point keeps the size.
    DEVMODEW depth{};
    depth.dmSize = sizeof(depth);
    depth.dmFields = DM_BITSPERPEL;
    depth.dmBitsPerPel = 32;
    Expect(ChangeDisplaySettingsExW(nullptr, &depth, nullptr, CDS_FULLSCREEN, nullptr) ==
               DISP_CHANGE_SUCCESSFUL,
           "ChangeDisplaySettingsExW depth-only");
    Expect(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &cur) && cur.dmPelsWidth == 640 &&
               cur.dmBitsPerPel == 32,
           "mode is now 640x480x32");

    Expect(ChangeDisplaySettingsA(nullptr, 0) == DISP_CHANGE_SUCCESSFUL, "restore (NULL, 0)");
    Expect(GetSystemMetrics(SM_CXSCREEN) == metricW, "GetSystemMetrics back to the desktop");

    DestroyWindow(listener);
    return Result();
}

int ProbeWindow(bool windowed) {
    if (!RequireDisplaySandbox()) return 3;
    Expect(SetMode(640, 480, 16) == DISP_CHANGE_SUCCESSFUL, "virtual mode 640x480x16");

    HWND hwnd = MakeWindow(WS_POPUP, 0, 0, 640, 480);
    Expect(hwnd != nullptr, "CreateWindowEx fullscreen popup");
    if (!hwnd) return 1;

    Expect(VirtualRectIs(hwnd, true, 640, 480), "GetClientRect is 640x480");
    Expect(VirtualRectIs(hwnd, false, 640, 480), "GetWindowRect is (0,0)-(640,480)");
    Expect(GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)) ==
               DPI_AWARENESS_PER_MONITOR_AWARE,
           "window is per-monitor DPI aware (no DWM blur)");

    const RealGeometry real = QueryReal(hwnd);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    std::printf("real client (%d,%d)-(%d,%d), monitor %dx%d\n", real.client.left, real.client.top,
                real.client.right, real.client.bottom, real.monitor.Width(),
                real.monitor.Height());
    if (windowed) {
        const int k = real.client.Width() / 640;
        Expect((style & WS_CAPTION) == WS_CAPTION && !(style & WS_POPUP), "captioned window");
        Expect(k >= 1 && real.client.Width() == 640 * k && real.client.Height() == 480 * k,
               "client is an integer multiple of 640x480");
        Expect(real.work.Contains(real.client), "window fits the work area");
    } else {
        Expect((style & WS_POPUP) && !(style & WS_CAPTION), "borderless popup");
        Expect(real.client == real.monitor, "window covers the monitor (taskbar hides)");
    }

    const Rect vp = PlanViewport({640, 480}, real.client, {}).rect;
    HDC dc = GetDC(hwnd);
    XFORM xf{};
    GetWorldTransform(dc, &xf);
    Expect(GetGraphicsMode(dc) == GM_ADVANCED && std::fabs(xf.eM11 - vp.Width() / 640.0f) < 1e-4f &&
               std::fabs(xf.eM22 - vp.Height() / 480.0f) < 1e-4f &&
               std::lround(xf.eDx) == vp.left - real.client.left &&
               std::lround(xf.eDy) == vp.top - real.client.top,
           "GetDC has the viewport scaling transform");
    ReleaseDC(hwnd, dc);

    SetWindowPos(hwnd, HWND_TOPMOST, 50, 50, 100, 100, SWP_NOACTIVATE);
    MoveWindow(hwnd, 10, 10, 200, 200, FALSE);
    Expect(QueryReal(hwnd).client == real.client, "SetWindowPos/MoveWindow can't move or shrink it");
    Expect(!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST), "topmost request dropped");

    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1234, 567));
    Expect(g_events.lastSize == MAKELPARAM(640, 480), "WM_SIZE reaches the game as 640x480");

    // Mouse at the real centre of the viewport -> virtual (320, 240).
    const int cx = (vp.left + vp.right) / 2 - real.client.left;
    const int cy = (vp.top + vp.bottom) / 2 - real.client.top;
    PostMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
    MSG msg{};
    const bool got = PeekMessageW(&msg, hwnd, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE) != FALSE;
    Expect(got && Near(LOWORD(msg.lParam), 320) && Near(HIWORD(msg.lParam), 240),
           "WM_MOUSEMOVE mapped to virtual (320,240)");

    // Top-left pixel of the client (a letterbox bar when fullscreen) clamps to (0,0).
    PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(0, 0));
    Expect(PeekMessageW(&msg, hwnd, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE) &&
               msg.lParam == MAKELPARAM(0, 0),
           "click in the letterbox clamps to the edge");

    POINT pt{};
    GetCursorPos(&pt);
    Expect(pt.x >= 0 && pt.x < 640 && pt.y >= 0 && pt.y < 480, "GetCursorPos in virtual space");

    // Clip rectangle round-trips in virtual coordinates. The window is hidden
    // (never foreground), so the real cursor is not confined.
    const RECT clip{10, 20, 300, 200};
    RECT back{};
    ClipCursor(&clip);
    GetClipCursor(&back);
    Expect(EqualRect(&clip, &back) != FALSE, "ClipCursor/GetClipCursor in virtual space");
    ClipCursor(nullptr);

    Expect(ChangeDisplaySettingsA(nullptr, 0) == DISP_CHANGE_SUCCESSFUL, "restore desktop mode");
    Expect(!VirtualRectIs(hwnd, true, 640, 480), "window released after restore");
    DestroyWindow(hwnd);
    return Result();
}

int ProbeAdoption() {
    if (!RequireDisplaySandbox()) return 3;

    // Created at 640x480 *before* the mode change (a very common order).
    HWND early = MakeWindow(WS_POPUP, 0, 0, 640, 480);
    const Rect before = QueryReal(early).client;
    Expect(SetMode(640, 480, 16) == DISP_CHANGE_SUCCESSFUL, "virtual mode 640x480x16");
    const RealGeometry adopted = QueryReal(early);
    Expect(adopted.client != before && adopted.client == adopted.monitor,
           "window created before the mode change is adopted");
    Expect(VirtualRectIs(early, true, 640, 480), "and reports a 640x480 client");

    // Created small with a caption, then sized to the screen with SetWindowPos.
    HWND later = MakeWindow(WS_OVERLAPPEDWINDOW, 100, 100, 300, 200);
    Expect(!VirtualRectIs(later, true, 640, 480), "small captioned window left alone");
    SetWindowPos(later, HWND_TOP, 0, 0, 640, 480, SWP_NOACTIVATE);
    Expect(VirtualRectIs(later, true, 640, 480) &&
               !(GetWindowLongW(later, GWL_STYLE) & WS_CAPTION),
           "SetWindowPos to the full screen adopts it (caption removed)");

    // A game swapping in its own window procedure keeps working, and our
    // subclass stays on top (WM_SIZE still translated).
    const LONG_PTR previous = SetWindowLongPtrW(later, GWLP_WNDPROC,
                                                reinterpret_cast<LONG_PTR>(ProbeWndProc));
    Expect(previous != 0, "SetWindowLongPtr(GWLP_WNDPROC) returns the previous procedure");
    SendMessageW(later, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1, 1));
    Expect(g_events.lastSize == MAKELPARAM(640, 480), "subclass survives a GWLP_WNDPROC change");

    ChangeDisplaySettingsA(nullptr, 0);
    DestroyWindow(early);
    DestroyWindow(later);
    return Result();
}

namespace {

// Times `frames` calls of `present` in seconds.
template <class Fn>
double TimeFrames(int frames, Fn present) {
    const int64_t start = retro::QpcNow();
    for (int i = 0; i < frames; ++i) present();
    return static_cast<double>(retro::QpcNow() - start) / retro::QpcFrequency();
}

void ExpectPaced(double seconds, int frames, uint32_t fps, const char* what) {
    std::printf("%s: %d frames in %.1f ms\n", what, frames, seconds * 1000);
    if (fps == 0) {
        Expect(seconds < 0.1, "unpaced (fast)");
    } else {
        // frames - 2 intervals: the scheduler may let one catch-up frame out
        // immediately if the previous sequence ended behind its cadence.
        const double minimum = (frames - 2) / static_cast<double>(fps);
        const double maximum = (frames + 1) / static_cast<double>(fps) * 1.5;
        Expect(seconds >= minimum * 0.99 && seconds <= maximum, "paced to the frame cap");
    }
}

}  // namespace

int ProbeRender(uint32_t fps) {
    constexpr int kFrames = 20;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 640;
    bmi.bmiHeader.biHeight = -480;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, dib);

    // 1) A windowed game (no mode change): back buffer blitted to the client.
    HWND plain = MakeWindow(WS_OVERLAPPEDWINDOW, 100, 100, 400, 300);
    RECT cr{};
    GetClientRect(plain, &cr);
    HDC dc = GetDC(plain);
    ExpectPaced(TimeFrames(kFrames,
                           [&] { BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY); }),
                kFrames, fps, "BitBlt, windowed");
    const double sprites = TimeFrames(200, [&] { BitBlt(dc, 5, 5, 16, 16, mem, 0, 0, SRCCOPY); });
    std::printf("200 sprite blits in %.1f ms\n", sprites * 1000);
    Expect(sprites < 0.1, "sprite-sized blits are never paced");
    ReleaseDC(plain, dc);
    DestroyWindow(plain);

    // OpenGL: SwapBuffers (gdi32) calls down into wglSwapBuffers (opengl32);
    // a frame must be paced once, not twice.
    HWND glWindow = MakeWindow(WS_OVERLAPPEDWINDOW, 100, 100, 400, 300);
    dc = GetDC(glWindow);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int format = ChoosePixelFormat(dc, &pfd);
    HGLRC gl = format && SetPixelFormat(dc, format, &pfd) ? wglCreateContext(dc) : nullptr;
    if (gl && wglMakeCurrent(dc, gl)) {
        // Turn the driver's own vsync off, so the timings measure the shim's
        // pacer and not the monitor's refresh rate.
        using SwapIntervalFn = BOOL(WINAPI*)(int);
        if (const auto swapInterval =
                reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"))) {
            swapInterval(0);
        }
        ExpectPaced(TimeFrames(kFrames, [&] { SwapBuffers(dc); }), kFrames, fps,
                    "SwapBuffers (OpenGL)");
        ExpectPaced(TimeFrames(kFrames, [&] { wglSwapBuffers(dc); }), kFrames, fps,
                    "wglSwapBuffers (OpenGL)");
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl);
    } else {
        std::printf("no OpenGL context available: skipping SwapBuffers checks\n");
    }
    ReleaseDC(glWindow, dc);
    DestroyWindow(glWindow);

    // 2) A fullscreen game in the sandbox: DIB presents through the scaled DC.
    if (ModuleActive("display")) {
        Expect(SetMode(640, 480, 32) == DISP_CHANGE_SUCCESSFUL, "virtual mode 640x480x32");
        HWND full = MakeWindow(WS_POPUP, 0, 0, 640, 480);
        dc = GetDC(full);
        ExpectPaced(TimeFrames(kFrames,
                               [&] {
                                   StretchDIBits(dc, 0, 0, 640, 480, 0, 0, 640, 480, bits, &bmi,
                                                 DIB_RGB_COLORS, SRCCOPY);
                               }),
                    kFrames, fps, "StretchDIBits, sandboxed");
        int lines = 0;
        ExpectPaced(TimeFrames(kFrames,
                               [&] {
                                   lines = SetDIBitsToDevice(dc, 0, 0, 640, 480, 0, 0, 0, 480,
                                                             bits, &bmi, DIB_RGB_COLORS);
                               }),
                    kFrames, fps, "SetDIBitsToDevice, sandboxed");
        Expect(lines == 480, "SetDIBitsToDevice (rewritten as StretchDIBits) returns 480 lines");
        ReleaseDC(full, dc);
        ChangeDisplaySettingsA(nullptr, 0);
        DestroyWindow(full);
    }

    DeleteDC(mem);
    DeleteObject(dib);
    return Result();
}

}  // namespace probe
