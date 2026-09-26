// DirectDraw / Direct3D 9 probes (see ShimProbe.cpp for the modes).
//
// Safety: exclusive fullscreen DirectDraw and fullscreen Direct3D would change
// the real display if the shim weren't intercepting them, so each probe first
// checks RetroShimIsModuleActive for the API ("ddraw"/"d3d9") *and* the
// display sandbox, and refuses to run otherwise. Windows stay hidden.

#include <windows.h>

#include <d3d9.h>
#include <d3d9on12.h>
#include <ddraw.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "ProbeCommon.h"
#include "retro/FramePacing.h"
#include "retro/PixelConvert.h"
#include "retro/ShimProtocol.h"

namespace probe {
namespace {

using retro::QpcFrequency;
using retro::QpcNow;

constexpr DWORD kW = 640, kH = 480;

bool Active(const char* name) {
    using Fn = BOOL(WINAPI*)(const char*);
    const HMODULE shim = GetModuleHandleW(L"RetroShim.dll");
    const auto fn = shim ? reinterpret_cast<Fn>(GetProcAddress(shim, "RetroShimIsModuleActive"))
                         : nullptr;
    return fn && fn(name);
}

retro::PresentStats Stats() {
    using Fn = BOOL(WINAPI*)(retro::PresentStats*);
    retro::PresentStats s;
    const HMODULE shim = GetModuleHandleW(L"RetroShim.dll");
    if (const auto fn = shim ? reinterpret_cast<Fn>(GetProcAddress(shim, "RetroShimGetPresentStats"))
                             : nullptr) {
        fn(&s);
    }
    return s;
}

HWND HiddenPopup(int w, int h) {
    static const ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RetroShimGfxProbe";
        return RegisterClassW(&wc);
    }();
    (void)cls;
    return CreateWindowExW(0, L"RetroShimGfxProbe", L"gfx probe", WS_POPUP, 0, 0, w, h, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

int DesktopWidth() {
    HDC dc = GetDC(nullptr);
    const int w = GetDeviceCaps(dc, DESKTOPHORZRES);
    ReleaseDC(nullptr, dc);
    return w;
}

void ExpectPaced(double seconds, int frames, uint32_t fps, const char* what) {
    std::printf("%s: %d frames in %.1f ms\n", what, frames, seconds * 1000);
    if (fps == 0) {
        Expect(seconds < 0.15, "unpaced (fast)");
    } else {
        const double minimum = (frames - 2) / static_cast<double>(fps);
        const double maximum = (frames + 1) / static_cast<double>(fps) * 1.5;
        Expect(seconds >= minimum * 0.99 && seconds <= maximum, "paced to the frame cap");
    }
}

template <class Fn>
double Time(int frames, Fn fn) {
    const int64_t start = QpcNow();
    for (int i = 0; i < frames; ++i) fn(i);
    return static_cast<double>(QpcNow() - start) / QpcFrequency();
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
}

// --- DirectDraw ------------------------------------------------------------------------------

// Version differences between IDirectDraw (desc v1) and IDirectDraw7 (desc v2).
HRESULT SetMode(IDirectDraw* dd, DWORD w, DWORD h, DWORD bpp) { return dd->SetDisplayMode(w, h, bpp); }
HRESULT SetMode(IDirectDraw7* dd, DWORD w, DWORD h, DWORD bpp) {
    return dd->SetDisplayMode(w, h, bpp, 0, 0);
}

struct ModeSearch {
    DWORD w, h, bpp;
    bool found = false;
};

template <class Desc>
HRESULT WINAPI OnMode(Desc* d, void* ctx) {
    auto* s = static_cast<ModeSearch*>(ctx);
    if (d->dwWidth == s->w && d->dwHeight == s->h && d->ddpfPixelFormat.dwRGBBitCount == s->bpp)
        s->found = true;
    return DDENUMRET_OK;
}

bool HasMode(IDirectDraw* dd, DWORD bpp) {
    ModeSearch s{kW, kH, bpp};
    dd->EnumDisplayModes(0, nullptr, &s, OnMode<DDSURFACEDESC>);
    return s.found;
}
bool HasMode(IDirectDraw7* dd, DWORD bpp) {
    ModeSearch s{kW, kH, bpp};
    dd->EnumDisplayModes(0, nullptr, &s, OnMode<DDSURFACEDESC2>);
    return s.found;
}

// The test image: something every pixel of which depends on x and y.
uint32_t PatternPixel(DWORD bpp, DWORD x, DWORD y) {
    if (bpp == 8) return (x + 2 * y) & 0xFF;
    return ((x & 31) << 11) | ((y & 63) << 5) | ((x ^ y) & 31);  // RGB565
}

// CRC the shim should report for the pattern, computed independently.
uint32_t ExpectedCrc(DWORD bpp, const uint32_t* palette) {
    const DWORD bytes = bpp / 8;
    std::vector<uint8_t> src(kW * kH * bytes);
    for (DWORD y = 0; y < kH; ++y) {
        for (DWORD x = 0; x < kW; ++x) {
            const uint32_t p = PatternPixel(bpp, x, y);
            std::memcpy(&src[(y * kW + x) * bytes], &p, bytes);
        }
    }
    std::vector<uint32_t> bgra(kW * kH);
    retro::ConvertToBgra32(src.data(), static_cast<int32_t>(kW * bytes), retro::FormatForDepth(bpp),
                           palette, kW, kH, bgra.data(), kW);
    return retro::Crc32(bgra.data(), bgra.size() * 4);
}

template <class Surface, class Desc>
bool DrawPattern(Surface* surface, DWORD bpp) {
    Desc d{};
    d.dwSize = sizeof(d);
    if (FAILED(surface->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) return false;
    auto* base = static_cast<uint8_t*>(d.lpSurface);
    const DWORD bytes = bpp / 8;
    for (DWORD y = 0; y < kH; ++y) {
        for (DWORD x = 0; x < kW; ++x) {
            const uint32_t p = PatternPixel(bpp, x, y);
            std::memcpy(base + y * d.lPitch + x * bytes, &p, bytes);
        }
    }
    surface->Unlock(nullptr);
    return true;
}

void MakePalette(PALETTEENTRY (&entries)[256], uint32_t (&bgra)[256], int variant) {
    for (int i = 0; i < 256; ++i) {
        entries[i] = {static_cast<BYTE>(i), static_cast<BYTE>(255 - i),
                      static_cast<BYTE>((i * 7 + variant * 40) & 0xFF), 0};
        bgra[i] = retro::PaletteToBgra(entries[i].peRed, entries[i].peGreen, entries[i].peBlue);
    }
}

template <class DD, class Surface, class Desc>
int RunDirectDraw(DD* dd, DWORD bpp, uint32_t fps) {
    using Caps = decltype(Desc::ddsCaps);
    const int desktopW = DesktopWidth();
    const int metricW = GetSystemMetrics(SM_CXSCREEN);
    HWND hwnd = HiddenPopup(metricW, GetSystemMetrics(SM_CYSCREEN));

    Expect(dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == DD_OK,
           "SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)");
    Expect(HasMode(dd, bpp), "EnumDisplayModes lists 640x480 at the test depth");
    Expect(SetMode(dd, kW, kH, bpp) == DD_OK, "SetDisplayMode(640x480)");
    if (DesktopWidth() != desktopW) {
        Expect(false, "real desktop resolution unchanged");
        dd->RestoreDisplayMode();
        return 1;
    }
    Expect(true, "real desktop resolution unchanged");
    Expect(GetSystemMetrics(SM_CXSCREEN) == static_cast<int>(kW), "GetSystemMetrics sees 640");

    Desc mode{};
    mode.dwSize = sizeof(mode);
    Expect(SUCCEEDED(dd->GetDisplayMode(&mode)) && mode.dwWidth == kW &&
               mode.ddpfPixelFormat.dwRGBBitCount == bpp,
           "GetDisplayMode reports the virtual mode");

    Desc pd{};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    pd.dwBackBufferCount = 1;
    Surface* primary = nullptr;
    Expect(dd->CreateSurface(&pd, &primary, nullptr) == DD_OK && primary,
           "CreateSurface(primary + back buffer)");
    if (!primary) return 1;

    Desc sd{};
    sd.dwSize = sizeof(sd);
    primary->GetSurfaceDesc(&sd);
    Expect(sd.dwWidth == kW && sd.dwHeight == kH && sd.ddpfPixelFormat.dwRGBBitCount == bpp &&
               (sd.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) &&
               ((bpp == 8) == ((sd.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) != 0)),
           "primary is 640x480 at the requested depth");

    Caps backCaps{};
    backCaps.dwCaps = DDSCAPS_BACKBUFFER;
    Surface* back = nullptr;
    Expect(primary->GetAttachedSurface(&backCaps, &back) == DD_OK && back,
           "GetAttachedSurface(BACKBUFFER)");
    if (!back) return 1;
    Caps caps{};
    back->GetCaps(&caps);
    Expect((caps.dwCaps & DDSCAPS_BACKBUFFER) != 0, "back buffer reports DDSCAPS_BACKBUFFER");

    // Offscreen surfaces without an explicit format get the virtual depth.
    Desc od{};
    od.dwSize = sizeof(od);
    od.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    od.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    od.dwWidth = od.dwHeight = 64;
    Surface* offscreen = nullptr;
    Expect(dd->CreateSurface(&od, &offscreen, nullptr) == DD_OK && offscreen, "offscreen surface");
    if (offscreen) {
        Desc check{};
        check.dwSize = sizeof(check);
        offscreen->GetSurfaceDesc(&check);
        Expect(check.ddpfPixelFormat.dwRGBBitCount == bpp,
               "offscreen surface takes the virtual depth, not the desktop's");
    }

    uint32_t paletteBgra[256] = {};
    IDirectDrawPalette* palette = nullptr;
    PALETTEENTRY entries[256];
    if (bpp == 8) {
        MakePalette(entries, paletteBgra, 0);
        Expect(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, &palette, nullptr) ==
                       DD_OK &&
                   primary->SetPalette(palette) == DD_OK,
               "palette attached to the primary");
    }

    Expect(DrawPattern<Surface, Desc>(back, bpp), "test pattern drawn into the back buffer");

    const uint32_t framesBefore = Stats().frames;
    ExpectPaced(Time(20, [&](int) { primary->Flip(nullptr, DDFLIP_WAIT); }), 20, fps, "Flip");
    retro::PresentStats s = Stats();
    const uint32_t expected = ExpectedCrc(bpp, paletteBgra);
    std::printf("presented %u frames, last %ux%u %u bpp crc %08X (expected %08X)\n",
                s.frames - framesBefore, s.width, s.height, s.bitsPerPixel, s.crc32, expected);
    Expect(s.frames - framesBefore == 20, "every Flip presented a frame");
    Expect(s.width == kW && s.height == kH && s.crc32 == expected,
           "presented image == pattern converted to 32-bit (palette applied)");

    // Games often sync on WaitForVerticalBlank and then Flip: one frame, not
    // two. (Unpaced, the real vertical blank depends on the monitor: skip.)
    if (fps > 0) {
        ExpectPaced(Time(20,
                         [&](int) {
                             dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, nullptr);
                             primary->Flip(nullptr, DDFLIP_WAIT);
                         }),
                    20, fps, "WaitForVerticalBlank + Flip");
    }

    if (palette) {
        // A palette change alone (fades) must reach the screen.
        uint32_t newBgra[256];
        MakePalette(entries, newBgra, 1);
        const uint32_t before = Stats().frames;
        palette->SetEntries(0, 0, 256, entries);
        s = Stats();
        Expect(s.frames == before + 1 && s.crc32 == ExpectedCrc(bpp, newBgra),
               "palette SetEntries re-presents with the new colours");
    }

    // A partial blit straight onto the primary is shown by the message pump.
    if (offscreen) {
        Sleep(40);  // let a frame period pass
        const uint32_t before = Stats().frames;
        RECT dst{0, 0, 64, 64};
        primary->Blt(&dst, offscreen, nullptr, DDBLT_WAIT, nullptr);
        PumpMessages();
        Expect(Stats().frames > before, "partial primary update presented");
        offscreen->Release();
    }

    if (palette) palette->Release();
    back->Release();
    primary->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    Expect(GetSystemMetrics(SM_CXSCREEN) == metricW, "desktop metrics back after DDSCL_NORMAL");
    DestroyWindow(hwnd);
    return Result();
}

}  // namespace

int ProbeDirectDraw(uint32_t fps, bool v7) {
    if (!Active("ddraw") || !Active("display")) {
        std::printf("ddraw hooks or display sandbox inactive: refusing exclusive mode\n");
        return 3;
    }
    if (v7) {
        IDirectDraw7* dd = nullptr;
        if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7,
                                      nullptr))) {
            std::printf("DirectDrawCreateEx failed\n");
            return 1;
        }
        const int r = RunDirectDraw<IDirectDraw7, IDirectDrawSurface7, DDSURFACEDESC2>(dd, 16, fps);
        dd->Release();
        return r;
    }
    IDirectDraw* dd = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr))) {
        std::printf("DirectDrawCreate failed\n");
        return 1;
    }
    const int r = RunDirectDraw<IDirectDraw, IDirectDrawSurface, DDSURFACEDESC>(dd, 8, fps);
    dd->Release();
    return r;
}

int ProbeDirect3D9(uint32_t fps, bool on12) {
    // Loaded on demand, like many games do: exercises the DLL-load watcher.
    const HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9 || !Active("d3d9") || !Active("display")) {
        std::printf("d3d9 hooks or display sandbox inactive: refusing a fullscreen device\n");
        return 3;
    }
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) {
        std::printf("Direct3D 9 unavailable on this machine: skipping\n");
        return 0;
    }

    const int desktopW = DesktopWidth();
    HWND hwnd = HiddenPopup(800, 600);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = FALSE;
    pp.BackBufferWidth = 800;
    pp.BackBufferHeight = 600;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.FullScreen_RefreshRateInHz = 60;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    std::printf("CreateDevice(fullscreen 800x600) -> 0x%08lX\n", hr);
    if (FAILED(hr) || !device) {
        d3d->Release();
        std::printf("no Direct3D 9 HAL device on this machine: skipping\n");
        return 0;
    }

    Expect(DesktopWidth() == desktopW, "real desktop resolution unchanged");
    Expect(GetSystemMetrics(SM_CXSCREEN) == 800, "GetSystemMetrics sees the 800x600 mode");
    RECT client{};
    GetClientRect(hwnd, &client);
    Expect(client.right == 800 && client.bottom == 600, "device window is sandboxed (800x600)");
    D3DDISPLAYMODE mode{};
    Expect(SUCCEEDED(device->GetDisplayMode(0, &mode)) && mode.Width == 800 && mode.Height == 600,
           "GetDisplayMode reports the virtual mode");

    IUnknown* on12Device = nullptr;
    const bool isOn12 =
        SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9On12),
                                         reinterpret_cast<void**>(&on12Device)));
    if (on12Device) on12Device->Release();
    std::printf("device runs %s\n", isOn12 ? "on D3D12 (9On12)" : "on native D3D9");
    Expect(isOn12 == on12, on12 ? "device runs over D3D12" : "device is native D3D9");

    bool allOk = true;
    ExpectPaced(Time(20,
                     [&](int i) {
                         device->Clear(0, nullptr, D3DCLEAR_TARGET,
                                       D3DCOLOR_XRGB(i * 10, 0, 255 - i * 10), 1.0f, 0);
                         allOk &= SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr));
                     }),
                20, fps, "Present");
    Expect(allOk, "Present succeeded");

    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return Result();
}

}  // namespace probe
