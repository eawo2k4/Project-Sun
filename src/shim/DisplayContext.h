#pragma once

// Shared state of the display sandbox: the game's virtual display mode, the
// windows being presented in the sandbox ("managed" windows), cursor
// confinement, and the "is this call from game code?" filter.
//
// Used by hooks/DisplayHooks.cpp (modes, windows, input) and
// hooks/RenderHooks.cpp (DC scaling, frame pacing).
//
// Coordinate spaces:
//   virtual  - what the game sees: a screen of mode.width x mode.height with
//              its window at (0,0) filling it, so virtual screen and virtual
//              client coordinates are the same.
//   real     - actual screen coordinates, in the managed window's own DPI
//              awareness context (physical pixels for windows we create).

#include <windows.h>

#include "retro/DisplayMath.h"
#include "retro/ShimProtocol.h"

namespace retro::shim::display {

// Real (un-hooked) entry points the sandbox itself needs. Detours rewrites
// these in place when the hooks attach, so they always reach the OS.
struct RealApi {
    decltype(&::SetWindowPos) SetWindowPos = ::SetWindowPos;
    decltype(&::GetWindowRect) GetWindowRect = ::GetWindowRect;
    decltype(&::GetClientRect) GetClientRect = ::GetClientRect;
    decltype(&::ClientToScreen) ClientToScreen = ::ClientToScreen;
    decltype(&::ClipCursor) ClipCursor = ::ClipCursor;
    decltype(&::GetCursorPos) GetCursorPos = ::GetCursorPos;
    decltype(&::SetWindowLongA) SetWindowLongA = ::SetWindowLongA;
    decltype(&::SetWindowLongW) SetWindowLongW = ::SetWindowLongW;
    decltype(&::EnumDisplaySettingsW) EnumDisplaySettingsW = ::EnumDisplaySettingsW;
};
RealApi& Real();

// Switches the calling thread's DPI awareness for the lifetime of the scope.
class DpiScope {
public:
    explicit DpiScope(DPI_AWARENESS_CONTEXT context)
        : previous_(context ? SetThreadDpiAwarenessContext(context) : nullptr) {}
    ~DpiScope() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

struct ManagedView {
    HWND hwnd = nullptr;
    Size virt;       // the game's virtual screen/client size
    Rect client;     // real client rectangle (screen coordinates)
    Rect window;     // real window rectangle (screen coordinates)
    Rect viewport;   // where the virtual screen is drawn (screen coordinates)
    DPI_AWARENESS_CONTEXT dpi = nullptr;

    ViewportMap Map() const { return {viewport, virt}; }
};

void Configure(const ShimConfig& config);
bool Windowed();
const ScalingOptions& Scaling();

// --- Virtual display mode ----------------------------------------------------

bool GetVirtualMode(DisplayMode& out);
DisplayMode RealMode();      // current real mode of the primary display
Size PrimaryMonitorSize();   // physical pixels

// Activates `mode`: adopts fullscreen-looking windows, relayouts managed ones
// and notifies the game's windows with WM_DISPLAYCHANGE.
void SetVirtualMode(const DisplayMode& mode);
// Back to the desktop mode: windows are released (styles restored).
void ClearVirtualMode();

// --- Managed windows -------------------------------------------------------------

bool AnyManaged();
bool FindManaged(HWND hwnd, ManagedView& out);
bool FirstManaged(ManagedView& out);

// Starts presenting `hwnd` in the sandbox (requires an active virtual mode).
bool Manage(HWND hwnd);

// For SetWindowPos/MoveWindow: adopt `hwnd` if the game is sizing it to cover
// the virtual screen. Returns true if the window is (now) managed.
bool AdoptIfFullscreen(HWND hwnd, const Rect& requestedVirtualRect);

// A game replacing GWL_WNDPROC on a subclassed window: keeps our subclass on
// top and returns what the game should see as the previous procedure.
// Returns false if `hwnd` isn't subclassed (caller should pass through).
bool ReplaceGameWndProc(HWND hwnd, LONG newProc, bool ansi, LONG& previous);

// --- Cursor ------------------------------------------------------------------------

void SetGameClip(const RECT* virtualRect);  // nullptr = no clip
bool GetGameClip(Rect& virtualRect);
void SetCursorHidden(bool hidden);

// Applies the effective clip for the current focus state. `active` is 1/0
// when known from an activation message, -1 to query the foreground window.
void UpdateCursorClip(int active);

// --- Caller filter ------------------------------------------------------------------

// True if `returnAddress` lies in a module under the Windows directory (or in
// RetroShim itself). Virtualization only applies to game code: DirectDraw,
// Direct3D and user32 internals keep seeing the real system.
bool IsSystemCaller(const void* returnAddress);

}  // namespace retro::shim::display
