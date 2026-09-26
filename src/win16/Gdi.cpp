// GDI: 16-bit handles over host GDI objects, window surfaces, presentation,
// and the GDI.EXE API.

#include <windows.h>

#include "win16/Gdi.h"

#include <cstring>

#include "retro/FramePacing.h"
#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

constexpr uint16_t kDesktop = 0xFFFF;

Gdi::Kind KindOfHost(HGDIOBJ obj) {
    switch (GetObjectType(obj)) {
    case OBJ_DC:
    case OBJ_MEMDC: return Gdi::Kind::Dc;
    case OBJ_BITMAP: return Gdi::Kind::Bitmap;
    case OBJ_BRUSH: return Gdi::Kind::Brush;
    case OBJ_PEN: return Gdi::Kind::Pen;
    case OBJ_FONT: return Gdi::Kind::Font;
    case OBJ_PAL: return Gdi::Kind::Palette;
    default: return Gdi::Kind::Other;
    }
}

}  // namespace

Gdi::Gdi(Runtime& rt)
    : rt_(rt),
      scheduler_(std::make_unique<FrameScheduler>(60, QpcFrequency())),
      waiter_(std::make_unique<PreciseWaiter>()) {}

Gdi::~Gdi() {
    for (auto& [hwnd, s] : surfaces_) FreeSurface(s);
    // DCs first, so no owned object is still selected when it's deleted.
    for (const auto& [h, o] : objects_) {
        if (o.owned && o.kind == Kind::Dc) DeleteDC(static_cast<HDC>(o.host));
    }
    for (const auto& [h, o] : objects_) {
        if (o.owned && o.kind != Kind::Dc) DeleteObject(static_cast<HGDIOBJ>(o.host));
    }
}

// --- Handle table ------------------------------------------------------------------------

uint16_t Gdi::Wrap(void* host, Kind kind, bool owned, bool stock, uint16_t window) {
    if (!host) return 0;
    if (const auto it = byHost_.find(host); it != byHost_.end()) return it->second;
    const uint16_t h = nextHandle_;
    nextHandle_ = uint16_t(nextHandle_ + 4);
    objects_[h] = Object{kind, host, owned, stock, window};
    byHost_[host] = h;
    return h;
}

void Gdi::Unwrap(uint16_t handle) {
    const auto it = objects_.find(handle);
    if (it == objects_.end()) return;
    byHost_.erase(it->second.host);
    objects_.erase(it);
}

const Gdi::Object* Gdi::Find(uint16_t handle) const {
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : &it->second;
}

void* Gdi::HostDc(uint16_t hdc) const { return HostObject(hdc, Kind::Dc); }

void* Gdi::HostObject(uint16_t handle, Kind kind) const {
    const Object* o = Find(handle);
    return o && o->kind == kind ? o->host : nullptr;
}

void* Gdi::HostBrush(uint16_t brush) const {
    if (brush >= 1 && brush <= 31) return GetSysColorBrush(brush - 1);  // COLOR_xxx + 1
    return HostObject(brush, Kind::Brush);
}

uint16_t Gdi::HandleForHost(void* host) const {
    const auto it = byHost_.find(host);
    return it == byHost_.end() ? 0 : it->second;
}

// --- Surfaces ---------------------------------------------------------------------------------

bool Gdi::MakeSurface(Surface& s, int width, int height) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!dib) {
        if (dc) DeleteDC(dc);
        return false;
    }
    s.dc = dc;
    s.dib = dib;
    s.previousBitmap = SelectObject(dc, dib);
    s.bits = static_cast<uint32_t*>(bits);
    s.width = width;
    s.height = height;
    std::memset(bits, 0, size_t(width) * height * 4);  // black until painted
    return true;
}

void Gdi::FreeSurface(Surface& s) {
    if (!s.dc) return;
    SelectObject(static_cast<HDC>(s.dc), static_cast<HGDIOBJ>(s.previousBitmap));
    DeleteObject(static_cast<HGDIOBJ>(s.dib));
    DeleteDC(static_cast<HDC>(s.dc));
    s = Surface{};
}

bool Gdi::CreateSurface(uint16_t hwnd, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    Surface s;
    if (!MakeSurface(s, width, height)) return false;
    s.hdc16 = Wrap(s.dc, Kind::Dc, false, false, hwnd);
    surfaces_[hwnd] = s;
    return true;
}

void Gdi::DestroySurface(uint16_t hwnd) {
    const auto it = surfaces_.find(hwnd);
    if (it == surfaces_.end()) return;
    Unwrap(it->second.hdc16);
    FreeSurface(it->second);
    surfaces_.erase(it);
}

Gdi::Surface* Gdi::SurfaceFor(uint16_t hwnd) {
    if (hwnd == 0) {  // the desktop: drawable, never presented
        if (!surfaces_.count(kDesktop) && !CreateSurface(kDesktop, kScreenWidth, kScreenHeight))
            return nullptr;
        hwnd = kDesktop;
    }
    const auto it = surfaces_.find(hwnd);
    return it == surfaces_.end() ? nullptr : &it->second;
}

const uint32_t* Gdi::SurfacePixels(uint16_t hwnd, int& width, int& height) const {
    const auto it = surfaces_.find(hwnd);
    if (it == surfaces_.end()) return nullptr;
    GdiFlush();
    width = it->second.width;
    height = it->second.height;
    return it->second.bits;
}

// --- DCs --------------------------------------------------------------------------------------

uint16_t Gdi::GetWindowDc(uint16_t hwnd) {
    Surface* s = SurfaceFor(hwnd);
    if (!s) return 0;
    SaveDC(static_cast<HDC>(s->dc));  // every GetDC starts from the default state
    ++s->saved;
    return s->hdc16;
}

bool Gdi::ReleaseWindowDc(uint16_t hdc) {
    const Object* o = Find(hdc);
    if (!o || o->kind != Kind::Dc || !o->window) return false;
    for (auto& [hwnd, s] : surfaces_) {
        if (s.hdc16 != hdc) continue;
        if (s.saved > 0) {
            RestoreDC(static_cast<HDC>(s.dc), -1);
            --s.saved;
        }
        s.dirty = hwnd != kDesktop;
        return true;
    }
    return false;
}

uint16_t Gdi::CreateCompatibleDc(uint16_t hdc) {
    HDC base = hdc ? static_cast<HDC>(HostDc(hdc)) : nullptr;
    if (hdc && !base) return 0;
    return Wrap(CreateCompatibleDC(base), Kind::Dc, true, false);
}

bool Gdi::DeleteDc(uint16_t hdc) {
    const Object* o = Find(hdc);
    if (!o || o->kind != Kind::Dc || !o->owned) return false;  // window DCs aren't deleted
    const BOOL ok = DeleteDC(static_cast<HDC>(o->host));
    Unwrap(hdc);
    return ok != FALSE;
}

void Gdi::ClipTo(uint16_t hdc, const Rect16& r) {
    if (HDC dc = static_cast<HDC>(HostDc(hdc))) IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
}

// --- Objects -----------------------------------------------------------------------------------

uint16_t Gdi::StockObject(int index) {
    HGDIOBJ obj = GetStockObject(index);
    return obj ? Wrap(obj, KindOfHost(obj), false, true) : 0;
}

uint16_t Gdi::CreateBitmap(int width, int height, int planes, int bitsPerPixel, const void* bits) {
    return Wrap(::CreateBitmap(width, height, UINT(planes), UINT(bitsPerPixel), bits), Kind::Bitmap,
                true, false);
}

uint16_t Gdi::CreateCompatibleBitmap(uint16_t hdc, int width, int height) {
    HDC dc = static_cast<HDC>(HostDc(hdc));
    return dc ? Wrap(::CreateCompatibleBitmap(dc, width, height), Kind::Bitmap, true, false) : 0;
}

uint16_t Gdi::CreateSolidBrush(uint32_t color) {
    return Wrap(::CreateSolidBrush(color), Kind::Brush, true, false);
}

uint16_t Gdi::CreatePen(int style, int width, uint32_t color) {
    return Wrap(::CreatePen(style, width, color), Kind::Pen, true, false);
}

uint16_t Gdi::Select(uint16_t hdc, uint16_t object) {
    HDC dc = static_cast<HDC>(HostDc(hdc));
    const Object* o = Find(object);
    if (!dc || !o || o->kind == Kind::Dc) return 0;
    HGDIOBJ previous = SelectObject(dc, static_cast<HGDIOBJ>(o->host));
    if (!previous || previous == HGDI_ERROR) return 0;
    // The previous object may be one the host created (a new DC's defaults):
    // give it a 16-bit handle too, without taking ownership.
    return Wrap(previous, KindOfHost(previous), false, false);
}

bool Gdi::Delete(uint16_t object) {
    const Object* o = Find(object);
    if (!o || o->kind == Kind::Dc) return false;
    if (o->stock || !o->owned) return true;  // stock/borrowed objects: nothing to free

    // Win16 refuses to delete an object still selected into a DC. Modern GDI
    // doesn't (it deletes selected bitmaps, defers brushes), so check ourselves.
    UINT type = 0;
    switch (o->kind) {
    case Kind::Bitmap: type = OBJ_BITMAP; break;
    case Kind::Brush: type = OBJ_BRUSH; break;
    case Kind::Pen: type = OBJ_PEN; break;
    case Kind::Font: type = OBJ_FONT; break;
    case Kind::Palette: type = OBJ_PAL; break;
    default: break;
    }
    if (type) {
        for (const auto& [h, dc] : objects_) {
            if (dc.kind == Kind::Dc && GetCurrentObject(static_cast<HDC>(dc.host), type) == o->host)
                return false;
        }
    }
    const BOOL ok = DeleteObject(static_cast<HGDIOBJ>(o->host));
    if (ok) Unwrap(object);
    return ok != FALSE;
}

bool Gdi::Fill(uint16_t hdc, const Rect16& r, uint16_t brush) {
    HDC dc = static_cast<HDC>(HostDc(hdc));
    HBRUSH b = static_cast<HBRUSH>(HostBrush(brush));
    if (!dc || !b) return false;
    const RECT rc{r.left, r.top, r.right, r.bottom};
    return FillRect(dc, &rc, b) != 0;
}

// --- Presentation ---------------------------------------------------------------------------

void Gdi::SetFrameCap(uint32_t fps) {
    scheduler_ = std::make_unique<FrameScheduler>(fps, QpcFrequency());
}

void Gdi::PresentPending() {
    bool any = false;
    for (const auto& [hwnd, s] : surfaces_) any = any || s.dirty;
    if (!any) return;

    // One frame slot for everything drawn since the last present.
    const int64_t now = QpcNow();
    const int64_t at = scheduler_->NextPresentTime(now);
    if (at > now) waiter_->WaitUntil(at);

    GdiFlush();
    for (auto& [hwnd, s] : surfaces_) {
        if (!s.dirty) continue;
        s.dirty = false;
        if (const uint64_t host = rt_.Windows().HostForHwnd(hwnd))
            rt_.Windows().Host().Present(host, s.bits, s.width, s.height);
    }
    ++presents_;
}

// --- GDI.EXE ----------------------------------------------------------------------------------

namespace {

HDC Dc(Runtime& rt, uint16_t hdc) { return static_cast<HDC>(rt.Graphics().HostDc(hdc)); }

void Api_Rectangle(Runtime& rt, Cpu& cpu) {  // (HDC, left, top, right, bottom)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    cpu.Regs().r[AX] = dc && Rectangle(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_PatBlt(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, w, h, DWORD rop)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    cpu.Regs().r[AX] = dc && PatBlt(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4), a.Long(5)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_SetPixel(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, COLORREF) -> COLORREF
    const PascalArgs a(cpu, {2, 2, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    SetResult(cpu, dc ? SetPixel(dc, a.Int(1), a.Int(2), a.Long(3)) : CLR_INVALID);
    cpu.ReturnFar(a.Bytes());
}

void Api_GetPixel(Runtime& rt, Cpu& cpu) {  // (HDC, x, y) -> COLORREF
    const PascalArgs a(cpu, {2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    GdiFlush();
    SetResult(cpu, dc ? GetPixel(dc, a.Int(1), a.Int(2)) : CLR_INVALID);
    cpu.ReturnFar(a.Bytes());
}

void Api_BitBlt(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, w, h, HDC src, xSrc, ySrc, DWORD rop)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 4});
    HDC dst = Dc(rt, a.Word(0));
    HDC src = Dc(rt, a.Word(5));  // may be null for pattern-only ROPs
    cpu.Regs().r[AX] = dst && BitBlt(dst, a.Int(1), a.Int(2), a.Int(3), a.Int(4), src, a.Int(6),
                                     a.Int(7), a.Long(8))
                           ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_StretchBlt(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, w, h, HDC src, xs, ys, ws, hs, rop)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4});
    HDC dst = Dc(rt, a.Word(0));
    HDC src = Dc(rt, a.Word(5));
    cpu.Regs().r[AX] = dst && StretchBlt(dst, a.Int(1), a.Int(2), a.Int(3), a.Int(4), src, a.Int(6),
                                         a.Int(7), a.Int(8), a.Int(9), a.Long(10))
                           ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_SelectObject(Runtime& rt, Cpu& cpu) {  // (HDC, HGDIOBJ) -> previous
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = rt.Graphics().Select(a.Word(0), a.Word(1));
    cpu.ReturnFar(a.Bytes());
}

void Api_CreateBitmap(Runtime& rt, Cpu& cpu) {  // (w, h, planes, bpp, const void FAR* bits)
    const PascalArgs a(cpu, {2, 2, 2, 2, 4});
    const int w = a.Int(0), h = a.Int(1), planes = a.Word(2), bpp = a.Word(3);
    const FarPtr bits = a.Ptr(4);
    std::vector<uint8_t> data;
    if (!bits.IsNull() && w > 0 && h > 0 && planes > 0 && bpp > 0) {
        const uint32_t rowBytes = ((uint32_t(w) * bpp + 15) / 16) * 2;  // rows padded to a WORD
        const uint32_t total = rowBytes * uint32_t(h) * uint32_t(planes);
        data.resize(total);
        for (uint32_t i = 0; i < total; ++i) data[i] = rt.Mem().Read8(bits.sel, uint16_t(bits.off + i));
    }
    cpu.Regs().r[AX] = w > 0 && h > 0 && planes > 0 && bpp > 0
                           ? rt.Graphics().CreateBitmap(w, h, planes, bpp, data.empty() ? nullptr : data.data())
                           : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_CreateCompatibleBitmap(Runtime& rt, Cpu& cpu) {  // (HDC, w, h)
    const PascalArgs a(cpu, {2, 2, 2});
    cpu.Regs().r[AX] = rt.Graphics().CreateCompatibleBitmap(a.Word(0), a.Int(1), a.Int(2));
    cpu.ReturnFar(a.Bytes());
}

void Api_CreateCompatibleDC(Runtime& rt, Cpu& cpu) {  // (HDC)
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Graphics().CreateCompatibleDc(a.Word(0));
    cpu.ReturnFar(a.Bytes());
}

void Api_CreatePen(Runtime& rt, Cpu& cpu) {  // (style, width, COLORREF)
    const PascalArgs a(cpu, {2, 2, 4});
    cpu.Regs().r[AX] = rt.Graphics().CreatePen(a.Int(0), a.Int(1), a.Long(2));
    cpu.ReturnFar(a.Bytes());
}

void Api_CreateSolidBrush(Runtime& rt, Cpu& cpu) {  // (COLORREF)
    const PascalArgs a(cpu, {4});
    cpu.Regs().r[AX] = rt.Graphics().CreateSolidBrush(a.Long(0));
    cpu.ReturnFar(a.Bytes());
}

void Api_DeleteDC(Runtime& rt, Cpu& cpu) {  // (HDC)
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Graphics().DeleteDc(a.Word(0)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_DeleteObject(Runtime& rt, Cpu& cpu) {  // (HGDIOBJ)
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Graphics().Delete(a.Word(0)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_GetStockObject(Runtime& rt, Cpu& cpu) {  // (int)
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Graphics().StockObject(a.Int(0));
    cpu.ReturnFar(a.Bytes());
}

}  // namespace

std::vector<ApiFunction> GdiApi() {
    return {
        {27, "RECTANGLE", Api_Rectangle},
        {29, "PATBLT", Api_PatBlt},
        {31, "SETPIXEL", Api_SetPixel},
        {34, "BITBLT", Api_BitBlt},
        {35, "STRETCHBLT", Api_StretchBlt},
        {45, "SELECTOBJECT", Api_SelectObject},
        {48, "CREATEBITMAP", Api_CreateBitmap},
        {51, "CREATECOMPATIBLEBITMAP", Api_CreateCompatibleBitmap},
        {52, "CREATECOMPATIBLEDC", Api_CreateCompatibleDC},
        {61, "CREATEPEN", Api_CreatePen},
        {66, "CREATESOLIDBRUSH", Api_CreateSolidBrush},
        {68, "DELETEDC", Api_DeleteDC},
        {69, "DELETEOBJECT", Api_DeleteObject},
        {83, "GETPIXEL", Api_GetPixel},
        {87, "GETSTOCKOBJECT", Api_GetStockObject},
    };
}

}  // namespace retro::win16
