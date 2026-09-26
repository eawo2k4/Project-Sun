// GDI: 16-bit handles over host GDI objects, window surfaces, presentation,
// text, and the GDI.EXE API.

#include <windows.h>

#include "win16/Gdi.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

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
    for (auto& [index, brush] : sysBrushes_) DeleteObject(static_cast<HGDIOBJ>(brush));
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
    if (brush >= 1 && brush <= 31) {  // COLOR_xxx + 1
        const int index = brush - 1;
        if (index > 20) return GetSysColorBrush(index);  // newer than Windows 3.1
        auto& cached = sysBrushes_[index];
        if (!cached) cached = ::CreateSolidBrush(ClassicSysColor(index));
        return cached;
    }
    return HostObject(brush, Kind::Brush);
}

bool Gdi::KindOf(uint16_t handle, Kind& kind) const {
    const Object* o = Find(handle);
    if (!o) return false;
    kind = o->kind;
    return true;
}

// --- Fonts ------------------------------------------------------------------------------------

void* Gdi::StockFont(int index) {
    // Windows 3.1 at VGA resolution: face, height, width, weight.
    struct Spec {
        int index;
        const wchar_t* face;
        int height, width, weight;
        BYTE pitch, charset;
    };
    static const Spec kFonts[] = {
        {OEM_FIXED_FONT, L"Terminal", 12, 8, FW_NORMAL, FIXED_PITCH | FF_MODERN, OEM_CHARSET},
        {ANSI_FIXED_FONT, L"Courier", 13, 0, FW_NORMAL, FIXED_PITCH | FF_MODERN, ANSI_CHARSET},
        {ANSI_VAR_FONT, L"MS Sans Serif", 13, 0, FW_NORMAL, VARIABLE_PITCH | FF_SWISS, ANSI_CHARSET},
        {SYSTEM_FONT, L"System", 16, 0, FW_BOLD, VARIABLE_PITCH | FF_SWISS, ANSI_CHARSET},
        {DEVICE_DEFAULT_FONT, L"System", 16, 0, FW_BOLD, VARIABLE_PITCH | FF_SWISS, ANSI_CHARSET},
        {SYSTEM_FIXED_FONT, L"Fixedsys", 15, 8, FW_NORMAL, FIXED_PITCH | FF_MODERN, ANSI_CHARSET},
    };
    for (const Spec& s : kFonts) {
        if (s.index != index) continue;
        void*& font = stockFonts_[index];
        if (!font) {
            LOGFONTW lf{};
            lf.lfHeight = s.height;  // cell height in pixels
            lf.lfWidth = s.width;
            lf.lfWeight = s.weight;
            lf.lfCharSet = s.charset;
            lf.lfQuality = NONANTIALIASED_QUALITY;
            lf.lfPitchAndFamily = s.pitch;
            wcscpy_s(lf.lfFaceName, s.face);
            font = CreateFontIndirectW(&lf);
            if (font) Wrap(font, Kind::Font, true, true);
        }
        return font;
    }
    return nullptr;
}

void Gdi::SelectDefaultFont(void* dc) {
    if (void* font = StockFont(SYSTEM_FONT)) SelectObject(static_cast<HDC>(dc), static_cast<HFONT>(font));
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
    SelectDefaultFont(dc);
    return true;
}

bool Gdi::ResizeSurface(uint16_t hwnd, int width, int height) {
    const auto it = surfaces_.find(hwnd);
    if (it == surfaces_.end() || width <= 0 || height <= 0) return false;
    Surface& s = it->second;
    if (s.saved > 0) return false;
    if (s.width == width && s.height == height) return true;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(static_cast<HDC>(s.dc), &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return false;
    std::memset(bits, 0, size_t(width) * height * 4);
    GdiFlush();
    // The old picture, top left.
    uint32_t* dst = static_cast<uint32_t*>(bits);
    for (int y = 0; y < std::min(height, s.height); ++y)
        std::memcpy(dst + size_t(y) * width, s.bits + size_t(y) * s.width, size_t(std::min(width, s.width)) * 4);
    HGDIOBJ old = SelectObject(static_cast<HDC>(s.dc), dib);
    DeleteObject(old);
    s.dib = dib;
    s.bits = dst;
    s.width = width;
    s.height = height;
    s.dirty = hwnd != kDesktop;
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
    HDC dc = CreateCompatibleDC(base);
    if (dc) SelectDefaultFont(dc);
    return Wrap(dc, Kind::Dc, true, false);
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
    if (void* font = StockFont(index)) return HandleForHost(font);
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

uint16_t Gdi::CreateBitmapFromDib(const uint8_t* dib, size_t size) {
    if (!dib || size < sizeof(BITMAPCOREHEADER)) return 0;
    uint32_t headerSize = 0;
    std::memcpy(&headerSize, dib, 4);
    int width = 0, height = 0, planes = 0, bpp = 0;
    uint32_t compression = BI_RGB, sizeImage = 0, colors = 0, entrySize = 0;
    if (headerSize == sizeof(BITMAPCOREHEADER)) {  // OS/2 1.x style
        BITMAPCOREHEADER h;
        std::memcpy(&h, dib, sizeof(h));
        width = h.bcWidth;
        height = h.bcHeight;
        planes = h.bcPlanes;
        bpp = h.bcBitCount;
        entrySize = sizeof(RGBTRIPLE);
        colors = bpp <= 8 ? 1u << bpp : 0;
    } else if (headerSize >= sizeof(BITMAPINFOHEADER) && headerSize <= size) {
        BITMAPINFOHEADER h;
        std::memcpy(&h, dib, sizeof(h));
        width = h.biWidth;
        height = h.biHeight;
        planes = h.biPlanes;
        bpp = h.biBitCount;
        compression = h.biCompression;
        sizeImage = h.biSizeImage;
        entrySize = sizeof(RGBQUAD);
        colors = h.biClrUsed ? h.biClrUsed : (bpp <= 8 ? 1u << bpp : 0);
        if (compression == BI_BITFIELDS && headerSize == sizeof(BITMAPINFOHEADER)) colors = 3;
    } else {
        return 0;
    }
    const bool rle = compression == BI_RLE8 || compression == BI_RLE4;
    if (width <= 0 || height == 0 || height == INT32_MIN || planes != 1 ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) || colors > 256 ||
        (compression != BI_RGB && compression != BI_BITFIELDS && !rle))
        return 0;
    const uint64_t rows = uint64_t(height < 0 ? -int64_t(height) : height);
    const uint64_t stride = ((uint64_t(width) * uint64_t(bpp) + 31) / 32) * 4;
    const uint64_t bitsOffset = uint64_t(headerSize) + uint64_t(colors) * entrySize;
    const uint64_t bitsSize = rle ? sizeImage : stride * rows;
    if (bitsSize == 0 || bitsOffset + bitsSize > size) return 0;

    // Copies with the alignment GDI expects.
    std::vector<uint32_t> info(size_t((bitsOffset + 3) / 4));
    std::memcpy(info.data(), dib, size_t(bitsOffset));
    const std::vector<uint8_t> bits(dib + bitsOffset, dib + bitsOffset + bitsSize);
    const BITMAPINFO* bmi = reinterpret_cast<const BITMAPINFO*>(info.data());

    HDC screen = GetDC(nullptr);
    HBITMAP bmp = nullptr;
    if (bpp == 1) {
        bmp = ::CreateBitmap(width, int(rows), 1, 1, nullptr);
        if (bmp && !SetDIBits(screen, bmp, 0, UINT(rows), bits.data(), bmi, DIB_RGB_COLORS)) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
    } else {
        bmp = CreateDIBitmap(screen, reinterpret_cast<const BITMAPINFOHEADER*>(info.data()), CBM_INIT,
                             bits.data(), bmi, DIB_RGB_COLORS);
    }
    ReleaseDC(nullptr, screen);
    return Wrap(bmp, Kind::Bitmap, true, false);
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

// --- Text ---

void Api_SetBkColor(Runtime& rt, Cpu& cpu) {  // (HDC, COLORREF) -> previous
    const PascalArgs a(cpu, {2, 4});
    HDC dc = Dc(rt, a.Word(0));
    SetResult(cpu, dc ? SetBkColor(dc, a.Long(1)) : CLR_INVALID);
    cpu.ReturnFar(a.Bytes());
}

void Api_SetBkMode(Runtime& rt, Cpu& cpu) {  // (HDC, TRANSPARENT 1 | OPAQUE 2) -> previous
    const PascalArgs a(cpu, {2, 2});
    HDC dc = Dc(rt, a.Word(0));
    cpu.Regs().r[AX] = dc ? uint16_t(SetBkMode(dc, a.Int(1))) : 0;
    cpu.ReturnFar(a.Bytes());
}

void Api_SetTextColor(Runtime& rt, Cpu& cpu) {  // (HDC, COLORREF) -> previous
    const PascalArgs a(cpu, {2, 4});
    HDC dc = Dc(rt, a.Word(0));
    SetResult(cpu, dc ? SetTextColor(dc, a.Long(1)) : CLR_INVALID);
    cpu.ReturnFar(a.Bytes());
}

void Api_GetTextColor(Runtime& rt, Cpu& cpu) {  // (HDC) -> COLORREF
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    SetResult(cpu, dc ? GetTextColor(dc) : 0);
    cpu.ReturnFar(a.Bytes());
}

void Api_TextOut(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, LPCSTR, int count) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    const FarPtr str = a.Ptr(3);
    const int count = a.Int(4);
    BOOL ok = FALSE;
    if (dc && count >= 0) {
        if (count > 0) rt.Mem().Translate(str.sel, str.off, uint32_t(count), Access::Read);  // #GP if out of bounds
        std::string text(size_t(count), '\0');
        for (int i = 0; i < count; ++i) text[size_t(i)] = char(rt.Mem().Read8(str.sel, uint16_t(str.off + i)));
        // Win16 text is ANSI (code page 1252), whatever the host's code page.
        std::wstring wide(text.size(), L'\0');
        const int n = text.empty() ? 0
                                   : MultiByteToWideChar(1252, 0, text.data(), int(text.size()), wide.data(),
                                                         int(wide.size()));
        ok = TextOutW(dc, a.Int(1), a.Int(2), wide.data(), n);
    }
    cpu.Regs().r[AX] = ok ? 1 : 0;
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
    std::vector<ApiFunction> api = GdiDrawApi();
    api.insert(api.end(), {
        {1, "SETBKCOLOR", Api_SetBkColor},
        {2, "SETBKMODE", Api_SetBkMode},
        {9, "SETTEXTCOLOR", Api_SetTextColor},
        {27, "RECTANGLE", Api_Rectangle},
        {29, "PATBLT", Api_PatBlt},
        {31, "SETPIXEL", Api_SetPixel},
        {34, "BITBLT", Api_BitBlt},
        {33, "TEXTOUT", Api_TextOut},
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
        {90, "GETTEXTCOLOR", Api_GetTextColor},
    });
    return api;
}

}  // namespace retro::win16
