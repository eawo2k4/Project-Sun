// Display sandbox: display modes, window containment, cursor and input
// coordinates.
//
//   ChangeDisplaySettings(Ex)A/W  record a virtual mode instead of changing the
//                                 real one (the desktop and other monitors are
//                                 never touched)
//   EnumDisplaySettings(Ex)A/W    report the virtual mode; offer classic
//                                 8/16/32-bit modes modern drivers dropped
//   GetSystemMetrics, GetDeviceCaps
//                                 screen size/depth as the game expects
//   CreateWindowExA/W, SetWindowPos, MoveWindow, SetWindowLongA/W
//                                 fullscreen-looking windows become sandboxed
//                                 (borderless or --windowed, 4:3, integer
//                                 scaled) and the game can't move them off
//   GetWindowRect, GetClientRect, ClientToScreen, ScreenToClient
//                                 the window sits at (0,0) with the virtual size
//   GetCursorPos, SetCursorPos, ClipCursor, GetClipCursor, ShowCursor
//                                 cursor in virtual coordinates, confined to
//                                 the viewport only while the game has focus
//   GetMessageA/W, PeekMessageA/W mouse message coordinates -> virtual
//
// Everything is filtered through IsSystemCaller: only game code sees the
// virtual world. DirectDraw/Direct3D keep driving the real display, which is
// why exclusive-mode DirectX games aren't sandboxed until the graphics
// wrapper exists.

#include <intrin.h>
#include <windowsx.h>

#include "DisplayContext.h"
#include "Hooks.h"
#include "Log.h"
#include "ShimState.h"
#include "gfx/Graphics.h"

namespace retro::shim {
namespace {

using namespace display;

decltype(&ChangeDisplaySettingsA) Real_ChangeDisplaySettingsA = ChangeDisplaySettingsA;
decltype(&ChangeDisplaySettingsW) Real_ChangeDisplaySettingsW = ChangeDisplaySettingsW;
decltype(&ChangeDisplaySettingsExA) Real_ChangeDisplaySettingsExA = ChangeDisplaySettingsExA;
decltype(&ChangeDisplaySettingsExW) Real_ChangeDisplaySettingsExW = ChangeDisplaySettingsExW;
decltype(&EnumDisplaySettingsA) Real_EnumDisplaySettingsA = EnumDisplaySettingsA;
decltype(&EnumDisplaySettingsExA) Real_EnumDisplaySettingsExA = EnumDisplaySettingsExA;
decltype(&EnumDisplaySettingsExW) Real_EnumDisplaySettingsExW = EnumDisplaySettingsExW;
decltype(&GetSystemMetrics) Real_GetSystemMetrics = GetSystemMetrics;
decltype(&GetDeviceCaps) Real_GetDeviceCaps = GetDeviceCaps;
decltype(&CreateWindowExA) Real_CreateWindowExA = CreateWindowExA;
decltype(&CreateWindowExW) Real_CreateWindowExW = CreateWindowExW;
decltype(&MoveWindow) Real_MoveWindow = MoveWindow;
decltype(&ScreenToClient) Real_ScreenToClient = ScreenToClient;
decltype(&SetCursorPos) Real_SetCursorPos = SetCursorPos;
decltype(&GetClipCursor) Real_GetClipCursor = GetClipCursor;
decltype(&ShowCursor) Real_ShowCursor = ShowCursor;
decltype(&GetMessageA) Real_GetMessageA = GetMessageA;
decltype(&GetMessageW) Real_GetMessageW = GetMessageW;
decltype(&PeekMessageA) Real_PeekMessageA = PeekMessageA;
decltype(&PeekMessageW) Real_PeekMessageW = PeekMessageW;

bool IsGame(void* returnAddress) { return !IsSystemCaller(returnAddress); }

Size VirtualSize(const DisplayMode& m) {
    return {static_cast<int32_t>(m.width), static_cast<int32_t>(m.height)};
}

// --- Display modes -------------------------------------------------------------------

template <class DevMode>
LONG SandboxModeChange(const char* api, const DevMode* dm, DWORD flags) {
    if (!dm) {
        // (NULL, 0) restores the registry mode: leave the virtual mode.
        if (!(flags & CDS_TEST)) {
            log::Write("%s(NULL): restore", api);
            ClearVirtualMode();
        }
        return DISP_CHANGE_SUCCESSFUL;
    }

    ModeRequest req;
    req.hasWidth = (dm->dmFields & DM_PELSWIDTH) != 0;
    req.hasHeight = (dm->dmFields & DM_PELSHEIGHT) != 0;
    req.hasBitsPerPixel = (dm->dmFields & DM_BITSPERPEL) != 0;
    req.hasRefresh = (dm->dmFields & DM_DISPLAYFREQUENCY) != 0;
    req.width = dm->dmPelsWidth;
    req.height = dm->dmPelsHeight;
    req.bitsPerPixel = dm->dmBitsPerPel;
    req.refreshHz = dm->dmDisplayFrequency;

    DisplayMode current;
    if (!GetVirtualMode(current)) current = RealMode();

    DisplayMode target;
    if (!ResolveModeRequest(req, current, target)) {
        log::Write("%s: %ux%u %u bpp rejected (DISP_CHANGE_BADMODE)", api, target.width,
                   target.height, target.bitsPerPixel);
        return DISP_CHANGE_BADMODE;
    }
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;

    log::Write("%s: %ux%u %u bpp %u Hz (flags 0x%lX) -> sandboxed", api, target.width,
               target.height, target.bitsPerPixel, target.refreshHz, flags);
    SetVirtualMode(target);
    return DISP_CHANGE_SUCCESSFUL;
}

LONG WINAPI Hook_ChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    if (!IsGame(_ReturnAddress())) return Real_ChangeDisplaySettingsA(dm, flags);
    return SandboxModeChange("ChangeDisplaySettingsA", dm, flags);
}

LONG WINAPI Hook_ChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags) {
    if (!IsGame(_ReturnAddress())) return Real_ChangeDisplaySettingsW(dm, flags);
    return SandboxModeChange("ChangeDisplaySettingsW", dm, flags);
}

LONG WINAPI Hook_ChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* dm, HWND hwnd, DWORD flags,
                                          LPVOID param) {
    if (!IsGame(_ReturnAddress()))
        return Real_ChangeDisplaySettingsExA(device, dm, hwnd, flags, param);
    return SandboxModeChange("ChangeDisplaySettingsExA", dm, flags);
}

LONG WINAPI Hook_ChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* dm, HWND hwnd, DWORD flags,
                                          LPVOID param) {
    if (!IsGame(_ReturnAddress()))
        return Real_ChangeDisplaySettingsExW(device, dm, hwnd, flags, param);
    return SandboxModeChange("ChangeDisplaySettingsExW", dm, flags);
}

template <class DevMode>
void WriteMode(DevMode* dm, const DisplayMode& m) {
    dm->dmPelsWidth = m.width;
    dm->dmPelsHeight = m.height;
    dm->dmBitsPerPel = m.bitsPerPixel;
    dm->dmDisplayFrequency = m.refreshHz;
    dm->dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
}

// `fillCurrent` fills *dm with the real current mode (so dmSize, device name
// and the other fields are valid), then the mode fields are overwritten.
template <class DevMode, class FillFn>
BOOL SandboxEnumMode(DWORD modeNum, DevMode* dm, FillFn fillCurrent) {
    if (modeNum == ENUM_CURRENT_SETTINGS || modeNum == ENUM_REGISTRY_SETTINGS) {
        if (!fillCurrent()) return FALSE;
        DisplayMode v;
        if (GetVirtualMode(v)) WriteMode(dm, v);
        return TRUE;
    }
    const std::vector<DisplayMode> modes = ClassicModeList(PrimaryMonitorSize());
    if (modeNum >= modes.size()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    if (!fillCurrent()) return FALSE;
    WriteMode(dm, modes[modeNum]);
    return TRUE;
}

BOOL WINAPI Hook_EnumDisplaySettingsA(LPCSTR device, DWORD modeNum, DEVMODEA* dm) {
    if (!dm || !IsGame(_ReturnAddress())) return Real_EnumDisplaySettingsA(device, modeNum, dm);
    return SandboxEnumMode(modeNum, dm, [&] {
        return Real_EnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, dm);
    });
}

BOOL WINAPI Hook_EnumDisplaySettingsW(LPCWSTR device, DWORD modeNum, DEVMODEW* dm) {
    if (!dm || !IsGame(_ReturnAddress())) return Real().EnumDisplaySettingsW(device, modeNum, dm);
    return SandboxEnumMode(modeNum, dm, [&] {
        return Real().EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, dm);
    });
}

BOOL WINAPI Hook_EnumDisplaySettingsExA(LPCSTR device, DWORD modeNum, DEVMODEA* dm, DWORD flags) {
    if (!dm || !IsGame(_ReturnAddress()))
        return Real_EnumDisplaySettingsExA(device, modeNum, dm, flags);
    return SandboxEnumMode(modeNum, dm, [&] {
        return Real_EnumDisplaySettingsExA(device, ENUM_CURRENT_SETTINGS, dm, flags);
    });
}

BOOL WINAPI Hook_EnumDisplaySettingsExW(LPCWSTR device, DWORD modeNum, DEVMODEW* dm, DWORD flags) {
    if (!dm || !IsGame(_ReturnAddress()))
        return Real_EnumDisplaySettingsExW(device, modeNum, dm, flags);
    return SandboxEnumMode(modeNum, dm, [&] {
        return Real_EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, dm, flags);
    });
}

int WINAPI Hook_GetSystemMetrics(int index) {
    switch (index) {
    case SM_CXSCREEN: case SM_CYSCREEN:
    case SM_CXFULLSCREEN: case SM_CYFULLSCREEN:
    case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN:
    case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN:
    case SM_CMONITORS:
        break;
    default:
        return Real_GetSystemMetrics(index);
    }

    DisplayMode m;
    if (!GetVirtualMode(m) || !IsGame(_ReturnAddress())) return Real_GetSystemMetrics(index);
    switch (index) {
    case SM_XVIRTUALSCREEN: case SM_YVIRTUALSCREEN: return 0;
    case SM_CMONITORS: return 1;
    case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN:
        return static_cast<int>(m.width);
    default:
        return static_cast<int>(m.height);
    }
}

int WINAPI Hook_GetDeviceCaps(HDC hdc, int index) {
    if (index != HORZRES && index != VERTRES && index != BITSPIXEL && index != VREFRESH)
        return Real_GetDeviceCaps(hdc, index);

    DisplayMode m;
    if (!GetVirtualMode(m) || !IsGame(_ReturnAddress()) || GetObjectType(hdc) != OBJ_DC ||
        Real_GetDeviceCaps(hdc, TECHNOLOGY) != DT_RASDISPLAY) {
        return Real_GetDeviceCaps(hdc, index);  // memory DCs, printers, system callers
    }
    switch (index) {
    case HORZRES: return static_cast<int>(m.width);
    case VERTRES: return static_cast<int>(m.height);
    case BITSPIXEL: return static_cast<int>(m.bitsPerPixel);
    default: return m.refreshHz > 1 ? static_cast<int>(m.refreshHz) : 60;
    }
}

// --- Windows --------------------------------------------------------------------------

template <class Char, class RealFn>
HWND SandboxCreateWindow(RealFn real, void* returnAddress, DWORD exStyle, const Char* className,
                         const Char* windowName, DWORD style, int x, int y, int w, int h,
                         HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    DisplayMode m;
    const bool candidate =
        GetVirtualMode(m) && x != CW_USEDEFAULT && parent != HWND_MESSAGE &&
        LooksLikeFullscreenWindow(style, exStyle, Rect{x, y, x + w, y + h}, VirtualSize(m)) &&
        IsGame(returnAddress);
    if (!candidate) {
        return real(exStyle, className, windowName, style, x, y, w, h, parent, menu, instance,
                    param);
    }

    // Create per-monitor DPI aware so Windows doesn't bitmap-stretch the
    // window: our integer scaling only stays crisp in physical pixels. Create
    // it hidden, lay it out, then show it where it belongs.
    HWND hwnd;
    {
        DpiScope scope(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        hwnd = real(exStyle, className, windowName, style & ~WS_VISIBLE, x, y, w, h, parent, menu,
                    instance, param);
    }
    if (hwnd) {
        Manage(hwnd);
        if (style & WS_VISIBLE) ShowWindow(hwnd, SW_SHOW);
    }
    return hwnd;
}

HWND WINAPI Hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y,
                                 int w, int h, HWND parent, HMENU menu, HINSTANCE inst,
                                 LPVOID param) {
    return SandboxCreateWindow(Real_CreateWindowExA, _ReturnAddress(), ex, cls, name, style, x, y,
                               w, h, parent, menu, inst, param);
}

HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                 int w, int h, HWND parent, HMENU menu, HINSTANCE inst,
                                 LPVOID param) {
    return SandboxCreateWindow(Real_CreateWindowExW, _ReturnAddress(), ex, cls, name, style, x, y,
                               w, h, parent, menu, inst, param);
}

// Requested geometry in the game's (virtual) coordinates, filling in the
// parts SWP_NOMOVE / SWP_NOSIZE leave out from the window's current rect.
Rect RequestedRect(HWND hwnd, int x, int y, int cx, int cy, UINT flags) {
    RECT current{};
    Real().GetWindowRect(hwnd, &current);
    if (flags & SWP_NOMOVE) {
        x = current.left;
        y = current.top;
    }
    if (flags & SWP_NOSIZE) {
        cx = current.right - current.left;
        cy = current.bottom - current.top;
    }
    return {x, y, x + cx, y + cy};
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND insertAfter, int x, int y, int cx, int cy,
                              UINT flags) {
    DisplayMode m;
    if (!GetVirtualMode(m) || !IsGame(_ReturnAddress()))
        return Real().SetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);

    if (!AdoptIfFullscreen(hwnd, RequestedRect(hwnd, x, y, cx, cy, flags)))
        return Real().SetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);

    // Managed: our layout owns position and size. Z-order, show/hide and
    // activation requests still go through (topmost is dropped, see
    // SandboxExStyle).
    if (insertAfter == HWND_TOPMOST) insertAfter = HWND_TOP;
    return Real().SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
}

BOOL WINAPI Hook_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint) {
    DisplayMode m;
    if (!GetVirtualMode(m) || !IsGame(_ReturnAddress()) ||
        !AdoptIfFullscreen(hwnd, Rect{x, y, x + w, y + h})) {
        return Real_MoveWindow(hwnd, x, y, w, h, repaint);
    }
    if (repaint) InvalidateRect(hwnd, nullptr, TRUE);
    return TRUE;
}

LONG SandboxSetWindowLong(HWND hwnd, int index, LONG value, bool ansi) {
    if (index == GWL_WNDPROC) {
        LONG previous = 0;
        if (ReplaceGameWndProc(hwnd, value, ansi, previous)) return previous;
    } else if (index == GWL_STYLE || index == GWL_EXSTYLE) {
        ManagedView v;
        if (FindManaged(hwnd, v)) {
            const auto requested = static_cast<uint32_t>(value);
            value = static_cast<LONG>(index == GWL_STYLE ? SandboxStyle(requested, Windowed())
                                                         : SandboxExStyle(requested, Windowed()));
        }
    }
    return ansi ? Real().SetWindowLongA(hwnd, index, value)
                : Real().SetWindowLongW(hwnd, index, value);
}

LONG WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG value) {
    if (!IsGame(_ReturnAddress())) return Real().SetWindowLongA(hwnd, index, value);
    return SandboxSetWindowLong(hwnd, index, value, true);
}

LONG WINAPI Hook_SetWindowLongW(HWND hwnd, int index, LONG value) {
    if (!IsGame(_ReturnAddress())) return Real().SetWindowLongW(hwnd, index, value);
    return SandboxSetWindowLong(hwnd, index, value, false);
}

BOOL WINAPI Hook_GetWindowRect(HWND hwnd, LPRECT rect) {
    ManagedView v;
    if (rect && FindManaged(hwnd, v) && IsGame(_ReturnAddress())) {
        *rect = {0, 0, v.virt.w, v.virt.h};
        return TRUE;
    }
    return Real().GetWindowRect(hwnd, rect);
}

BOOL WINAPI Hook_GetClientRect(HWND hwnd, LPRECT rect) {
    ManagedView v;
    if (rect && FindManaged(hwnd, v) && IsGame(_ReturnAddress())) {
        *rect = {0, 0, v.virt.w, v.virt.h};
        return TRUE;
    }
    return Real().GetClientRect(hwnd, rect);
}

// The sandboxed window sits at the virtual origin: client == screen.
BOOL WINAPI Hook_ClientToScreen(HWND hwnd, LPPOINT pt) {
    ManagedView v;
    if (pt && FindManaged(hwnd, v) && IsGame(_ReturnAddress())) return TRUE;
    return Real().ClientToScreen(hwnd, pt);
}

BOOL WINAPI Hook_ScreenToClient(HWND hwnd, LPPOINT pt) {
    ManagedView v;
    if (pt && FindManaged(hwnd, v) && IsGame(_ReturnAddress())) return TRUE;
    return Real_ScreenToClient(hwnd, pt);
}

// --- Cursor -------------------------------------------------------------------------------

BOOL WINAPI Hook_GetCursorPos(LPPOINT pt) {
    ManagedView v;
    if (!pt || !FirstManaged(v) || !IsGame(_ReturnAddress())) return Real().GetCursorPos(pt);

    POINT real{};
    {
        DpiScope scope(v.dpi);
        if (!Real().GetCursorPos(&real)) return FALSE;
    }
    const Point p = v.Map().ToVirtual({real.x, real.y});
    *pt = {p.x, p.y};
    return TRUE;
}

BOOL WINAPI Hook_SetCursorPos(int x, int y) {
    ManagedView v;
    if (!FirstManaged(v) || !IsGame(_ReturnAddress())) return Real_SetCursorPos(x, y);

    const Point p = v.Map().ToRealCenter({x, y});
    DpiScope scope(v.dpi);
    return Real_SetCursorPos(p.x, p.y);
}

BOOL WINAPI Hook_ClipCursor(const RECT* rect) {
    if (!AnyManaged() || !IsGame(_ReturnAddress())) return Real().ClipCursor(rect);
    SetGameClip(rect);
    UpdateCursorClip(-1);
    return TRUE;
}

BOOL WINAPI Hook_GetClipCursor(LPRECT rect) {
    ManagedView v;
    if (!rect || !FirstManaged(v) || !IsGame(_ReturnAddress())) return Real_GetClipCursor(rect);
    Rect clip;
    if (!GetGameClip(clip)) clip = {0, 0, v.virt.w, v.virt.h};
    *rect = {clip.left, clip.top, clip.right, clip.bottom};
    return TRUE;
}

int WINAPI Hook_ShowCursor(BOOL show) {
    const int count = Real_ShowCursor(show);
    if (IsGame(_ReturnAddress())) {
        SetCursorHidden(count < 0);
        if (AnyManaged()) UpdateCursorClip(-1);
    }
    return count;
}

// --- Input messages -------------------------------------------------------------------------

// Rewrites mouse coordinates of messages for a managed window into the
// virtual space. Done at retrieval, not in the window procedure, because many
// games read mouse positions straight out of the MSG in their message loop.
void TranslateInput(MSG& msg) {
    ManagedView v;
    if (!FindManaged(msg.hwnd, v)) return;
    const ViewportMap map = v.Map();

    if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) {
        Point screen{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
        const bool screenCoords = msg.message == WM_MOUSEWHEEL || msg.message == WM_MOUSEHWHEEL;
        if (!screenCoords) {
            screen.x += v.client.left;
            screen.y += v.client.top;
        }
        const Point p = map.ToVirtual(screen);
        msg.lParam = MAKELPARAM(p.x, p.y);
    }
    const Point pt = map.ToVirtual({msg.pt.x, msg.pt.y});
    msg.pt = {pt.x, pt.y};
}

BOOL WINAPI Hook_GetMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max) {
    const BOOL r = Real_GetMessageA(msg, hwnd, min, max);
    if (r > 0 && AnyManaged() && IsGame(_ReturnAddress())) TranslateInput(*msg);
    gfx::PresentPendingFrames();  // flush DirectDraw primary updates
    return r;
}

BOOL WINAPI Hook_GetMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max) {
    const BOOL r = Real_GetMessageW(msg, hwnd, min, max);
    if (r > 0 && AnyManaged() && IsGame(_ReturnAddress())) TranslateInput(*msg);
    gfx::PresentPendingFrames();  // flush DirectDraw primary updates
    return r;
}

BOOL WINAPI Hook_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    const BOOL r = Real_PeekMessageA(msg, hwnd, min, max, remove);
    if (r && AnyManaged() && IsGame(_ReturnAddress())) TranslateInput(*msg);
    gfx::PresentPendingFrames();  // flush DirectDraw primary updates
    return r;
}

BOOL WINAPI Hook_PeekMessageW(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    const BOOL r = Real_PeekMessageW(msg, hwnd, min, max, remove);
    if (r && AnyManaged() && IsGame(_ReturnAddress())) TranslateInput(*msg);
    gfx::PresentPendingFrames();  // flush DirectDraw primary updates
    return r;
}

LONG ApplyHooks(bool attach) {
    RealApi& real = Real();
    LONG err = NO_ERROR;
    HookStep(err, attach, Real_ChangeDisplaySettingsA, Hook_ChangeDisplaySettingsA);
    HookStep(err, attach, Real_ChangeDisplaySettingsW, Hook_ChangeDisplaySettingsW);
    HookStep(err, attach, Real_ChangeDisplaySettingsExA, Hook_ChangeDisplaySettingsExA);
    HookStep(err, attach, Real_ChangeDisplaySettingsExW, Hook_ChangeDisplaySettingsExW);
    HookStep(err, attach, Real_EnumDisplaySettingsA, Hook_EnumDisplaySettingsA);
    HookStep(err, attach, real.EnumDisplaySettingsW, Hook_EnumDisplaySettingsW);
    HookStep(err, attach, Real_EnumDisplaySettingsExA, Hook_EnumDisplaySettingsExA);
    HookStep(err, attach, Real_EnumDisplaySettingsExW, Hook_EnumDisplaySettingsExW);
    HookStep(err, attach, Real_GetSystemMetrics, Hook_GetSystemMetrics);
    HookStep(err, attach, Real_GetDeviceCaps, Hook_GetDeviceCaps);
    HookStep(err, attach, Real_CreateWindowExA, Hook_CreateWindowExA);
    HookStep(err, attach, Real_CreateWindowExW, Hook_CreateWindowExW);
    HookStep(err, attach, real.SetWindowPos, Hook_SetWindowPos);
    HookStep(err, attach, Real_MoveWindow, Hook_MoveWindow);
    HookStep(err, attach, real.SetWindowLongA, Hook_SetWindowLongA);
    HookStep(err, attach, real.SetWindowLongW, Hook_SetWindowLongW);
    HookStep(err, attach, real.GetWindowRect, Hook_GetWindowRect);
    HookStep(err, attach, real.GetClientRect, Hook_GetClientRect);
    HookStep(err, attach, real.ClientToScreen, Hook_ClientToScreen);
    HookStep(err, attach, Real_ScreenToClient, Hook_ScreenToClient);
    HookStep(err, attach, real.GetCursorPos, Hook_GetCursorPos);
    HookStep(err, attach, Real_SetCursorPos, Hook_SetCursorPos);
    HookStep(err, attach, real.ClipCursor, Hook_ClipCursor);
    HookStep(err, attach, Real_GetClipCursor, Hook_GetClipCursor);
    HookStep(err, attach, Real_ShowCursor, Hook_ShowCursor);
    HookStep(err, attach, Real_GetMessageA, Hook_GetMessageA);
    HookStep(err, attach, Real_GetMessageW, Hook_GetMessageW);
    HookStep(err, attach, Real_PeekMessageA, Hook_PeekMessageA);
    HookStep(err, attach, Real_PeekMessageW, Hook_PeekMessageW);
    return err;
}

}  // namespace

LONG AttachDisplayHooks() {
    Configure(Config());
    log::Write("display: %s, %s scaling", Windowed() ? "windowed" : "borderless fullscreen",
               Scaling().integerScaling ? "integer" : "fractional");
    return ApplyHooks(true);
}

LONG DetachDisplayHooks() { return ApplyHooks(false); }

}  // namespace retro::shim
