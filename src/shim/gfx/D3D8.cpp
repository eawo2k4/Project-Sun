// Direct3D 8.
//
// Default (native d3d8.dll):
//   IDirect3D8::CreateDevice/Reset  fullscreen devices become windowed in the
//                                   managed window (virtual mode = back buffer)
//   IDirect3DDevice8::Present       paced, shown in the integer-scaled viewport
//   GetDisplayMode, GetAdapterDisplayMode  report the virtual mode
//
// --d3d8to9: Direct3DCreate8 returns the vendored d3d8to9 bridge instead
// (third_party/d3d8to9), which implements D3D8 on top of D3D9. It gets its
// IDirect3D9 through the hooked Direct3DCreate9, so the D3D9 hooks contain and
// pace it, and --d3d9on12 puts the whole D3D8 game on D3D12.

#include <windows.h>

#include <detours/detours.h>

#include <atomic>
#include <vector>

#include "d3d8.hpp"  // third_party/d3d8to9: the D3D8 interfaces (no longer in the Windows SDK)

#include "../Log.h"
#include "../Pacing.h"
#include "../ShimState.h"
#include "../hooks/Hooks.h"
#include "D3DCommon.h"
#include "Graphics.h"
#include "VtableHook.h"

// d3d8to9's entry point (third_party/d3d8to9/source/d3d8to9.cpp).
extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion);

// d3d8to9 calls Direct3DCreate9 directly. Rather than importing d3d9.dll into
// every process the shim is injected into, resolve it on first use; the call
// lands on d3d9.dll's (hooked) export either way.
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    using Fn = IDirect3D9*(WINAPI*)(UINT);
    const HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    const auto create = d3d9 ? reinterpret_cast<Fn>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    return create ? create(sdkVersion) : nullptr;
}

namespace retro::shim::gfx::d3d8 {
namespace {

namespace d3dslot {
constexpr int GetAdapterDisplayMode = 8, CreateDevice = 15;
}
namespace devslot {
constexpr int GetDisplayMode = 8, Reset = 14, Present = 15;
}

using Direct3DCreate8Fn = IDirect3D8*(WINAPI*)(UINT);
using GetAdapterDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DDISPLAYMODE*);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS8*, IDirect3DDevice8**);
using DevGetDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DDISPLAYMODE*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DPRESENT_PARAMETERS8*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);

Direct3DCreate8Fn Real_Direct3DCreate8 = nullptr;

struct ContainedDevice {
    void* device;
    HWND hwnd;
};
std::vector<ContainedDevice> g_devices;
SRWLOCK g_lock = SRWLOCK_INIT;

std::atomic<uint32_t> g_containedDevices{0};
std::atomic<uint32_t> g_bridgedInterfaces{0};

bool SandboxOn() { return (Config().features & ShimFeature_DisplaySandbox) != 0; }

bool FindDevice(void* device, HWND& hwnd) {
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (const ContainedDevice& d : g_devices) {
        if (d.device == device) {
            hwnd = d.hwnd;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void Remember(void* device, HWND hwnd) {
    AcquireSRWLockExclusive(&g_lock);
    for (ContainedDevice& d : g_devices) {
        if (d.device == device) {
            d.hwnd = hwnd;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
    }
    g_devices.push_back({device, hwnd});
    ReleaseSRWLockExclusive(&g_lock);
}

void PatchDevice(void* device);

HRESULT STDMETHODCALLTYPE D3D_GetAdapterDisplayMode(void* self, UINT adapter, D3DDISPLAYMODE* mode) {
    const HRESULT hr =
        Original<GetAdapterDisplayModeFn>(self, d3dslot::GetAdapterDisplayMode)(self, adapter, mode);
    if (SUCCEEDED(hr) && SandboxOn() && !InternalCall::Active()) WriteVirtualMode(mode);
    return hr;
}

HRESULT STDMETHODCALLTYPE D3D_CreateDevice(void* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD behavior, D3DPRESENT_PARAMETERS8* pp,
                                           IDirect3DDevice8** out) {
    const auto real = Original<CreateDeviceFn>(self, d3dslot::CreateDevice);
    if (InternalCall::Active() || !pp || pp->Windowed || !SandboxOn()) {
        const HRESULT hr = real(self, adapter, type, focus, behavior, pp, out);
        if (SUCCEEDED(hr) && out && *out) PatchDevice(*out);
        return hr;
    }

    D3DPRESENT_PARAMETERS8 windowed = *pp;
    const HWND window = ContainFullscreen(windowed, focus);
    HRESULT hr = real(self, adapter, type, focus, behavior, &windowed, out);
    if (FAILED(hr) && windowed.BackBufferFormat != D3DFMT_X8R8G8B8) {
        log::Write("Direct3D8: windowed %u-bit back buffer refused (0x%08lX); retrying 32-bit",
                   BitsOf(windowed.BackBufferFormat), hr);
        windowed.BackBufferFormat = D3DFMT_X8R8G8B8;
        hr = real(self, adapter, type, focus, behavior, &windowed, out);
    }
    if (FAILED(hr)) {
        log::Write("Direct3D8: CreateDevice (contained) failed 0x%08lX", hr);
        display::ClearVirtualMode();
        return hr;
    }

    pp->BackBufferCount = windowed.BackBufferCount;
    Remember(*out, window);
    PatchDevice(*out);
    ++g_containedDevices;
    log::Write("Direct3D8: fullscreen device %ux%u -> windowed in the sandbox (window %p)",
               windowed.BackBufferWidth, windowed.BackBufferHeight, window);
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_GetDisplayMode(void* self, D3DDISPLAYMODE* mode) {
    const HRESULT hr = Original<DevGetDisplayModeFn>(self, devslot::GetDisplayMode)(self, mode);
    HWND hwnd;
    if (SUCCEEDED(hr) && !InternalCall::Active() && FindDevice(self, hwnd)) WriteVirtualMode(mode);
    return hr;
}

HRESULT STDMETHODCALLTYPE Dev_Reset(void* self, D3DPRESENT_PARAMETERS8* pp) {
    const auto real = Original<ResetFn>(self, devslot::Reset);
    if (InternalCall::Active() || !pp || pp->Windowed || !SandboxOn()) return real(self, pp);

    D3DPRESENT_PARAMETERS8 windowed = *pp;
    HWND focus = nullptr;
    FindDevice(self, focus);
    const HWND window = ContainFullscreen(windowed, focus);
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
    RECT viewport;
    if (!dst && !override && FindDevice(self, hwnd) && PrepareViewportPresent(hwnd, viewport))
        return real(self, src, &viewport, override, dirty);
    return real(self, src, dst, override, dirty);
}

void PatchDevice(void* device) {
    PatchVtable(device, {
                            {devslot::GetDisplayMode, reinterpret_cast<void*>(&Dev_GetDisplayMode)},
                            {devslot::Reset, reinterpret_cast<void*>(&Dev_Reset)},
                            {devslot::Present, reinterpret_cast<void*>(&Dev_Present)},
                        });
}

IDirect3D8* WINAPI Hook_Direct3DCreate8(UINT sdkVersion) {
    if (InternalCall::Active()) return Real_Direct3DCreate8(sdkVersion);

    if (Config().displayFlags & DisplayFlag_D3D8To9) {
        if (IDirect3D8* bridged = ::Direct3DCreate8(sdkVersion)) {
            ++g_bridgedInterfaces;
            log::Write("Direct3D8: bridged to Direct3D 9 (d3d8to9)%s",
                       (Config().displayFlags & DisplayFlag_D3D9On12) ? " over D3D12" : "");
            return bridged;
        }
        log::Write("Direct3D8: d3d8to9 bridge unavailable (no d3d9.dll?); using native D3D8");
    }

    IDirect3D8* d3d = Real_Direct3DCreate8(sdkVersion);
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
    if (Real_Direct3DCreate8) return;
    Real_Direct3DCreate8 =
        reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(module, "Direct3DCreate8"));
    if (!Real_Direct3DCreate8) return;

    const bool owns = DetourTransactionBegin() == NO_ERROR;  // or join the shim's transaction
    if (owns) DetourUpdateThread(GetCurrentThread());
    LONG err = NO_ERROR;
    HookStep(err, true, Real_Direct3DCreate8, Hook_Direct3DCreate8);
    if (owns) err = err == NO_ERROR ? DetourTransactionCommit() : (DetourTransactionAbort(), err);
    if (err != NO_ERROR) Real_Direct3DCreate8 = nullptr;
    log::Write("graphics: d3d8.dll %s (error %ld)", err == NO_ERROR ? "hooked" : "NOT hooked", err);
}

void OnUnloaded(const void* base, size_t size) {
    Real_Direct3DCreate8 = nullptr;
    ForgetVtablesInRange(base, size);
    AcquireSRWLockExclusive(&g_lock);
    g_devices.clear();
    ReleaseSRWLockExclusive(&g_lock);
    log::Write("graphics: d3d8.dll unloaded");
}

LONG Unhook() {
    LONG err = NO_ERROR;
    if (Real_Direct3DCreate8) HookStep(err, false, Real_Direct3DCreate8, Hook_Direct3DCreate8);
    return err;
}

bool Hooked() { return Real_Direct3DCreate8 != nullptr; }

void AddStats(PresentStats& out) {
    out.d3d8DevicesContained = g_containedDevices;
    out.d3d8InterfacesBridged = g_bridgedInterfaces;
}

}  // namespace retro::shim::gfx::d3d8
