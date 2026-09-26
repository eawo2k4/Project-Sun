#pragma once

// Containment shared by the Direct3D 8 and 9 hooks. D3DPRESENT_PARAMETERS
// (9) and D3DPRESENT_PARAMETERS8 share the field names used here; D3D8 adds
// FullScreen_PresentationInterval, which must be reset for windowed devices.

#include <windows.h>

#include <d3d9.h>

#include "../DisplayContext.h"
#include "retro/DisplayMath.h"

namespace retro::shim::gfx {

inline uint32_t BitsOf(D3DFORMAT format) {
    switch (format) {
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X4R4G4B4: return 16;
    case D3DFMT_R8G8B8: return 24;
    case D3DFMT_P8: return 8;
    default: return 32;
    }
}

inline D3DFORMAT FormatForBits(uint32_t bits) {
    return bits == 16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
}

// Reports the sandbox's virtual mode through a D3DDISPLAYMODE (same in 8 and 9).
inline void WriteVirtualMode(D3DDISPLAYMODE* mode) {
    DisplayMode m;
    if (!mode || !display::GetVirtualMode(m)) return;
    mode->Width = m.width;
    mode->Height = m.height;
    mode->RefreshRate = m.refreshHz > 1 ? m.refreshHz : 60;
    mode->Format = FormatForBits(m.bitsPerPixel);
}

// Fullscreen presentation parameters -> windowed inside the sandbox: the back
// buffer becomes the virtual mode and the device window is managed.
// Returns the device window.
template <class PresentParameters>
HWND ContainFullscreen(PresentParameters& pp, HWND focus) {
    DisplayMode mode{pp.BackBufferWidth, pp.BackBufferHeight, BitsOf(pp.BackBufferFormat),
                     pp.FullScreen_RefreshRateInHz};
    if (!mode.width || !mode.height) {
        const DisplayMode desktop = display::RealMode();
        mode.width = desktop.width;
        mode.height = desktop.height;
    }
    const HWND window = pp.hDeviceWindow ? pp.hDeviceWindow : focus;
    display::SetVirtualMode(mode);
    if (window) display::Manage(window);

    pp.BackBufferWidth = mode.width;
    pp.BackBufferHeight = mode.height;
    pp.Windowed = TRUE;
    pp.FullScreen_RefreshRateInHz = 0;
    if constexpr (requires { pp.FullScreen_PresentationInterval; }) {
        pp.FullScreen_PresentationInterval = 0;  // D3D8: windowed requires DEFAULT
    }
    return window;
}

// For Present on a contained device: paints the letterbox bars and returns
// the viewport (client coordinates) the back buffer should be shown in.
inline bool PrepareViewportPresent(HWND window, RECT& viewport) {
    display::ManagedView v;
    if (!window || !display::FindManaged(window, v)) return false;
    {
        display::DpiScope scope(v.dpi);
        if (HDC dc = GetDC(v.hwnd)) {
            display::FillLetterbox(dc, v);
            ReleaseDC(v.hwnd, dc);
        }
    }
    const Rect vp = v.ClientViewport();
    viewport = {vp.left, vp.top, vp.right, vp.bottom};
    return true;
}

}  // namespace retro::shim::gfx
