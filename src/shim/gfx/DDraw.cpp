// DirectDraw containment.
//
// A game asking for DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN gets DDSCL_NORMAL
// instead, and its display mode becomes the sandbox's virtual mode (the real
// desktop never changes). Its primary surface (plus back buffer) is created
// as system-memory surfaces in the pixel format of the virtual mode, 8-bit
// palettized included, and reported to the game as a flipping primary chain.
//
// Frames reach the screen when the game Flips, blits a whole frame onto the
// primary, or changes the palette; partial updates (sprites, cursor, Lock/
// Unlock, GetDC/ReleaseDC) are coalesced to at most one present per frame
// period and flushed from the message pump. Presenting converts the primary
// to 32-bit BGRA (palette lookup for 8-bit) and draws it into the managed
// window's integer-scaled viewport, paced by the shared pacer.
//
// Not virtualized: a primary created with DDSCAPS_3DDEVICE (Direct3D 3-7
// rendering straight to the primary needs a video-memory surface). The game
// then gets the real exclusive mode it asked for, and the log says so.

#include <windows.h>

#include <ddraw.h>
#include <detours/detours.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

#include "../DisplayContext.h"
#include "../Log.h"
#include "../Pacing.h"
#include "../ShimState.h"
#include "../hooks/Hooks.h"
#include "Graphics.h"
#include "VtableHook.h"
#include "retro/DisplayMath.h"
#include "retro/FramePacing.h"
#include "retro/PixelConvert.h"

namespace retro::shim::gfx::ddraw {
namespace {

// --- Interface layout -----------------------------------------------------------------

namespace ddslot {
constexpr int QueryInterface = 0, CreateSurface = 6, EnumDisplayModes = 8, GetDisplayMode = 12,
              RestoreDisplayMode = 19, SetCooperativeLevel = 20, SetDisplayMode = 21,
              WaitForVerticalBlank = 22;
}
namespace sslot {
constexpr int QueryInterface = 0, Blt = 5, BltFast = 7, Flip = 11, GetAttachedSurface = 12,
              GetCaps = 14, GetSurfaceDesc = 22, ReleaseDC = 26, SetPalette = 31, Unlock = 32;
}
namespace pslot {
constexpr int SetEntries = 6;
}

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using CreateSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void**, IUnknown*);
using EnumDisplayModesFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, void*, void*);
using GetDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
using RestoreDisplayModeFn = HRESULT(STDMETHODCALLTYPE*)(void*);
using SetCooperativeLevelFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
using SetDisplayMode1Fn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD);
using SetDisplayMode2Fn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD);
using WaitForVerticalBlankFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, HANDLE);

using BltFn = HRESULT(STDMETHODCALLTYPE*)(void*, RECT*, void*, RECT*, DWORD, DDBLTFX*);
using BltFastFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, void*, RECT*, DWORD);
using FlipFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, DWORD);
using GetAttachedSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, DDSCAPS*, void**);
using GetCapsFn = HRESULT(STDMETHODCALLTYPE*)(void*, DDSCAPS*);
using GetSurfaceDescFn = HRESULT(STDMETHODCALLTYPE*)(void*, DDSURFACEDESC*);
using ReleaseDCFn = HRESULT(STDMETHODCALLTYPE*)(void*, HDC);
using SetPaletteFn = HRESULT(STDMETHODCALLTYPE*)(void*, IDirectDrawPalette*);
using UnlockFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
using SetEntriesFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DWORD, DWORD, LPPALETTEENTRY);

// Surface hooks treat DDSURFACEDESC and DDSURFACEDESC2 (and DDSCAPS/DDSCAPS2)
// through their common prefix.
static_assert(offsetof(DDSURFACEDESC, ddsCaps) == offsetof(DDSURFACEDESC2, ddsCaps));
static_assert(offsetof(DDSURFACEDESC, ddpfPixelFormat) == offsetof(DDSURFACEDESC2, ddpfPixelFormat));
static_assert(offsetof(DDSURFACEDESC, dwBackBufferCount) ==
              offsetof(DDSURFACEDESC2, dwBackBufferCount));

// IDirectDraw/IDirectDraw2 use DDSURFACEDESC, IDirectDraw4/7 DDSURFACEDESC2.
template <int V>
using DescFor = std::conditional_t<(V >= 4), DDSURFACEDESC2, DDSURFACEDESC>;

decltype(&DirectDrawCreate) Real_DirectDrawCreate = nullptr;
decltype(&DirectDrawCreateEx) Real_DirectDrawCreateEx = nullptr;

// --- State ----------------------------------------------------------------------------------

struct Session {
    bool exclusive = false;  // the game holds a (virtualized) exclusive level
    bool fellBack = false;   // gave the game the real exclusive mode instead
    DWORD requestedFlags = 0;
    HWND hwnd = nullptr;
    bool haveMode = false;
    DisplayMode mode{};
};

struct Alias {
    void* ptr;
    IID iid;
};

struct FakePrimary {
    void* front = nullptr;  // as returned to the game (the game owns this reference)
    void* back = nullptr;   // our reference; the game gets its own via GetAttachedSurface
    IDirectDrawSurface7* front7 = nullptr;  // our handles for internal work
    IDirectDrawSurface7* back7 = nullptr;
    std::vector<Alias> frontAliases;  // every interface pointer of each surface we know
    std::vector<Alias> backAliases;
    DisplayMode mode{};
    HWND hwnd = nullptr;
    void* palette = nullptr;  // attached IDirectDrawPalette (ddraw holds the reference)
    std::vector<uint32_t> bgra;
};

struct CriticalSection {
    CRITICAL_SECTION cs;
    CriticalSection() { InitializeCriticalSection(&cs); }
};
CriticalSection g_cs;  // recursive: hooks can re-enter through internal calls

class Lock {
public:
    Lock() { EnterCriticalSection(&g_cs.cs); }
    ~Lock() { LeaveCriticalSection(&g_cs.cs); }
};

Session g_session;
std::unique_ptr<FakePrimary> g_fake;
std::atomic<bool> g_dirty{false};
PresentStats g_stats;

enum class Role { None, Front, Back };

bool SandboxOn() { return (Config().features & ShimFeature_DisplaySandbox) != 0; }

bool Virtualizing() {
    Lock l;
    return SandboxOn() && g_session.exclusive && !g_session.fellBack;
}

Role RoleOfLocked(void* surface) {
    if (!g_fake || !surface) return Role::None;
    for (const Alias& a : g_fake->frontAliases) {
        if (a.ptr == surface) return Role::Front;
    }
    for (const Alias& a : g_fake->backAliases) {
        if (a.ptr == surface) return Role::Back;
    }
    return Role::None;
}

Role RoleOf(void* surface) {
    if (!surface) return Role::None;
    Lock l;
    return RoleOfLocked(surface);
}

// --- Formats ---------------------------------------------------------------------------------

DDPIXELFORMAT PixelFormatFor(uint32_t depth) {
    DDPIXELFORMAT pf{};
    pf.dwSize = sizeof(pf);
    pf.dwFlags = DDPF_RGB;
    const PixelFormat f = FormatForDepth(depth);
    pf.dwRGBBitCount = f.bitsPerPixel;
    if (f.palettized) {
        pf.dwFlags |= DDPF_PALETTEINDEXED8;
    } else {
        pf.dwRBitMask = f.rMask;
        pf.dwGBitMask = f.gMask;
        pf.dwBBitMask = f.bMask;
    }
    return pf;
}

PixelFormat FromDD(const DDPIXELFORMAT& pf) {
    PixelFormat f;
    f.bitsPerPixel = pf.dwRGBBitCount;
    f.palettized = (pf.dwFlags & DDPF_PALETTEINDEXED8) != 0;
    f.rMask = pf.dwRBitMask;
    f.gMask = pf.dwGBitMask;
    f.bMask = pf.dwBBitMask;
    return f;
}

template <class Desc>
void WriteModeDesc(Desc* d, const DisplayMode& m) {
    const uint32_t bytes = (FormatForDepth(m.bitsPerPixel).bitsPerPixel + 7) / 8;
    d->dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = m.width;
    d->dwHeight = m.height;
    d->lPitch = static_cast<LONG>(m.width * bytes);
    d->ddpfPixelFormat = PixelFormatFor(m.bitsPerPixel);
    d->dwRefreshRate = m.refreshHz > 1 ? m.refreshHz : 60;
}

int DirectDrawVersionOf(REFIID iid) {
    if (IsEqualIID(iid, IID_IDirectDraw)) return 1;
    if (IsEqualIID(iid, IID_IDirectDraw2)) return 2;
    if (IsEqualIID(iid, IID_IDirectDraw4)) return 4;
    if (IsEqualIID(iid, IID_IDirectDraw7)) return 7;
    return 0;
}

bool IsSurfaceIid(REFIID iid) {
    return IsEqualIID(iid, IID_IDirectDrawSurface) || IsEqualIID(iid, IID_IDirectDrawSurface2) ||
           IsEqualIID(iid, IID_IDirectDrawSurface3) || IsEqualIID(iid, IID_IDirectDrawSurface4) ||
           IsEqualIID(iid, IID_IDirectDrawSurface7);
}

// Surfaces from IDirectDraw/IDirectDraw2 are IDirectDrawSurface; from 4 and 7, 4 and 7.
const IID& SurfaceIidForDirectDraw(int version) {
    if (version >= 7) return IID_IDirectDrawSurface7;
    if (version >= 4) return IID_IDirectDrawSurface4;
    return IID_IDirectDrawSurface;
}

// --- Presentation --------------------------------------------------------------------------

void ReadPalette(FakePrimary& f, uint32_t (&out)[256]) {
    for (uint32_t i = 0; i < 256; ++i) out[i] = PaletteToBgra(uint8_t(i), uint8_t(i), uint8_t(i));
    IDirectDrawPalette* palette = nullptr;
    if (FAILED(f.front7->GetPalette(&palette)) || !palette) return;
    PALETTEENTRY entries[256];
    if (SUCCEEDED(palette->GetEntries(0, 0, 256, entries))) {
        for (int i = 0; i < 256; ++i)
            out[i] = PaletteToBgra(entries[i].peRed, entries[i].peGreen, entries[i].peBlue);
    }
    palette->Release();
}

void DrawToWindow(const FakePrimary& f) {
    display::ManagedView v;
    if (!display::FindManaged(f.hwnd, v) && !display::FirstManaged(v)) return;

    display::DpiScope scope(v.dpi);
    HDC dc = GetDC(v.hwnd);
    if (!dc) return;
    display::FillLetterbox(dc, v);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(f.mode.width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(f.mode.height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const Rect vp = v.ClientViewport();
    SetStretchBltMode(dc, COLORONCOLOR);  // nearest neighbour: crisp integer scaling
    StretchDIBits(dc, vp.left, vp.top, vp.Width(), vp.Height(), 0, 0,
                  static_cast<int>(f.mode.width), static_cast<int>(f.mode.height), f.bgra.data(),
                  &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(v.hwnd, dc);
}

// Converts the virtual primary to BGRA and puts it on screen (no pacing here:
// callers pace where the game's frame boundary is).
void Present() {
    Lock l;
    FakePrimary* f = g_fake.get();
    if (!f || !f->front7) return;

    InternalCall internal;
    uint32_t palette[256];
    ReadPalette(*f, palette);

    DDSURFACEDESC2 d{};
    d.dwSize = sizeof(d);
    if (FAILED(f->front7->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR,
                               nullptr))) {
        return;
    }
    const auto w = static_cast<int32_t>(f->mode.width), h = static_cast<int32_t>(f->mode.height);
    f->bgra.resize(static_cast<size_t>(w) * h);
    const bool ok = ConvertToBgra32(static_cast<const uint8_t*>(d.lpSurface), d.lPitch,
                                    FromDD(d.ddpfPixelFormat), palette, w, h, f->bgra.data(), w);
    f->front7->Unlock(nullptr);
    g_dirty = false;
    if (!ok) return;

    ++g_stats.frames;
    g_stats.width = f->mode.width;
    g_stats.height = f->mode.height;
    g_stats.bitsPerPixel = f->mode.bitsPerPixel;
    DrawToWindow(*f);
}

// Partial update of the primary: show it now if a frame period has passed,
// otherwise let the next update or the message pump flush it.
void MarkDirty() {
    g_dirty = true;
    if (pacing::DueForPresent()) Present();
}

bool IsWholeFrame(const RECT* r) {
    if (!r) return true;
    Lock l;
    if (!g_fake) return false;
    return IsFramePresent(r->right - r->left, r->bottom - r->top,
                          static_cast<int32_t>(g_fake->mode.width),
                          static_cast<int32_t>(g_fake->mode.height));
}

// --- Patching -----------------------------------------------------------------------------

void PatchSurface(void* surface);
void PatchPalette(void* palette);
void PatchDirectDrawVersion(void* dd, int version);

void AddAlias(void* existing, void* alias, REFIID iid) {
    Lock l;
    switch (RoleOfLocked(existing)) {
    case Role::Front: g_fake->frontAliases.push_back({alias, iid}); break;
    case Role::Back: g_fake->backAliases.push_back({alias, iid}); break;
    default: break;
    }
}

void ReleaseFake(std::unique_ptr<FakePrimary> f) {
    if (!f) return;
    InternalCall internal;
    if (f->front7) f->front7->Release();
    if (f->back7) f->back7->Release();
    if (f->back) static_cast<IUnknown*>(f->back)->Release();
    // f->front belongs to the game.
}

// --- IDirectDraw hooks --------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE DD_QueryInterface(void* self, REFIID iid, void** out) {
    const HRESULT hr = Original<QueryInterfaceFn>(self, ddslot::QueryInterface)(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && !InternalCall::Active()) {
        if (const int v = DirectDrawVersionOf(iid)) PatchDirectDrawVersion(*out, v);
    }
    return hr;
}

void Contain(const DisplayMode& mode, HWND hwnd) {
    display::SetVirtualMode(mode);
    if (hwnd) display::Manage(hwnd);
}

template <int V>
HRESULT STDMETHODCALLTYPE DD_SetCooperativeLevel(void* self, HWND hwnd, DWORD flags) {
    const auto real = Original<SetCooperativeLevelFn>(self, ddslot::SetCooperativeLevel);
    if (InternalCall::Active() || !SandboxOn()) return real(self, hwnd, flags);

    if (!(flags & DDSCL_EXCLUSIVE)) {
        bool wasVirtual;
        {
            Lock l;
            wasVirtual = g_session.exclusive && !g_session.fellBack;
            g_session = {};
        }
        if (wasVirtual) {
            log::Write("DirectDraw%d: back to DDSCL_NORMAL, leaving the virtual mode", V);
            display::ClearVirtualMode();
        }
        return real(self, hwnd, flags);
    }

    constexpr DWORD kExclusiveOnly = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWMODEX |
                                     DDSCL_ALLOWREBOOT | DDSCL_NOWINDOWCHANGES |
                                     DDSCL_CREATEDEVICEWINDOW | DDSCL_SETDEVICEWINDOW |
                                     DDSCL_SETFOCUSWINDOW;
    const HRESULT hr = real(self, hwnd, (flags & ~kExclusiveOnly) | DDSCL_NORMAL);
    if (FAILED(hr)) {
        log::Write("DirectDraw%d: SetCooperativeLevel(NORMAL) failed 0x%08lX", V, hr);
        return hr;
    }
    {
        Lock l;
        g_session = {};
        g_session.exclusive = true;
        g_session.requestedFlags = flags;
        g_session.hwnd = hwnd;
    }
    log::Write("DirectDraw%d: SetCooperativeLevel(%p, 0x%lX) -> DDSCL_NORMAL (exclusive virtualized)",
               V, hwnd, flags);
    Contain(display::RealMode(), hwnd);  // exclusive at the desktop mode until SetDisplayMode
    return DD_OK;
}

HRESULT VirtualSetDisplayMode(int version, DWORD w, DWORD h, DWORD bpp, DWORD hz) {
    const DisplayMode mode{w, h, bpp, hz};
    if (!IsPlausibleMode(mode)) {
        log::Write("DirectDraw%d: SetDisplayMode(%lux%lux%lu) -> DDERR_INVALIDMODE", version, w, h,
                   bpp);
        return DDERR_INVALIDMODE;
    }
    HWND hwnd;
    {
        Lock l;
        g_session.haveMode = true;
        g_session.mode = mode;
        hwnd = g_session.hwnd;
    }
    log::Write("DirectDraw%d: SetDisplayMode(%lux%lu, %lu bpp, %lu Hz) -> virtual", version, w, h,
               bpp, hz);
    Contain(mode, hwnd);
    return DD_OK;
}

template <int V>
HRESULT STDMETHODCALLTYPE DD_SetDisplayMode1(void* self, DWORD w, DWORD h, DWORD bpp) {
    if (InternalCall::Active() || !Virtualizing())
        return Original<SetDisplayMode1Fn>(self, ddslot::SetDisplayMode)(self, w, h, bpp);
    return VirtualSetDisplayMode(V, w, h, bpp, 0);
}

template <int V>
HRESULT STDMETHODCALLTYPE DD_SetDisplayMode2(void* self, DWORD w, DWORD h, DWORD bpp, DWORD hz,
                                             DWORD flags) {
    if (InternalCall::Active() || !Virtualizing()) {
        return Original<SetDisplayMode2Fn>(self, ddslot::SetDisplayMode)(self, w, h, bpp, hz,
                                                                         flags);
    }
    return VirtualSetDisplayMode(V, w, h, bpp, hz);
}

HRESULT STDMETHODCALLTYPE DD_RestoreDisplayMode(void* self) {
    if (InternalCall::Active() || !Virtualizing())
        return Original<RestoreDisplayModeFn>(self, ddslot::RestoreDisplayMode)(self);
    {
        Lock l;
        g_session.haveMode = false;
    }
    display::SetVirtualMode(display::RealMode());  // still exclusive, at the desktop mode
    return DD_OK;
}

template <int V>
HRESULT STDMETHODCALLTYPE DD_GetDisplayMode(void* self, void* desc) {
    const HRESULT hr = Original<GetDisplayModeFn>(self, ddslot::GetDisplayMode)(self, desc);
    DisplayMode m;
    if (SUCCEEDED(hr) && desc && !InternalCall::Active() && Virtualizing() &&
        display::GetVirtualMode(m)) {
        WriteModeDesc(static_cast<DescFor<V>*>(desc), m);
    }
    return hr;
}

// Lists the classic modes the sandbox accepts (modern drivers dropped most).
template <int V>
HRESULT STDMETHODCALLTYPE DD_EnumDisplayModes(void* self, DWORD flags, void* filter, void* context,
                                              void* callback) {
    if (InternalCall::Active() || !SandboxOn() || !callback) {
        return Original<EnumDisplayModesFn>(self, ddslot::EnumDisplayModes)(self, flags, filter,
                                                                            context, callback);
    }
    using Desc = DescFor<V>;
    using Callback = HRESULT(WINAPI*)(Desc*, void*);
    const auto* f = static_cast<const Desc*>(filter);
    for (const DisplayMode& m : ClassicModeList(display::PrimaryMonitorSize())) {
        if (f) {
            if ((f->dwFlags & DDSD_WIDTH) && f->dwWidth != m.width) continue;
            if ((f->dwFlags & DDSD_HEIGHT) && f->dwHeight != m.height) continue;
            if ((f->dwFlags & DDSD_PIXELFORMAT) &&
                f->ddpfPixelFormat.dwRGBBitCount != FormatForDepth(m.bitsPerPixel).bitsPerPixel)
                continue;
        }
        Desc d{};
        d.dwSize = sizeof(d);
        WriteModeDesc(&d, m);
        if (reinterpret_cast<Callback>(callback)(&d, context) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE DD_WaitForVerticalBlank(void* self, DWORD flags, HANDLE event) {
    if (!InternalCall::Active() && pacing::Enabled() &&
        (flags == DDWAITVB_BLOCKBEGIN || flags == DDWAITVB_BLOCKEND)) {
        // Games time themselves off the vertical blank; on a 144/165 Hz monitor
        // the real one runs them 2-3x too fast. Pace to the cap instead.
        pacing::PaceVerticalBlank();
        PresentPending();
        return DD_OK;
    }
    return Original<WaitForVerticalBlankFn>(self, ddslot::WaitForVerticalBlank)(self, flags, event);
}

// Direct3D rendering onto the primary needs a real video-memory primary:
// give the game the exclusive mode it asked for after all.
template <int V>
void FallBackToRealExclusive(void* dd) {
    Session s;
    {
        Lock l;
        g_session.fellBack = true;
        s = g_session;
    }
    log::Write("DirectDraw%d: primary with DDSCAPS_3DDEVICE can't be virtualized; applying the "
               "real exclusive mode (display will change)",
               V);
    display::ClearVirtualMode();

    InternalCall internal;
    Original<SetCooperativeLevelFn>(dd, ddslot::SetCooperativeLevel)(dd, s.hwnd, s.requestedFlags);
    if (!s.haveMode) return;
    if constexpr (V == 1) {
        Original<SetDisplayMode1Fn>(dd, ddslot::SetDisplayMode)(dd, s.mode.width, s.mode.height,
                                                               s.mode.bitsPerPixel);
    } else {
        Original<SetDisplayMode2Fn>(dd, ddslot::SetDisplayMode)(
            dd, s.mode.width, s.mode.height, s.mode.bitsPerPixel, s.mode.refreshHz, 0);
    }
}

template <int V>
HRESULT CreateFakePrimary(void* dd, const DescFor<V>& request, void** out, CreateSurfaceFn real) {
    using Desc = DescFor<V>;
    DisplayMode mode;
    if (!display::GetVirtualMode(mode)) mode = display::RealMode();

    const bool wantsBack = (request.ddsCaps.dwCaps & DDSCAPS_FLIP) &&
                           (request.dwFlags & DDSD_BACKBUFFERCOUNT) && request.dwBackBufferCount > 0;

    Desc d{};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = mode.width;
    d.dwHeight = mode.height;
    d.ddpfPixelFormat = PixelFormatFor(mode.bitsPerPixel);

    auto fake = std::make_unique<FakePrimary>();
    fake->mode = mode;
    {
        Lock l;
        fake->hwnd = g_session.hwnd;
    }
    {
        InternalCall internal;
        HRESULT hr = real(dd, &d, &fake->front, nullptr);
        if (SUCCEEDED(hr) && wantsBack) hr = real(dd, &d, &fake->back, nullptr);
        if (FAILED(hr)) {
            log::Write("DirectDraw%d: creating the virtual primary (%ux%u, %u bpp) failed 0x%08lX",
                       V, mode.width, mode.height, mode.bitsPerPixel, hr);
            if (fake->front) static_cast<IUnknown*>(fake->front)->Release();
            if (fake->back) static_cast<IUnknown*>(fake->back)->Release();
            return hr;
        }
        auto* front = static_cast<IUnknown*>(fake->front);
        front->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&fake->front7));
        if (fake->back) {
            static_cast<IUnknown*>(fake->back)
                ->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&fake->back7));
        }

        DDBLTFX fx{};
        fx.dwSize = sizeof(fx);
        if (fake->front7) fake->front7->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (fake->back7) fake->back7->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    }
    if (!fake->front7 || (fake->back && !fake->back7)) {
        log::Write("DirectDraw%d: IDirectDrawSurface7 unavailable; primary not virtualized", V);
        ReleaseFake(std::move(fake));
        return DDERR_GENERIC;
    }

    const IID& iid = SurfaceIidForDirectDraw(V);
    fake->frontAliases = {{fake->front, iid}, {fake->front7, IID_IDirectDrawSurface7}};
    if (fake->back) fake->backAliases = {{fake->back, iid}, {fake->back7, IID_IDirectDrawSurface7}};
    PatchSurface(fake->front);
    PatchSurface(fake->front7);

    void* front = fake->front;
    std::unique_ptr<FakePrimary> previous;
    {
        Lock l;
        previous = std::move(g_fake);
        g_fake = std::move(fake);
    }
    ReleaseFake(std::move(previous));

    log::Write("DirectDraw%d: primary %ux%u %u bpp virtualized (system memory%s)", V, mode.width,
               mode.height, mode.bitsPerPixel, wantsBack ? ", 1 back buffer" : "");
    *out = front;
    Present();  // show the cleared frame right away
    return DD_OK;
}

// Surfaces created without an explicit pixel format take the display's
// format. Under the sandbox the display "is" the virtual mode (e.g. 8-bit),
// but DirectDraw would use the real 32-bit desktop's: supply it explicitly.
template <int V>
HRESULT CreateInVirtualFormat(void* dd, const DescFor<V>& request, void** out, IUnknown* outer,
                              CreateSurfaceFn real) {
    DisplayMode mode;
    if (!display::GetVirtualMode(mode)) return real(dd, const_cast<DescFor<V>*>(&request), out, outer);

    DescFor<V> d = request;
    d.dwFlags |= DDSD_PIXELFORMAT;
    d.ddpfPixelFormat = PixelFormatFor(mode.bitsPerPixel);
    HRESULT hr = real(dd, &d, out, outer);
    if (FAILED(hr) && !(d.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        // Video memory may not do the legacy format; system memory always does.
        d.dwFlags |= DDSD_CAPS;
        d.ddsCaps.dwCaps = (d.ddsCaps.dwCaps & ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM |
                                                 DDSCAPS_NONLOCALVIDMEM)) |
                           DDSCAPS_SYSTEMMEMORY;
        hr = real(dd, &d, out, outer);
    }
    return hr;
}

template <int V>
HRESULT STDMETHODCALLTYPE DD_CreateSurface(void* self, void* desc, void** out, IUnknown* outer) {
    const auto real = Original<CreateSurfaceFn>(self, ddslot::CreateSurface);
    if (InternalCall::Active() || !desc || !out) return real(self, desc, out, outer);

    const auto& d = *static_cast<const DescFor<V>*>(desc);
    const DWORD caps = (d.dwFlags & DDSD_CAPS) ? d.ddsCaps.dwCaps : 0;
    constexpr DWORD kExplicitFormat =
        DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_OVERLAY;

    HRESULT hr;
    if (Virtualizing() && (caps & DDSCAPS_PRIMARYSURFACE)) {
        if (!(caps & DDSCAPS_3DDEVICE)) return CreateFakePrimary<V>(self, d, out, real);
        FallBackToRealExclusive<V>(self);
        hr = real(self, desc, out, outer);
    } else if (Virtualizing() && !(d.dwFlags & DDSD_PIXELFORMAT) && !(caps & kExplicitFormat)) {
        hr = CreateInVirtualFormat<V>(self, d, out, outer, real);
    } else {
        hr = real(self, desc, out, outer);
    }
    if (SUCCEEDED(hr) && *out) PatchSurface(*out);
    return hr;
}

// --- IDirectDrawSurface hooks ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE S_QueryInterface(void* self, REFIID iid, void** out) {
    const HRESULT hr = Original<QueryInterfaceFn>(self, sslot::QueryInterface)(self, iid, out);
    if (SUCCEEDED(hr) && out && *out && !InternalCall::Active() && IsSurfaceIid(iid)) {
        PatchSurface(*out);
        AddAlias(self, *out, iid);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE S_Flip(void* self, void* target, DWORD flags) {
    const auto real = Original<FlipFn>(self, sslot::Flip);
    if (InternalCall::Active()) return real(self, target, flags);
    if (RoleOf(self) != Role::Front) {
        pacing::PaceFlip();  // a real flipping chain (sandbox off or Direct3D fallback)
        return real(self, target, flags);
    }

    pacing::PaceFlip();
    {
        Lock l;
        if (g_fake && g_fake->back7) {
            InternalCall internal;
            g_fake->front7->Blt(nullptr, g_fake->back7, nullptr, DDBLT_WAIT, nullptr);
        }
    }
    Present();
    return DD_OK;
}

HRESULT STDMETHODCALLTYPE S_Blt(void* dst, RECT* dstRect, void* src, RECT* srcRect, DWORD flags,
                                DDBLTFX* fx) {
    const auto real = Original<BltFn>(dst, sslot::Blt);
    if (InternalCall::Active() || RoleOf(dst) != Role::Front)
        return real(dst, dstRect, src, srcRect, flags, fx);

    const bool wholeFrame = IsWholeFrame(dstRect);
    if (wholeFrame) pacing::PaceFrame();
    const HRESULT hr = real(dst, dstRect, src, srcRect, flags, fx);
    if (SUCCEEDED(hr)) {
        if (wholeFrame) Present(); else MarkDirty();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE S_BltFast(void* dst, DWORD x, DWORD y, void* src, RECT* srcRect,
                                    DWORD trans) {
    const auto real = Original<BltFastFn>(dst, sslot::BltFast);
    if (InternalCall::Active() || RoleOf(dst) != Role::Front)
        return real(dst, x, y, src, srcRect, trans);

    const bool wholeFrame = IsWholeFrame(srcRect);
    if (wholeFrame) pacing::PaceFrame();
    const HRESULT hr = real(dst, x, y, src, srcRect, trans);
    if (SUCCEEDED(hr)) {
        if (wholeFrame) Present(); else MarkDirty();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE S_Unlock(void* self, void* rect) {
    const HRESULT hr = Original<UnlockFn>(self, sslot::Unlock)(self, rect);
    if (SUCCEEDED(hr) && !InternalCall::Active() && RoleOf(self) == Role::Front) MarkDirty();
    return hr;
}

HRESULT STDMETHODCALLTYPE S_ReleaseDC(void* self, HDC dc) {
    const HRESULT hr = Original<ReleaseDCFn>(self, sslot::ReleaseDC)(self, dc);
    if (SUCCEEDED(hr) && !InternalCall::Active() && RoleOf(self) == Role::Front) MarkDirty();
    return hr;
}

// The virtual primary chain: front <-> back, returned in the interface
// version the caller uses.
HRESULT STDMETHODCALLTYPE S_GetAttachedSurface(void* self, DDSCAPS* caps, void** out) {
    const auto real = Original<GetAttachedSurfaceFn>(self, sslot::GetAttachedSurface);
    if (InternalCall::Active() || !caps || !out ||
        !(caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        return real(self, caps, out);
    }

    void* result = nullptr;
    {
        Lock l;
        const Role role = RoleOfLocked(self);
        if (role == Role::None) return real(self, caps, out);

        const auto& mine = role == Role::Front ? g_fake->frontAliases : g_fake->backAliases;
        IDirectDrawSurface7* peer = role == Role::Front ? g_fake->back7 : g_fake->front7;
        if (!peer) return DDERR_NOTFOUND;  // single-buffered primary
        const IID* iid = &IID_IDirectDrawSurface;
        for (const Alias& a : mine) {
            if (a.ptr == self) iid = &a.iid;
        }
        InternalCall internal;
        if (FAILED(peer->QueryInterface(*iid, &result))) return DDERR_NOTFOUND;
        auto& theirs = role == Role::Front ? g_fake->backAliases : g_fake->frontAliases;
        theirs.push_back({result, *iid});
    }
    PatchSurface(result);
    *out = result;
    return DD_OK;
}

void DescribeAsChain(Role role, DWORD& caps) {
    constexpr DWORD kVirtualized = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    caps = (caps & ~kVirtualized) | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY |
           DDSCAPS_LOCALVIDMEM;
    caps |= role == Role::Front ? DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE
                                : DDSCAPS_BACKBUFFER;
}

HRESULT STDMETHODCALLTYPE S_GetCaps(void* self, DDSCAPS* caps) {
    const HRESULT hr = Original<GetCapsFn>(self, sslot::GetCaps)(self, caps);
    if (SUCCEEDED(hr) && caps && !InternalCall::Active()) {
        const Role role = RoleOf(self);
        if (role != Role::None) DescribeAsChain(role, caps->dwCaps);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE S_GetSurfaceDesc(void* self, DDSURFACEDESC* desc) {
    const HRESULT hr = Original<GetSurfaceDescFn>(self, sslot::GetSurfaceDesc)(self, desc);
    if (SUCCEEDED(hr) && desc && !InternalCall::Active()) {
        const Role role = RoleOf(self);
        if (role != Role::None) {
            DescribeAsChain(role, desc->ddsCaps.dwCaps);
            if (role == Role::Front) {
                Lock l;
                desc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                desc->dwBackBufferCount = g_fake && g_fake->back ? 1 : 0;
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE S_SetPalette(void* self, IDirectDrawPalette* palette) {
    const HRESULT hr = Original<SetPaletteFn>(self, sslot::SetPalette)(self, palette);
    if (FAILED(hr) || InternalCall::Active() || RoleOf(self) == Role::None) return hr;

    {
        // One palette for the whole chain, like a real primary.
        Lock l;
        if (!g_fake) return hr;
        InternalCall internal;
        g_fake->front7->SetPalette(palette);
        if (g_fake->back7) g_fake->back7->SetPalette(palette);
        g_fake->palette = palette;
    }
    if (palette) PatchPalette(palette);
    Present();
    return hr;
}

// --- IDirectDrawPalette hooks ------------------------------------------------------------

HRESULT STDMETHODCALLTYPE P_SetEntries(void* self, DWORD flags, DWORD start, DWORD count,
                                       LPPALETTEENTRY entries) {
    const HRESULT hr = Original<SetEntriesFn>(self, pslot::SetEntries)(self, flags, start, count, entries);
    if (SUCCEEDED(hr) && !InternalCall::Active()) {
        bool ours;
        {
            Lock l;
            ours = g_fake && g_fake->palette == self;
        }
        // Palette fades and cycling change the picture without any blit.
        if (ours) Present();
    }
    return hr;
}

void PatchSurface(void* surface) {
    if (!surface) return;
    PatchVtable(surface, {
                             {sslot::QueryInterface, reinterpret_cast<void*>(&S_QueryInterface)},
                             {sslot::Blt, reinterpret_cast<void*>(&S_Blt)},
                             {sslot::BltFast, reinterpret_cast<void*>(&S_BltFast)},
                             {sslot::Flip, reinterpret_cast<void*>(&S_Flip)},
                             {sslot::GetAttachedSurface, reinterpret_cast<void*>(&S_GetAttachedSurface)},
                             {sslot::GetCaps, reinterpret_cast<void*>(&S_GetCaps)},
                             {sslot::GetSurfaceDesc, reinterpret_cast<void*>(&S_GetSurfaceDesc)},
                             {sslot::ReleaseDC, reinterpret_cast<void*>(&S_ReleaseDC)},
                             {sslot::SetPalette, reinterpret_cast<void*>(&S_SetPalette)},
                             {sslot::Unlock, reinterpret_cast<void*>(&S_Unlock)},
                         });
}

void PatchPalette(void* palette) {
    PatchVtable(palette, {{pslot::SetEntries, reinterpret_cast<void*>(&P_SetEntries)}});
}

template <int V>
void PatchDirectDraw(void* dd) {
    void* setDisplayMode = V == 1 ? reinterpret_cast<void*>(&DD_SetDisplayMode1<V>)
                                  : reinterpret_cast<void*>(&DD_SetDisplayMode2<V>);
    PatchVtable(dd, {
                        {ddslot::QueryInterface, reinterpret_cast<void*>(&DD_QueryInterface)},
                        {ddslot::CreateSurface, reinterpret_cast<void*>(&DD_CreateSurface<V>)},
                        {ddslot::EnumDisplayModes, reinterpret_cast<void*>(&DD_EnumDisplayModes<V>)},
                        {ddslot::GetDisplayMode, reinterpret_cast<void*>(&DD_GetDisplayMode<V>)},
                        {ddslot::RestoreDisplayMode, reinterpret_cast<void*>(&DD_RestoreDisplayMode)},
                        {ddslot::SetCooperativeLevel, reinterpret_cast<void*>(&DD_SetCooperativeLevel<V>)},
                        {ddslot::SetDisplayMode, setDisplayMode},
                        {ddslot::WaitForVerticalBlank, reinterpret_cast<void*>(&DD_WaitForVerticalBlank)},
                    });
}

void PatchDirectDrawVersion(void* dd, int version) {
    switch (version) {
    case 1: PatchDirectDraw<1>(dd); break;
    case 2: PatchDirectDraw<2>(dd); break;
    case 4: PatchDirectDraw<4>(dd); break;
    case 7: PatchDirectDraw<7>(dd); break;
    default: break;
    }
}

// --- Exports -------------------------------------------------------------------------------

HRESULT WINAPI Hook_DirectDrawCreate(GUID FAR* guid, LPDIRECTDRAW FAR* out, IUnknown FAR* outer) {
    const HRESULT hr = Real_DirectDrawCreate(guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) PatchDirectDraw<1>(*out);
    return hr;
}

HRESULT WINAPI Hook_DirectDrawCreateEx(GUID FAR* guid, LPVOID* out, REFIID iid, IUnknown FAR* outer) {
    const HRESULT hr = Real_DirectDrawCreateEx(guid, out, iid, outer);
    if (SUCCEEDED(hr) && out && *out) PatchDirectDrawVersion(*out, DirectDrawVersionOf(iid));
    return hr;
}

// Joins the caller's Detours transaction if there is one (shim attach),
// otherwise runs its own (a DLL loaded later).
class Transaction {
public:
    Transaction() : owns_(DetourTransactionBegin() == NO_ERROR) {
        if (owns_) DetourUpdateThread(GetCurrentThread());
    }
    LONG Commit(LONG err) {
        if (!owns_) return err;
        if (err != NO_ERROR) {
            DetourTransactionAbort();
            return err;
        }
        return DetourTransactionCommit();
    }

private:
    bool owns_;
};

}  // namespace

void OnLoaded(HMODULE module) {
    if (Real_DirectDrawCreate) return;
    Real_DirectDrawCreate =
        reinterpret_cast<decltype(Real_DirectDrawCreate)>(GetProcAddress(module, "DirectDrawCreate"));
    Real_DirectDrawCreateEx = reinterpret_cast<decltype(Real_DirectDrawCreateEx)>(
        GetProcAddress(module, "DirectDrawCreateEx"));

    Transaction t;
    LONG err = NO_ERROR;
    if (Real_DirectDrawCreate) HookStep(err, true, Real_DirectDrawCreate, Hook_DirectDrawCreate);
    if (Real_DirectDrawCreateEx) HookStep(err, true, Real_DirectDrawCreateEx, Hook_DirectDrawCreateEx);
    err = t.Commit(err);
    if (err != NO_ERROR) {
        Real_DirectDrawCreate = nullptr;
        Real_DirectDrawCreateEx = nullptr;
    }
    log::Write("graphics: ddraw.dll %s (error %ld)", err == NO_ERROR ? "hooked" : "NOT hooked", err);
}

void OnUnloaded(const void* base, size_t size) {
    Real_DirectDrawCreate = nullptr;
    Real_DirectDrawCreateEx = nullptr;
    ForgetVtablesInRange(base, size);
    Lock l;
    (void)g_fake.release();  // its interfaces died with ddraw.dll: don't Release them
    g_session = {};
    g_dirty = false;
    log::Write("graphics: ddraw.dll unloaded");
}

LONG Unhook() {
    LONG err = NO_ERROR;
    if (Real_DirectDrawCreate) HookStep(err, false, Real_DirectDrawCreate, Hook_DirectDrawCreate);
    if (Real_DirectDrawCreateEx)
        HookStep(err, false, Real_DirectDrawCreateEx, Hook_DirectDrawCreateEx);
    return err;
}

bool Hooked() { return Real_DirectDrawCreate != nullptr; }

void PresentPending() {
    if (g_dirty.load(std::memory_order_relaxed) && pacing::DueForPresent()) Present();
}

void GetStats(PresentStats& out) {
    Lock l;
    out = g_stats;
    out.cbSize = sizeof(PresentStats);
    out.crc32 = g_fake && !g_fake->bgra.empty()
                    ? Crc32(g_fake->bgra.data(), g_fake->bgra.size() * sizeof(uint32_t))
                    : 0;
}

}  // namespace retro::shim::gfx::ddraw
