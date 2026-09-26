// Presentation layer: GDI scaling for sandboxed windows and frame pacing.
//
// Scaling: DCs the game obtains for a managed window (GetDC, GetDCEx,
// GetWindowDC, BeginPaint, and the screen DC in fullscreen) get a GDI world
// transform that maps the virtual screen onto the viewport, plus a clip to
// the viewport. Every GDI call the game makes (BitBlt, StretchDIBits, text,
// shapes) is then scaled by GDI itself, with nearest-neighbour stretching.
// SetDIBitsToDevice is the exception (GDI never scales it) and is rewritten as
// the equivalent StretchDIBits.
//
// Pacing: whole-frame presents (a blit covering >= 75% of the target window,
// or SwapBuffers) wait for the next frame slot of a FrameScheduler.
// DirectDraw/Direct3D presents go through their own interfaces; the graphics
// wrapper (next step) will call the same PaceFrame().

#include <intrin.h>

#include <memory>

#include "DisplayContext.h"
#include "Hooks.h"
#include "Log.h"
#include "ShimState.h"
#include "retro/FramePacing.h"

namespace retro::shim {
namespace {

using namespace display;

decltype(&GetDC) Real_GetDC = GetDC;
decltype(&GetDCEx) Real_GetDCEx = GetDCEx;
decltype(&GetWindowDC) Real_GetWindowDC = GetWindowDC;
decltype(&BeginPaint) Real_BeginPaint = BeginPaint;
decltype(&InvalidateRect) Real_InvalidateRect = InvalidateRect;
decltype(&BitBlt) Real_BitBlt = BitBlt;
decltype(&StretchBlt) Real_StretchBlt = StretchBlt;
decltype(&StretchDIBits) Real_StretchDIBits = StretchDIBits;
decltype(&SetDIBitsToDevice) Real_SetDIBitsToDevice = SetDIBitsToDevice;
decltype(&SwapBuffers) Real_SwapBuffers = SwapBuffers;

using WglSwapBuffersFn = BOOL WINAPI(HDC);
WglSwapBuffersFn* Real_wglSwapBuffers = nullptr;  // only if opengl32 is loaded at attach

bool g_scalingEnabled = false;
bool g_pacingEnabled = false;
std::unique_ptr<FrameScheduler> g_scheduler;
SRWLOCK g_pacerLock = SRWLOCK_INIT;

bool IsGame(void* returnAddress) { return !IsSystemCaller(returnAddress); }

// --- Frame pacing ----------------------------------------------------------------------

void PaceFrame() {
    thread_local PreciseWaiter waiter;
    const int64_t now = QpcNow();
    int64_t presentAt;
    AcquireSRWLockExclusive(&g_pacerLock);
    presentAt = g_scheduler->NextPresentTime(now);
    ReleaseSRWLockExclusive(&g_pacerLock);
    if (presentAt > now) waiter.WaitUntil(presentAt);
}

// The managed window a DC draws to, if any (the screen DC counts in
// fullscreen, where the game believes it owns the whole screen).
bool ManagedForDc(HDC hdc, ManagedView& v) {
    if (!AnyManaged()) return false;
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd || hwnd == GetDesktopWindow()) return !Windowed() && FirstManaged(v);
    return FindManaged(GetAncestor(hwnd, GA_ROOT), v);
}

// Is a blit of w x h onto `hdc` a whole-frame present?
bool IsPresentBlit(HDC hdc, int w, int h) {
    if (!g_pacingEnabled || GetObjectType(hdc) != OBJ_DC) return false;  // memory DCs etc.

    ManagedView v;
    if (ManagedForDc(hdc, v)) return IsFramePresent(w, h, v.virt.w, v.virt.h);

    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd || hwnd == GetDesktopWindow()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;

    RECT client{};
    Real().GetClientRect(hwnd, &client);
    return IsFramePresent(w, h, client.right, client.bottom);
}

// --- DC scaling ---------------------------------------------------------------------

enum class DcOrigin { Client, Window, Screen };

void ApplyScaling(HDC hdc, const ManagedView& v, DcOrigin origin) {
    if (!hdc) return;
    POINT o{0, 0};
    if (origin == DcOrigin::Client) o = {v.client.left, v.client.top};
    if (origin == DcOrigin::Window) o = {v.window.left, v.window.top};

    const Rect vp = v.viewport;
    SetGraphicsMode(hdc, GM_ADVANCED);
    ModifyWorldTransform(hdc, nullptr, MWT_IDENTITY);
    // With an identity transform logical == device units, so this clips the
    // game to the viewport and keeps the letterbox bars clean.
    IntersectClipRect(hdc, vp.left - o.x, vp.top - o.y, vp.right - o.x, vp.bottom - o.y);

    const ViewportMap map = v.Map();
    const XFORM xf{static_cast<FLOAT>(map.ScaleX()), 0.0f, 0.0f, static_cast<FLOAT>(map.ScaleY()),
                   static_cast<FLOAT>(vp.left - o.x), static_cast<FLOAT>(vp.top - o.y)};
    SetWorldTransform(hdc, &xf);
    SetStretchBltMode(hdc, COLORONCOLOR);  // nearest neighbour: crisp pixels
}

// Applies scaling to a DC the game got for `hwnd` (nullptr = the screen).
void ScaleIfManaged(HDC hdc, HWND hwnd, DcOrigin origin) {
    if (!hdc || !g_scalingEnabled || !AnyManaged()) return;
    ManagedView v;
    if (!hwnd) {
        if (!Windowed() && FirstManaged(v)) ApplyScaling(hdc, v, DcOrigin::Screen);
    } else if (FindManaged(hwnd, v)) {
        ApplyScaling(hdc, v, origin);
    }
}

HDC WINAPI Hook_GetDC(HWND hwnd) {
    const HDC hdc = Real_GetDC(hwnd);
    if (IsGame(_ReturnAddress())) ScaleIfManaged(hdc, hwnd, DcOrigin::Client);
    return hdc;
}

HDC WINAPI Hook_GetDCEx(HWND hwnd, HRGN clip, DWORD flags) {
    const HDC hdc = Real_GetDCEx(hwnd, clip, flags);
    if (IsGame(_ReturnAddress()))
        ScaleIfManaged(hdc, hwnd, (flags & DCX_WINDOW) ? DcOrigin::Window : DcOrigin::Client);
    return hdc;
}

HDC WINAPI Hook_GetWindowDC(HWND hwnd) {
    const HDC hdc = Real_GetWindowDC(hwnd);
    if (IsGame(_ReturnAddress())) ScaleIfManaged(hdc, hwnd, DcOrigin::Window);
    return hdc;
}

HDC WINAPI Hook_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
    const HDC hdc = Real_BeginPaint(hwnd, ps);
    ManagedView v;
    if (hdc && g_scalingEnabled && FindManaged(hwnd, v) && IsGame(_ReturnAddress())) {
        ApplyScaling(hdc, v, DcOrigin::Client);
        ps->rcPaint = {0, 0, v.virt.w, v.virt.h};  // see Hook_InvalidateRect
    }
    return hdc;
}

// A partial invalidation in virtual coordinates would clip the (scaled)
// repaint to the wrong region; repaint the whole window instead.
BOOL WINAPI Hook_InvalidateRect(HWND hwnd, const RECT* rect, BOOL erase) {
    ManagedView v;
    if (rect && g_scalingEnabled && FindManaged(hwnd, v) && IsGame(_ReturnAddress()))
        rect = nullptr;
    return Real_InvalidateRect(hwnd, rect, erase);
}

// --- Presentation paths --------------------------------------------------------------------

BOOL WINAPI Hook_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    if (g_pacingEnabled && IsGame(_ReturnAddress()) && IsPresentBlit(dst, w, h)) PaceFrame();
    return Real_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

BOOL WINAPI Hook_StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw,
                            int sh, DWORD rop) {
    if (g_pacingEnabled && IsGame(_ReturnAddress()) && IsPresentBlit(dst, w, h)) PaceFrame();
    return Real_StretchBlt(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}

int WINAPI Hook_StretchDIBits(HDC hdc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                              const VOID* bits, const BITMAPINFO* bmi, UINT usage, DWORD rop) {
    if (g_pacingEnabled && IsGame(_ReturnAddress()) && IsPresentBlit(hdc, w, h)) PaceFrame();
    return Real_StretchDIBits(hdc, x, y, w, h, sx, sy, sw, sh, bits, bmi, usage, rop);
}

// Whole-image SetDIBitsToDevice, the only form where the StretchDIBits
// rewrite is unambiguous (banded uploads keep their original behaviour).
bool IsWholeImage(int w, int h, int sx, int sy, UINT startScan, UINT lines, const BITMAPINFO* bmi) {
    if (!bmi || bmi->bmiHeader.biSize < sizeof(BITMAPINFOHEADER)) return false;
    const LONG height = bmi->bmiHeader.biHeight < 0 ? -bmi->bmiHeader.biHeight
                                                    : bmi->bmiHeader.biHeight;
    return sx == 0 && sy == 0 && startScan == 0 && static_cast<LONG>(lines) == height &&
           w == bmi->bmiHeader.biWidth && h == height;
}

int WINAPI Hook_SetDIBitsToDevice(HDC hdc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                  UINT startScan, UINT lines, const VOID* bits,
                                  const BITMAPINFO* bmi, UINT usage) {
    if (!IsGame(_ReturnAddress()))
        return Real_SetDIBitsToDevice(hdc, x, y, w, h, sx, sy, startScan, lines, bits, bmi, usage);

    const int iw = static_cast<int>(w), ih = static_cast<int>(h);
    if (IsPresentBlit(hdc, iw, ih)) PaceFrame();

    ManagedView v;
    if (g_scalingEnabled && ManagedForDc(hdc, v) &&
        IsWholeImage(iw, ih, sx, sy, startScan, lines, bmi)) {
        const int r = Real_StretchDIBits(hdc, x, y, iw, ih, 0, 0, iw, ih, bits, bmi, usage, SRCCOPY);
        return r > 0 ? static_cast<int>(lines) : 0;
    }
    return Real_SetDIBitsToDevice(hdc, x, y, w, h, sx, sy, startScan, lines, bits, bmi, usage);
}

BOOL WINAPI Hook_SwapBuffers(HDC hdc) {
    if (g_pacingEnabled && IsGame(_ReturnAddress())) PaceFrame();
    return Real_SwapBuffers(hdc);
}

BOOL WINAPI Hook_wglSwapBuffers(HDC hdc) {
    // gdi32!SwapBuffers calling down into opengl32 is a system caller, so a
    // frame presented through SwapBuffers is only paced once.
    if (g_pacingEnabled && IsGame(_ReturnAddress())) PaceFrame();
    return Real_wglSwapBuffers(hdc);
}

LONG ApplyHooks(bool attach) {
    LONG err = NO_ERROR;
    HookStep(err, attach, Real_GetDC, Hook_GetDC);
    HookStep(err, attach, Real_GetDCEx, Hook_GetDCEx);
    HookStep(err, attach, Real_GetWindowDC, Hook_GetWindowDC);
    HookStep(err, attach, Real_BeginPaint, Hook_BeginPaint);
    HookStep(err, attach, Real_InvalidateRect, Hook_InvalidateRect);
    HookStep(err, attach, Real_BitBlt, Hook_BitBlt);
    HookStep(err, attach, Real_StretchBlt, Hook_StretchBlt);
    HookStep(err, attach, Real_StretchDIBits, Hook_StretchDIBits);
    HookStep(err, attach, Real_SetDIBitsToDevice, Hook_SetDIBitsToDevice);
    HookStep(err, attach, Real_SwapBuffers, Hook_SwapBuffers);
    if (Real_wglSwapBuffers) HookStep(err, attach, Real_wglSwapBuffers, Hook_wglSwapBuffers);
    return err;
}

}  // namespace

LONG AttachRenderHooks() {
    const ShimConfig& config = Config();
    g_scalingEnabled = (config.features & ShimFeature_DisplaySandbox) != 0;
    const uint32_t fps = (config.features & ShimFeature_FrameLimiter) ? config.fpsCap : 0;
    g_pacingEnabled = fps > 0;
    g_scheduler = std::make_unique<FrameScheduler>(fps, QpcFrequency());

    if (HMODULE gl = GetModuleHandleW(L"opengl32.dll")) {
        Real_wglSwapBuffers =
            reinterpret_cast<WglSwapBuffersFn*>(GetProcAddress(gl, "wglSwapBuffers"));
    }

    const PreciseWaiter probe;
    log::Write("render: GDI scaling %s; pacing %s%u fps (%s timer)%s",
               g_scalingEnabled ? "on" : "off", g_pacingEnabled ? "" : "off, cap ", fps,
               probe.HighResolution() ? "high-resolution" : "legacy",
               Real_wglSwapBuffers ? ", wglSwapBuffers hooked" : "");
    return ApplyHooks(true);
}

LONG DetachRenderHooks() { return ApplyHooks(false); }

}  // namespace retro::shim
