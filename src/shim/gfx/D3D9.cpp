// Direct3D 9 containment and pacing.
//
//   Direct3DCreate9   optionally routed through Direct3DCreate9On12 (--d3d9on12),
//                     so the game's D3D9 runs on D3D12 queues
//   CreateDevice/Reset a fullscreen device (Windowed = FALSE) becomes a
//                     windowed device in the managed window; its back buffer
//                     size and format become the virtual display mode
//   Present           paced by the shared pacer; for contained devices, the
//                     back buffer goes to the integer-scaled viewport and the
//                     bars are painted black
//   GetDisplayMode, GetAdapterDisplayMode
//                     report the virtual mode

#include <windows.h>

#include <d3d9.h>
#include <d3d9on12.h>
#include <detours/detours.h>

#include <algorithm>
#include <vector>

#include "../DisplayContext.h"
#include "../Log.h"
#include "../Pacing.h"
#include "../ShimState.h"
#include "../hooks/Hooks.h"
#include "Graphics.h"
#include "VtableHook.h"
#include "retro/DisplayMath.h"

namespace retro::shim::gfx::d3d9 {
namespace {

namespace d3dslot {
constexpr int GetAdapterDisplayMode = 8, CreateDevice = 16;
}
namespace devslot {
constexpr int GetDisplayMode = 8, Reset = 16, Present = 17;
}

using Direct3DCreate9On12Fn = IDirect3D9*(WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
using GetAdapterDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DDISPLAYMODE*);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using DevGetDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DDISPLAYMODE*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);

decltype(&Direct3DCreate9) Real_Direct3DCreate9 = nullptr;
HMODULE g_module = nullptr;

struct ContainedDevice {
    void* device;
    HWND hwnd;
};
std::vector<ContainedDevice> g_devices;
SRWLOCK g_lock = SRWLOCK_INIT;

bool SandboxOn() { return (Config().features & ShimFeature_DisplaySandbox) != 0; }

uint32_t BitsOf(D3DFORMAT format) {
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

D3DFORMAT FormatFor(uint32_t bits) { return bits == 16 ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8; }

bool FindDevice(void* device, HWND& hwnd) {
    AcquireSRWLockShared(&g_lock);
    const auto it = std::find_if(g_devices.begin(), g_devices.end(),
                                 [&](const ContainedDevice& d) { return d.device == device; });
    const bool found = it != g_devices.end();
    if (found) hwnd = it->hwnd;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void Remember(void* device, HWND hwnd) {
    AcquireSRWLockExclusive(&g_lock);
    const auto it = std::find_if(g_devices.begin(), g_devices.end(),
                                 [&](const ContainedDevice& d) { return d.device == device; });
    if (it != g_devices.end()) it->hwnd = hwnd; else g_devices.push_back({device, hwnd});
    ReleaseSRWLockExclusive(&g_lock);
}

void WriteVirtualMode(D3DDISPLAYMODE* mode) {
    DisplayMode m;
    if (!mode || !display::GetVirtualMode(m)) return;
    mode->Width = m.width;
    mode->Height = m.height;
    mode->RefreshRate = m.refreshHz > 1 ? m.refreshHz : 60;
    mode->Format = FormatFor(m.bitsPerPixel);
}

// Fullscreen parameters -> windowed inside the sandbox. Returns the window.
HWND Contain(D3DPRESENT_PARAMETERS& pp, HWND focus) {
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
    return window;
}

void PatchDevice(void* device);

HRESULT STDMETHODCALLTYPE D3D_GetAdapterDisplayMode(void* self, UINT adapter, D3DDISPLAYMODE* mode) {
    const HRESULT hr =
        Original<GetAdapterDisplayModeFn>(self, d3dslot::GetAdapterDisplayMode)(self, adapter, mode);
    if (SUCCEEDED(hr) && SandboxOn() && !InternalCall::Active()) WriteVirtualMode(mode);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D_CreateDevice(void* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD behavior, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out) {
    const auto real = Original<CreateDeviceFn>(self, d3dslot::CreateDevice);
    if (InternalCall::Active() || !pp || pp->Windowed || !SandboxOn()) {
        const HRESULT hr = real(self, adapter, type, focus, behavior, pp, out);
        if (SUCCEEDED(hr) && out && *out) PatchDevice(*out);
        return hr;
    }

    D3DPRESENT_PARAMETERS windowed = *pp;
    const HWND window = Contain(windowed, focus);
    HRESULT hr = real(self, adapter, type, focus, behavior, &windowed, out);
    if (FAILED(hr) && windowed.BackBufferFormat != D3DFMT_X8R8G8B8) {
        log::Write("Direct3D9: windowed %u-bit back buffer refused (0x%08lX); retrying 32-bit",
                   BitsOf(windowed.BackBufferFormat), hr);
        windowed.BackBufferFormat = D3DFMT_X8R8G8B8;
        hr = real(self, adapter, type, focus, behavior, &windowed, out);
    }
    if (FAILED(hr)) {
        log::Write("Direct3D9: CreateDevice (contained) failed 0x%08lX", hr);
        display::ClearVirtualMode();
        return hr;
    }

    pp->BackBufferCount = windowed.BackBufferCount;  // D3D fills this in
    Remember(*out, window);
    PatchDevice(*out);
    log::Write("Direct3D9: fullscreen device %ux%u -> windowed in the sandbox (window %p)",
               windowed.BackBufferWidth, windowed.BackBufferHeight, window);
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_GetDisplayMode(void* self, UINT swapChain, D3DDISPLAYMODE* mode) {
    const HRESULT hr =
        Original<DevGetDisplayModeFn>(self, devslot::GetDisplayMode)(self, swapChain, mode);
    HWND hwnd;
    if (SUCCEEDED(hr) && !InternalCall::Active() && FindDevice(self, hwnd)) WriteVirtualMode(mode);
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_Reset(void* self, D3DPRESENT_PARAMETERS* pp) {
    const auto real = Original<ResetFn>(self, devslot::Reset);
    if (InternalCall::Active() || !pp || pp->Windowed || !SandboxOn()) return real(self, pp);

    D3DPRESENT_PARAMETERS windowed = *pp;
    HWND focus = nullptr;
    FindDevice(self, focus);
    const HWND window = Contain(windowed, focus);
    const HRESULT hr = real(self, &windowed);
    if (SUCCEEDED(hr)) {
        pp->BackBufferCount = windowed.BackBufferCount;
        Remember(self, window);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_Present(void* self, const RECT* src, const RECT* dst, HWND override,
                                      const RGNDATA* dirty) {
    const auto real = Original<PresentFn>(self, devslot::Present);
    if (InternalCall::Active()) return real(self, src, dst, override, dirty);

    pacing::PaceFrame();

    HWND hwnd;
    display::ManagedView v;
    if (!dst && !override && FindDevice(self, hwnd) && display::FindManaged(hwnd, v)) {
        {
            display::DpiScope scope(v.dpi);
            if (HDC dc = GetDC(v.hwnd)) {
                display::FillLetterbox(dc, v);
                ReleaseDC(v.hwnd, dc);
            }
        }
        const Rect vp = v.ClientViewport();
        const RECT target{vp.left, vp.top, vp.right, vp.bottom};
        return real(self, src, &target, override, dirty);
    }
    return real(self, src, dst, override, dirty);
}

void PatchDevice(void* device) {
    PatchVtable(device, {
                            {devslot::GetDisplayMode, reinterpret_cast<void*>(&Dev_GetDisplayMode)},
                            {devslot::Reset, reinterpret_cast<void*>(&Dev_Reset)},
                            {devslot::Present, reinterpret_cast<void*>(&Dev_Present)},
                        });
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdkVersion) {
    if (InternalCall::Active()) return Real_Direct3DCreate9(sdkVersion);

    IDirect3D9* d3d = nullptr;
    if (Config().displayFlags & DisplayFlag_D3D9On12) {
        const auto on12 = reinterpret_cast<Direct3DCreate9On12Fn>(
            GetProcAddress(g_module, "Direct3DCreate9On12"));
        if (on12) {
            InternalCall internal;  // it may call Direct3DCreate9 itself
            D3D9ON12_ARGS args{};
            args.Enable9On12 = TRUE;
            d3d = on12(sdkVersion, &args, 1);
        }
        log::Write("Direct3D9: Direct3DCreate9On12 %s",
                   d3d ? "active (D3D9 over D3D12)" : "unavailable; using native D3D9");
    }
    if (!d3d) d3d = Real_Direct3DCreate9(sdkVersion);
    if (d3d) {
        PatchVtable(d3d, {
                             {d3dslot::GetAdapterDisplayMode,
                              reinterpret_cast<void*>(&D3D_GetAdapterDisplayMode)},
                             {d3dslot::CreateDevice, reinterpret_cast<void*>(&D3D_CreateDevice)},
                         });
    }
    return d3d;
}

}  // namespace

void OnLoaded(HMODULE module) {
    if (Real_Direct3DCreate9) return;
    g_module = module;
    Real_Direct3DCreate9 =
        reinterpret_cast<decltype(Real_Direct3DCreate9)>(GetProcAddress(module, "Direct3DCreate9"));
    if (!Real_Direct3DCreate9) return;

    const bool owns = DetourTransactionBegin() == NO_ERROR;  // or join the shim's transaction
    if (owns) DetourUpdateThread(GetCurrentThread());
    LONG err = NO_ERROR;
    HookStep(err, true, Real_Direct3DCreate9, Hook_Direct3DCreate9);
    if (owns) err = err == NO_ERROR ? DetourTransactionCommit() : (DetourTransactionAbort(), err);
    if (err != NO_ERROR) Real_Direct3DCreate9 = nullptr;
    log::Write("graphics: d3d9.dll %s (error %ld)", err == NO_ERROR ? "hooked" : "NOT hooked", err);
}

void OnUnloaded(const void* base, size_t size) {
    Real_Direct3DCreate9 = nullptr;
    g_module = nullptr;
    ForgetVtablesInRange(base, size);
    AcquireSRWLockExclusive(&g_lock);
    g_devices.clear();
    ReleaseSRWLockExclusive(&g_lock);
    log::Write("graphics: d3d9.dll unloaded");
}

LONG Unhook() {
    LONG err = NO_ERROR;
    if (Real_Direct3DCreate9) HookStep(err, false, Real_Direct3DCreate9, Hook_Direct3DCreate9);
    return err;
}

bool Hooked() { return Real_Direct3DCreate9 != nullptr; }

}  // namespace retro::shim::gfx::d3d9
