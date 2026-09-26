// Direct3D 8 probe (see ShimProbe.cpp): a fullscreen D3D8 device, natively or
// bridged through d3d8to9, must be contained, paced and still render
// correctly (checked by reading the back buffer).
//
// Safety: refuses to create a fullscreen device unless the shim reports the
// d3d8 hooks and the display sandbox active. Windows stay hidden.

#include <windows.h>

#include <cstdio>
#include <string>

#include "ProbeCommon.h"
#include "d3d8.hpp"  // third_party/d3d8to9: D3D8 interfaces (not in the Windows SDK)
#include "retro/FramePacing.h"
#include "retro/ShimProtocol.h"

namespace probe {
namespace {

constexpr UINT kD3D8SdkVersion = 220;
constexpr UINT kW = 800, kH = 600;
constexpr D3DCOLOR kRed = 0xFFFF0000, kGreen = 0xFF00FF00;

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

HWND HiddenPopup() {
    static const ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RetroShimD3D8Probe";
        return RegisterClassW(&wc);
    }();
    (void)cls;
    return CreateWindowExW(0, L"RetroShimD3D8Probe", L"d3d8 probe", WS_POPUP, 0, 0, kW, kH,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

int DesktopWidth() {
    HDC dc = GetDC(nullptr);
    const int w = GetDeviceCaps(dc, DESKTOPHORZRES);
    ReleaseDC(nullptr, dc);
    return w;
}

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

// Clears to red and draws a green triangle over the centre.
void DrawFrame(IDirect3DDevice8* device) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, kRed, 1.0f, 0);
    device->BeginScene();
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetVertexShader(kFvf);  // D3D8: fixed function by FVF
    const Vertex tri[3] = {{kW / 2.0f, kH / 4.0f, 0.5f, 1.0f, kGreen},
                           {kW * 3 / 4.0f, kH * 3 / 4.0f, 0.5f, 1.0f, kGreen},
                           {kW / 4.0f, kH * 3 / 4.0f, 0.5f, 1.0f, kGreen}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
    device->EndScene();
}

// Reads a pixel of the back buffer (lockable, X8R8G8B8).
bool ReadPixel(IDirect3DDevice8* device, UINT x, UINT y, D3DCOLOR& out) {
    IDirect3DSurface8* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
    D3DLOCKED_RECT lr{};
    const bool ok = SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY));
    if (ok) {
        const auto* row = reinterpret_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
        out = reinterpret_cast<const D3DCOLOR*>(row)[x] | 0xFF000000;
        back->UnlockRect();
    }
    back->Release();
    return ok;
}

}  // namespace

int ProbeDirect3D8(uint32_t fps, D3D8Path path) {
    const HMODULE d3d8 = LoadLibraryW(L"d3d8.dll");  // on demand, like many games
    if (!d3d8 || !Active("d3d8") || !Active("display")) {
        std::printf("d3d8 hooks or display sandbox inactive: refusing a fullscreen device\n");
        return 3;
    }
    using CreateFn = IDirect3D8*(WINAPI*)(UINT);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d8, "Direct3DCreate8"));
    const retro::PresentStats before = Stats();
    IDirect3D8* d3d = create ? create(kD3D8SdkVersion) : nullptr;
    if (!d3d) {
        std::printf("Direct3D 8 unavailable on this machine: skipping\n");
        return 0;
    }

    const int desktopW = DesktopWidth();
    HWND hwnd = HiddenPopup();
    D3DPRESENT_PARAMETERS8 pp{};
    pp.BackBufferWidth = kW;
    pp.BackBufferHeight = kH;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = FALSE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    pp.FullScreen_RefreshRateInHz = 60;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice8* device = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    std::printf("CreateDevice(fullscreen %ux%u) -> 0x%08lX\n", kW, kH, hr);
    if (FAILED(hr) || !device) {
        d3d->Release();
        std::printf("no Direct3D 8 HAL device on this machine: skipping\n");
        return 0;
    }

    Expect(DesktopWidth() == desktopW, "real desktop resolution unchanged");
    Expect(GetSystemMetrics(SM_CXSCREEN) == static_cast<int>(kW), "GetSystemMetrics sees 800x600");
    RECT client{};
    GetClientRect(hwnd, &client);
    Expect(client.right == static_cast<LONG>(kW) && client.bottom == static_cast<LONG>(kH),
           "device window is sandboxed (800x600)");
    D3DDISPLAYMODE mode{};
    Expect(SUCCEEDED(device->GetDisplayMode(&mode)) && mode.Width == kW && mode.Height == kH,
           "GetDisplayMode reports the virtual mode");

    // Which path actually served the device?
    const retro::PresentStats after = Stats();
    const uint32_t native = after.d3d8DevicesContained - before.d3d8DevicesContained;
    const uint32_t bridged = after.d3d8InterfacesBridged - before.d3d8InterfacesBridged;
    const uint32_t viaD3D9 = after.d3d9DevicesContained - before.d3d9DevicesContained;
    const uint32_t on12 = after.d3d9On12Interfaces - before.d3d9On12Interfaces;
    std::printf("native D3D8 devices %u, bridged D3D8 interfaces %u, contained D3D9 devices %u, "
                "9On12 interfaces %u\n",
                native, bridged, viaD3D9, on12);
    if (path == D3D8Path::Native) {
        Expect(native == 1 && bridged == 0 && viaD3D9 == 0, "native D3D8 device contained");
    } else {
        Expect(native == 0 && bridged == 1 && viaD3D9 == 1,
               "D3D8 bridged to D3D9, and its D3D9 device contained");
        Expect((on12 == 1) == (path == D3D8Path::BridgeOn12),
               path == D3D8Path::BridgeOn12 ? "bridge runs over D3D12" : "bridge runs on native D3D9");
    }

    // Rendering through whichever path: clear + fixed-function triangle.
    DrawFrame(device);
    D3DCOLOR centre = 0, corner = 0;
    const bool read = ReadPixel(device, kW / 2, kH / 2, centre) && ReadPixel(device, 5, 5, corner);
    std::printf("back buffer: centre %08lX, corner %08lX\n", centre, corner);
    Expect(read && centre == kGreen && corner == kRed, "frame rendered correctly (triangle over clear)");

    const int64_t start = retro::QpcNow();
    bool presentsOk = true;
    for (int i = 0; i < 20; ++i) {
        DrawFrame(device);
        presentsOk &= SUCCEEDED(device->Present(nullptr, nullptr, nullptr, nullptr));
    }
    const double seconds = static_cast<double>(retro::QpcNow() - start) / retro::QpcFrequency();
    std::printf("Present: 20 frames in %.1f ms\n", seconds * 1000);
    Expect(presentsOk, "Present succeeded");
    if (fps == 0) {
        Expect(seconds < 0.15, "unpaced (fast)");
    } else {
        Expect(seconds >= 18.0 / fps * 0.99 && seconds <= 21.0 / fps * 1.5, "paced to the frame cap");
    }

    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return Result();
}

}  // namespace probe
