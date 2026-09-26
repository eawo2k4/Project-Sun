// GDI.EXE: fonts and text metrics, lines and shapes, brushes and pens, DC
// state and mapping modes, device capabilities, GetObject, device-independent
// bitmaps; and USER's GDI-based drawing (DrawText, FrameRect, InvertRect).

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

HDC Dc(Runtime& rt, uint16_t hdc) { return static_cast<HDC>(rt.Graphics().HostDc(hdc)); }

void Return(Cpu& cpu, const PascalArgs& a, uint32_t value) {
    SetResult(cpu, value);
    cpu.ReturnFar(a.Bytes());
}

uint32_t MakeLong(int lo, int hi) { return uint16_t(lo) | (uint32_t(uint16_t(hi)) << 16); }

RECT ReadRect(const Memory& mem, FarPtr p) {
    return {int16_t(mem.Read16(p.sel, p.off)), int16_t(mem.Read16(p.sel, uint16_t(p.off + 2))),
            int16_t(mem.Read16(p.sel, uint16_t(p.off + 4))), int16_t(mem.Read16(p.sel, uint16_t(p.off + 6)))};
}

void WriteRect(Memory& mem, FarPtr p, const RECT& r) {
    mem.Write16(p.sel, p.off, uint16_t(r.left));
    mem.Write16(p.sel, uint16_t(p.off + 2), uint16_t(r.top));
    mem.Write16(p.sel, uint16_t(p.off + 4), uint16_t(r.right));
    mem.Write16(p.sel, uint16_t(p.off + 6), uint16_t(r.bottom));
}

// `count` characters at p (-1: up to the NUL), as UTF-16 from code page 1252.
std::wstring ReadText(Runtime& rt, FarPtr p, int count) {
    std::string s;
    if (p.IsNull()) return {};
    if (count < 0) {
        s = rt.Mem().ReadString(p.sel, p.off, 0xFFFF);
    } else if (count > 0) {
        rt.Mem().Translate(p.sel, p.off, uint32_t(count), Access::Read);  // #GP if out of bounds
        s.assign(reinterpret_cast<const char*>(rt.Mem().SegmentData(p.sel) + p.off), size_t(count));
    }
    std::wstring w(s.size(), L' ');
    const int n = s.empty() ? 0 : MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    w.resize(size_t(n));
    return w;
}

std::string Narrow(const wchar_t* w) {
    char buf[64] = {};
    WideCharToMultiByte(1252, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

// --- Fonts and text --------------------------------------------------------------------------

// LOGFONT (Win16, 50 bytes): 5 ints, 8 bytes, lfFaceName[32].
LOGFONTW ReadLogFont(const Memory& mem, FarPtr p) {
    LOGFONTW lf{};
    lf.lfHeight = int16_t(mem.Read16(p.sel, p.off));
    lf.lfWidth = int16_t(mem.Read16(p.sel, uint16_t(p.off + 2)));
    lf.lfEscapement = int16_t(mem.Read16(p.sel, uint16_t(p.off + 4)));
    lf.lfOrientation = int16_t(mem.Read16(p.sel, uint16_t(p.off + 6)));
    lf.lfWeight = int16_t(mem.Read16(p.sel, uint16_t(p.off + 8)));
    BYTE* bytes[] = {&lf.lfItalic, &lf.lfUnderline, &lf.lfStrikeOut, &lf.lfCharSet,
                     &lf.lfOutPrecision, &lf.lfClipPrecision, &lf.lfQuality, &lf.lfPitchAndFamily};
    for (int i = 0; i < 8; ++i) *bytes[i] = mem.Read8(p.sel, uint16_t(p.off + 10 + i));
    const std::string face = mem.ReadString(p.sel, uint16_t(p.off + 18), 31);
    MultiByteToWideChar(1252, 0, face.c_str(), -1, lf.lfFaceName, LF_FACESIZE);
    return lf;
}

void WriteLogFont(Memory& mem, FarPtr p, const LOGFONTW& lf, uint16_t size) {
    uint8_t b[50] = {};
    auto put16 = [&](int at, int v) {
        b[at] = uint8_t(v);
        b[at + 1] = uint8_t(v >> 8);
    };
    put16(0, lf.lfHeight);
    put16(2, lf.lfWidth);
    put16(4, lf.lfEscapement);
    put16(6, lf.lfOrientation);
    put16(8, lf.lfWeight);
    const BYTE bytes[] = {lf.lfItalic, lf.lfUnderline, lf.lfStrikeOut, lf.lfCharSet,
                          lf.lfOutPrecision, lf.lfClipPrecision, lf.lfQuality, lf.lfPitchAndFamily};
    std::memcpy(b + 10, bytes, 8);
    const std::string face = Narrow(lf.lfFaceName);
    std::memcpy(b + 18, face.c_str(), std::min<size_t>(face.size(), 31));
    for (uint16_t i = 0; i < std::min<uint16_t>(size, 50); ++i) mem.Write8(p.sel, uint16_t(p.off + i), b[i]);
}

uint16_t MakeFont(Runtime& rt, LOGFONTW lf) {
    lf.lfQuality = NONANTIALIASED_QUALITY;  // Windows 3.1 had no font smoothing
    return rt.Graphics().Own(CreateFontIndirectW(&lf), Gdi::Kind::Font);
}

void Api_CreateFont(Runtime& rt, Cpu& cpu) {
    // (height, width, escapement, orientation, weight, italic, underline,
    //  strikeout, charset, outprecision, clipprecision, quality, pitchfamily, face)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4});
    LOGFONTW lf{};
    lf.lfHeight = a.Int(0);
    lf.lfWidth = a.Int(1);
    lf.lfEscapement = a.Int(2);
    lf.lfOrientation = a.Int(3);
    lf.lfWeight = a.Int(4);
    lf.lfItalic = BYTE(a.Word(5));
    lf.lfUnderline = BYTE(a.Word(6));
    lf.lfStrikeOut = BYTE(a.Word(7));
    lf.lfCharSet = BYTE(a.Word(8));
    lf.lfOutPrecision = BYTE(a.Word(9));
    lf.lfClipPrecision = BYTE(a.Word(10));
    lf.lfPitchAndFamily = BYTE(a.Word(12));
    const FarPtr face = a.Ptr(13);
    if (!face.IsNull()) {
        const std::string f = rt.Mem().ReadString(face.sel, face.off, 31);
        MultiByteToWideChar(1252, 0, f.c_str(), -1, lf.lfFaceName, LF_FACESIZE);
    }
    Return(cpu, a, MakeFont(rt, lf));
}

void Api_CreateFontIndirect(Runtime& rt, Cpu& cpu) {  // (const LOGFONT FAR*)
    const PascalArgs a(cpu, {4});
    Return(cpu, a, a.Ptr(0).IsNull() ? 0 : MakeFont(rt, ReadLogFont(rt.Mem(), a.Ptr(0))));
}

// TEXTMETRIC (Win16, 31 bytes): 8 ints, 9 bytes, 3 ints.
void Api_GetTextMetrics(Runtime& rt, Cpu& cpu) {  // (HDC, TEXTMETRIC FAR*) -> BOOL
    const PascalArgs a(cpu, {2, 4});
    HDC dc = Dc(rt, a.Word(0));
    TEXTMETRICW tm{};
    const bool ok = dc && GetTextMetricsW(dc, &tm);
    if (ok) {
        const FarPtr p = a.Ptr(1);
        Memory& mem = rt.Mem();
        const LONG ints[] = {tm.tmHeight, tm.tmAscent, tm.tmDescent, tm.tmInternalLeading,
                             tm.tmExternalLeading, tm.tmAveCharWidth, tm.tmMaxCharWidth, tm.tmWeight};
        for (int i = 0; i < 8; ++i) mem.Write16(p.sel, uint16_t(p.off + 2 * i), uint16_t(ints[i]));
        const BYTE bytes[] = {tm.tmItalic, tm.tmUnderlined, tm.tmStruckOut,
                              BYTE(std::min<WCHAR>(tm.tmFirstChar, 255)), BYTE(std::min<WCHAR>(tm.tmLastChar, 255)),
                              BYTE(std::min<WCHAR>(tm.tmDefaultChar, 255)), BYTE(std::min<WCHAR>(tm.tmBreakChar, 255)),
                              tm.tmPitchAndFamily, tm.tmCharSet};
        for (int i = 0; i < 9; ++i) mem.Write8(p.sel, uint16_t(p.off + 16 + i), bytes[i]);
        mem.Write16(p.sel, uint16_t(p.off + 25), uint16_t(tm.tmOverhang));
        mem.Write16(p.sel, uint16_t(p.off + 27), uint16_t(tm.tmDigitizedAspectX));
        mem.Write16(p.sel, uint16_t(p.off + 29), uint16_t(tm.tmDigitizedAspectY));
    }
    Return(cpu, a, ok ? 1 : 0);
}

SIZE TextSize(Runtime& rt, HDC dc, FarPtr text, int count) {
    SIZE size{};
    if (dc) {
        const std::wstring w = ReadText(rt, text, std::max<int>(count, 0));
        GetTextExtentPoint32W(dc, w.c_str(), int(w.size()), &size);
    }
    return size;
}

void Api_GetTextExtent(Runtime& rt, Cpu& cpu) {  // (HDC, LPCSTR, int count) -> DWORD cx | cy << 16
    const PascalArgs a(cpu, {2, 4, 2});
    const SIZE s = TextSize(rt, Dc(rt, a.Word(0)), a.Ptr(1), a.Int(2));
    Return(cpu, a, MakeLong(s.cx, s.cy));
}

void Api_GetTextExtentPoint(Runtime& rt, Cpu& cpu) {  // (HDC, LPCSTR, int, SIZE FAR*) -> BOOL
    const PascalArgs a(cpu, {2, 4, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    const SIZE s = TextSize(rt, dc, a.Ptr(1), a.Int(2));
    const FarPtr out = a.Ptr(3);
    if (dc && !out.IsNull()) {
        rt.Mem().Write16(out.sel, out.off, uint16_t(s.cx));
        rt.Mem().Write16(out.sel, uint16_t(out.off + 2), uint16_t(s.cy));
    }
    Return(cpu, a, dc ? 1 : 0);
}

void Api_SetTextAlign(Runtime& rt, Cpu& cpu) {  // (HDC, flags) -> previous
    const PascalArgs a(cpu, {2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(SetTextAlign(dc, a.Word(1))) : 0);
}

void Api_GetTextAlign(Runtime& rt, Cpu& cpu) {  // (HDC) -> flags
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(GetTextAlign(dc)) : 0);
}

void Api_SetTextCharacterExtra(Runtime& rt, Cpu& cpu) {  // (HDC, int) -> previous
    const PascalArgs a(cpu, {2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(SetTextCharacterExtra(dc, a.Int(1))) : 0);
}

void Api_GetTextFace(Runtime& rt, Cpu& cpu) {  // (HDC, int count, LPSTR) -> length
    const PascalArgs a(cpu, {2, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    wchar_t face[LF_FACESIZE] = {};
    uint16_t n = 0;
    const FarPtr buf = a.Ptr(2);
    if (dc && GetTextFaceW(dc, LF_FACESIZE, face) && !buf.IsNull() && a.Int(1) > 0) {
        const std::string s = Narrow(face);
        n = uint16_t(std::min<size_t>(s.size(), size_t(a.Int(1) - 1)));
        for (uint16_t i = 0; i < n; ++i) rt.Mem().Write8(buf.sel, uint16_t(buf.off + i), uint8_t(s[i]));
        rt.Mem().Write8(buf.sel, uint16_t(buf.off + n), 0);
    }
    Return(cpu, a, n);
}

// (HDC, x, y, options, const RECT FAR*, LPCSTR, count, const int FAR* dx) -> BOOL
void Api_ExtTextOut(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 2, 2, 4, 4, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    BOOL ok = FALSE;
    if (dc) {
        const FarPtr rp = a.Ptr(4), dxp = a.Ptr(7);
        RECT r{};
        if (!rp.IsNull()) r = ReadRect(rt.Mem(), rp);
        const std::wstring text = ReadText(rt, a.Ptr(5), std::max<int>(a.Int(6), 0));
        std::vector<INT> dx;
        if (!dxp.IsNull()) {
            for (size_t i = 0; i < text.size(); ++i)
                dx.push_back(int16_t(rt.Mem().Read16(dxp.sel, uint16_t(dxp.off + 2 * i))));
        }
        ok = ExtTextOutW(dc, a.Int(1), a.Int(2), a.Word(3), rp.IsNull() ? nullptr : &r, text.c_str(),
                         UINT(text.size()), dx.empty() ? nullptr : dx.data());
    }
    Return(cpu, a, ok ? 1 : 0);
}

// --- Lines and shapes --------------------------------------------------------------------------

void Api_MoveTo(Runtime& rt, Cpu& cpu) {  // (HDC, x, y) -> previous position
    const PascalArgs a(cpu, {2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    POINT old{};
    if (dc) MoveToEx(dc, a.Int(1), a.Int(2), &old);
    Return(cpu, a, dc ? MakeLong(old.x, old.y) : 0);
}

void Api_MoveToEx(Runtime& rt, Cpu& cpu) {  // (HDC, x, y, POINT FAR* old) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    POINT old{};
    const bool ok = dc && MoveToEx(dc, a.Int(1), a.Int(2), &old);
    const FarPtr p = a.Ptr(3);
    if (ok && !p.IsNull()) {
        rt.Mem().Write16(p.sel, p.off, uint16_t(old.x));
        rt.Mem().Write16(p.sel, uint16_t(p.off + 2), uint16_t(old.y));
    }
    Return(cpu, a, ok ? 1 : 0);
}

void Api_GetCurrentPosition(Runtime& rt, Cpu& cpu) {  // (HDC) -> DWORD
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    POINT p{};
    if (dc) GetCurrentPositionEx(dc, &p);
    Return(cpu, a, MakeLong(p.x, p.y));
}

void Api_LineTo(Runtime& rt, Cpu& cpu) {  // (HDC, x, y) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && LineTo(dc, a.Int(1), a.Int(2)) ? 1 : 0);
}

void Api_Ellipse(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && Ellipse(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4)) ? 1 : 0);
}

void Api_RoundRect(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b, w, h) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && RoundRect(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4), a.Int(5), a.Int(6)) ? 1 : 0);
}

void Api_Arc(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b, x1, y1, x2, y2) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && Arc(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4), a.Int(5), a.Int(6), a.Int(7), a.Int(8)) ? 1 : 0);
}

void Api_Pie(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b, x1, y1, x2, y2) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && Pie(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4), a.Int(5), a.Int(6), a.Int(7), a.Int(8)) ? 1 : 0);
}

std::vector<POINT> ReadPoints(const Memory& mem, FarPtr p, int count) {
    std::vector<POINT> pts;
    for (int i = 0; i < count; ++i) {
        pts.push_back({int16_t(mem.Read16(p.sel, uint16_t(p.off + 4 * i))),
                       int16_t(mem.Read16(p.sel, uint16_t(p.off + 4 * i + 2)))});
    }
    return pts;
}

void Api_Polygon(Runtime& rt, Cpu& cpu) {  // (HDC, const POINT FAR*, count) -> BOOL
    const PascalArgs a(cpu, {2, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    const std::vector<POINT> pts = ReadPoints(rt.Mem(), a.Ptr(1), std::max<int>(a.Int(2), 0));
    Return(cpu, a, dc && pts.size() >= 2 && Polygon(dc, pts.data(), int(pts.size())) ? 1 : 0);
}

void Api_Polyline(Runtime& rt, Cpu& cpu) {  // (HDC, const POINT FAR*, count) -> BOOL
    const PascalArgs a(cpu, {2, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    const std::vector<POINT> pts = ReadPoints(rt.Mem(), a.Ptr(1), std::max<int>(a.Int(2), 0));
    Return(cpu, a, dc && pts.size() >= 2 && Polyline(dc, pts.data(), int(pts.size())) ? 1 : 0);
}

// --- Brushes and pens ------------------------------------------------------------------------

void Api_CreateHatchBrush(Runtime& rt, Cpu& cpu) {  // (style, COLORREF)
    const PascalArgs a(cpu, {2, 4});
    Return(cpu, a, rt.Graphics().Own(CreateHatchBrush(a.Int(0), a.Long(1)), Gdi::Kind::Brush));
}

void Api_CreatePatternBrush(Runtime& rt, Cpu& cpu) {  // (HBITMAP)
    const PascalArgs a(cpu, {2});
    HBITMAP bmp = static_cast<HBITMAP>(rt.Graphics().HostObject(a.Word(0), Gdi::Kind::Bitmap));
    Return(cpu, a, bmp ? rt.Graphics().Own(CreatePatternBrush(bmp), Gdi::Kind::Brush) : 0);
}

// LOGBRUSH (Win16, 8 bytes): style, COLORREF, hatch.
void Api_CreateBrushIndirect(Runtime& rt, Cpu& cpu) {  // (const LOGBRUSH FAR*)
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    const Memory& mem = rt.Mem();
    LOGBRUSH lb{};
    lb.lbStyle = mem.Read16(p.sel, p.off);
    lb.lbColor = mem.Read16(p.sel, uint16_t(p.off + 2)) | (uint32_t(mem.Read16(p.sel, uint16_t(p.off + 4))) << 16);
    lb.lbHatch = int16_t(mem.Read16(p.sel, uint16_t(p.off + 6)));
    if (lb.lbStyle == BS_PATTERN) {  // lbHatch is a 16-bit bitmap handle
        lb.lbHatch = reinterpret_cast<ULONG_PTR>(rt.Graphics().HostObject(uint16_t(lb.lbHatch), Gdi::Kind::Bitmap));
    } else if (lb.lbStyle == BS_DIBPATTERN) {
        rt.Note("dibpattern", "DIB pattern brushes aren't supported yet (a solid brush is used)");
        lb.lbStyle = BS_SOLID;
    }
    Return(cpu, a, rt.Graphics().Own(CreateBrushIndirect(&lb), Gdi::Kind::Brush));
}

// LOGPEN (Win16, 10 bytes): style, POINT width, COLORREF.
void Api_CreatePenIndirect(Runtime& rt, Cpu& cpu) {  // (const LOGPEN FAR*)
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    const Memory& mem = rt.Mem();
    const int style = mem.Read16(p.sel, p.off), width = int16_t(mem.Read16(p.sel, uint16_t(p.off + 2)));
    const uint32_t color = mem.Read16(p.sel, uint16_t(p.off + 6)) | (uint32_t(mem.Read16(p.sel, uint16_t(p.off + 8))) << 16);
    Return(cpu, a, rt.Graphics().Own(CreatePen(style, width, color), Gdi::Kind::Pen));
}

// --- DC state ------------------------------------------------------------------------------------

template <typename F>
void DcCall(Runtime& rt, Cpu& cpu, F f) {  // (HDC, WORD) -> WORD
    const PascalArgs a(cpu, {2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(f(dc, a.Int(1))) : 0);
}

void Api_SetROP2(Runtime& rt, Cpu& cpu) { DcCall(rt, cpu, [](HDC dc, int v) { return SetROP2(dc, v); }); }
void Api_SetStretchBltMode(Runtime& rt, Cpu& cpu) {
    DcCall(rt, cpu, [](HDC dc, int v) { return SetStretchBltMode(dc, v); });
}
void Api_SetPolyFillMode(Runtime& rt, Cpu& cpu) {
    DcCall(rt, cpu, [](HDC dc, int v) { return SetPolyFillMode(dc, v); });
}
void Api_SetMapMode(Runtime& rt, Cpu& cpu) { DcCall(rt, cpu, [](HDC dc, int v) { return SetMapMode(dc, v); }); }

void Api_GetMapMode(Runtime& rt, Cpu& cpu) {  // (HDC)
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(GetMapMode(dc)) : 0);
}

// SetWindowOrg/Ext, SetViewportOrg/Ext: (HDC, x, y) -> previous as DWORD.
template <typename F>
void OrgExt(Runtime& rt, Cpu& cpu, F f) {
    const PascalArgs a(cpu, {2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    POINT old{};
    const bool ok = dc && f(dc, a.Int(1), a.Int(2), &old);
    Return(cpu, a, ok ? MakeLong(old.x, old.y) : 0);
}

void Api_SetWindowOrg(Runtime& rt, Cpu& cpu) {
    OrgExt(rt, cpu, [](HDC dc, int x, int y, POINT* o) { return SetWindowOrgEx(dc, x, y, o) != FALSE; });
}
void Api_SetViewportOrg(Runtime& rt, Cpu& cpu) {
    OrgExt(rt, cpu, [](HDC dc, int x, int y, POINT* o) { return SetViewportOrgEx(dc, x, y, o) != FALSE; });
}
void Api_SetWindowExt(Runtime& rt, Cpu& cpu) {
    OrgExt(rt, cpu, [](HDC dc, int x, int y, POINT* o) {
        SIZE s{};
        const bool ok = SetWindowExtEx(dc, x, y, &s) != FALSE;
        *o = {s.cx, s.cy};
        return ok;
    });
}
void Api_SetViewportExt(Runtime& rt, Cpu& cpu) {
    OrgExt(rt, cpu, [](HDC dc, int x, int y, POINT* o) {
        SIZE s{};
        const bool ok = SetViewportExtEx(dc, x, y, &s) != FALSE;
        *o = {s.cx, s.cy};
        return ok;
    });
}

void Api_GetBkColor(Runtime& rt, Cpu& cpu) {  // (HDC) -> COLORREF
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? GetBkColor(dc) : 0);
}

void Api_GetBkMode(Runtime& rt, Cpu& cpu) {  // (HDC) -> mode
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(GetBkMode(dc)) : 0);
}

void Api_GetNearestColor(Runtime&, Cpu& cpu) {  // (HDC, COLORREF) -> COLORREF: true colour
    const PascalArgs a(cpu, {2, 4});
    Return(cpu, a, a.Long(1) & 0x00FFFFFF);
}

void Api_SaveDC(Runtime& rt, Cpu& cpu) {  // (HDC) -> level
    const PascalArgs a(cpu, {2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(SaveDC(dc)) : 0);
}

void Api_RestoreDC(Runtime& rt, Cpu& cpu) {  // (HDC, int level) -> BOOL
    const PascalArgs a(cpu, {2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc && RestoreDC(dc, a.Int(1)) ? 1 : 0);
}

void Api_IntersectClipRect(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b) -> region type
    const PascalArgs a(cpu, {2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(IntersectClipRect(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4))) : 0);
}

void Api_ExcludeClipRect(Runtime& rt, Cpu& cpu) {  // (HDC, l, t, r, b) -> region type
    const PascalArgs a(cpu, {2, 2, 2, 2, 2});
    HDC dc = Dc(rt, a.Word(0));
    Return(cpu, a, dc ? uint16_t(ExcludeClipRect(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4))) : 0);
}

void Api_GetClipBox(Runtime& rt, Cpu& cpu) {  // (HDC, RECT FAR*) -> region type
    const PascalArgs a(cpu, {2, 4});
    HDC dc = Dc(rt, a.Word(0));
    RECT r{};
    const int type = dc ? GetClipBox(dc, &r) : 0;
    if (dc) WriteRect(rt.Mem(), a.Ptr(1), r);
    Return(cpu, a, uint16_t(type));
}

// What a Windows 3.1 true-colour display driver at 640x480 reports. There's
// no palette (RC_PALETTE clear, NUMCOLORS -1): palette support comes later.
void Api_GetDeviceCaps(Runtime&, Cpu& cpu) {  // (HDC, index) -> int
    const PascalArgs a(cpu, {2, 2});
    int v = 0;
    switch (a.Int(1)) {
    case 0: v = 0x030A; break;           // DRIVERVERSION
    case 2: v = 1; break;                // TECHNOLOGY: DT_RASDISPLAY
    case 4: v = 208; break;              // HORZSIZE (mm)
    case 6: v = 156; break;              // VERTSIZE
    case 8: v = kScreenWidth; break;     // HORZRES
    case 10: v = kScreenHeight; break;   // VERTRES
    case 12: v = 24; break;              // BITSPIXEL
    case 14: v = 1; break;               // PLANES
    case 16: case 18: v = -1; break;     // NUMBRUSHES, NUMPENS
    case 22: v = 0; break;               // NUMFONTS
    case 24: v = -1; break;              // NUMCOLORS: more than a palette's worth
    case 26: v = 0x100; break;           // PDEVICESIZE
    case 28: v = 0xFF; break;            // CURVECAPS
    case 30: v = 0xFE; break;            // LINECAPS
    case 32: v = 0xFF; break;            // POLYGONALCAPS
    case 34: v = 0x0004; break;          // TEXTCAPS: TC_CP_STROKE
    case 36: v = 1; break;               // CLIPCAPS: CP_RECTANGLE
    case 38: v = 0x2E99; break;          // RASTERCAPS: BITBLT, BITMAP64, GDI20_OUTPUT, DI_BITMAP,
                                         //   DIBTODEV, BIGFONT, STRETCHBLT, STRETCHDIB (no PALETTE)
    case 40: case 42: v = 36; break;     // ASPECTX, ASPECTY
    case 44: v = 51; break;              // ASPECTXY
    case 88: case 90: v = 96; break;     // LOGPIXELSX, LOGPIXELSY
    case 104: case 106: v = 0; break;    // SIZEPALETTE, NUMRESERVED
    case 108: v = 24; break;             // COLORRES
    default: v = 0; break;
    }
    Return(cpu, a, uint16_t(int16_t(v)));
}

// --- GetObject and bitmap bits --------------------------------------------------------------

void Api_GetObject(Runtime& rt, Cpu& cpu) {  // (HGDIOBJ, int size, void FAR*) -> bytes copied
    const PascalArgs a(cpu, {2, 2, 4});
    Gdi::Kind kind{};
    const uint16_t h = a.Word(0);
    const FarPtr p = a.Ptr(2);
    const int size = a.Int(1);
    uint8_t buf[64] = {};
    int bytes = 0;
    auto put16 = [&](int at, int v) {
        buf[at] = uint8_t(v);
        buf[at + 1] = uint8_t(v >> 8);
    };
    auto put32 = [&](int at, uint32_t v) {
        put16(at, int(v & 0xFFFF));
        put16(at + 2, int(v >> 16));
    };
    if (rt.Graphics().KindOf(h, kind)) {
        void* host = rt.Graphics().HostObject(h, kind);
        switch (kind) {
        case Gdi::Kind::Bitmap: {  // BITMAP: type, width, height, widthBytes, planes, bitsPixel, bits
            BITMAP bm{};
            if (GetObjectW(host, sizeof(bm), &bm)) {
                put16(0, 0);
                put16(2, bm.bmWidth);
                put16(4, bm.bmHeight);
                put16(6, ((bm.bmWidth * bm.bmBitsPixel + 15) / 16) * 2);  // Win16 rows are WORD aligned
                buf[8] = uint8_t(bm.bmPlanes);
                buf[9] = uint8_t(bm.bmBitsPixel);
                put32(10, 0);
                bytes = 14;
            }
            break;
        }
        case Gdi::Kind::Brush: {  // LOGBRUSH: style, colour, hatch
            LOGBRUSH lb{};
            if (GetObjectW(host, sizeof(lb), &lb)) {
                put16(0, int(lb.lbStyle));
                put32(2, lb.lbColor);
                put16(6, int(lb.lbHatch));
                bytes = 8;
            }
            break;
        }
        case Gdi::Kind::Pen: {  // LOGPEN: style, width (POINT), colour
            LOGPEN lp{};
            if (GetObjectW(host, sizeof(lp), &lp)) {
                put16(0, int(lp.lopnStyle));
                put16(2, lp.lopnWidth.x);
                put16(4, lp.lopnWidth.y);
                put32(6, lp.lopnColor);
                bytes = 10;
            }
            break;
        }
        case Gdi::Kind::Font: {
            LOGFONTW lf{};
            if (GetObjectW(host, sizeof(lf), &lf)) {
                const uint16_t n = uint16_t(std::clamp(size, 0, 50));
                if (!p.IsNull()) WriteLogFont(rt.Mem(), p, lf, n);
                Return(cpu, a, n);
                return;
            }
            break;
        }
        case Gdi::Kind::Palette: {
            WORD entries = 0;
            if (GetObjectW(host, sizeof(entries), &entries)) {
                put16(0, entries);
                bytes = 2;
            }
            break;
        }
        default: break;
        }
    }
    const int n = std::clamp(size, 0, bytes);
    for (int i = 0; i < n && !p.IsNull(); ++i) rt.Mem().Write8(p.sel, uint16_t(p.off + i), buf[i]);
    Return(cpu, a, uint16_t(n));
}

void Api_GetBitmapBits(Runtime& rt, Cpu& cpu) {  // (HBITMAP, LONG count, void FAR*) -> bytes
    const PascalArgs a(cpu, {2, 4, 4});
    HBITMAP bmp = static_cast<HBITMAP>(rt.Graphics().HostObject(a.Word(0), Gdi::Kind::Bitmap));
    const FarPtr p = a.Ptr(2);
    const uint32_t count = std::min<uint32_t>(a.Long(1), 0xFFFF);
    LONG got = 0;
    if (bmp && !p.IsNull() && count) {
        rt.Mem().Translate(p.sel, p.off, count, Access::Write);
        got = GetBitmapBits(bmp, LONG(count), rt.Mem().SegmentData(p.sel) + p.off);
    }
    Return(cpu, a, uint32_t(got));
}

void Api_SetBitmapBits(Runtime& rt, Cpu& cpu) {  // (HBITMAP, DWORD count, const void FAR*) -> bytes
    const PascalArgs a(cpu, {2, 4, 4});
    HBITMAP bmp = static_cast<HBITMAP>(rt.Graphics().HostObject(a.Word(0), Gdi::Kind::Bitmap));
    const FarPtr p = a.Ptr(2);
    const uint32_t count = std::min<uint32_t>(a.Long(1), 0xFFFF);
    LONG set = 0;
    if (bmp && !p.IsNull() && count) {
        rt.Mem().Translate(p.sel, p.off, count, Access::Read);
        set = SetBitmapBits(bmp, DWORD(count), rt.Mem().SegmentData(p.sel) + p.off);
    }
    Return(cpu, a, uint32_t(set));
}

// --- Device-independent bitmaps ----------------------------------------------------------------

// A BITMAPINFO (header + colour table) and bits, copied out of 16-bit memory
// with bounds checks; bits over a 64 KB segment (huge pointers) aren't supported.
struct Dib {
    std::vector<uint32_t> info;  // BITMAPINFO, DWORD aligned
    std::vector<uint8_t> bits;
    bool ok = false;
    BITMAPINFO* Info() { return reinterpret_cast<BITMAPINFO*>(info.data()); }
    const BITMAPINFOHEADER& Header() { return *reinterpret_cast<BITMAPINFOHEADER*>(info.data()); }
};

bool ReadDibHeader(Runtime& rt, FarPtr p, uint16_t usage, Dib& dib, uint32_t& rowBytes, int& height) {
    if (p.IsNull()) return false;
    if (usage == DIB_PAL_COLORS) {
        rt.Note("dibpal", "DIBs with palette indices (DIB_PAL_COLORS) aren't supported yet (palettes come later)");
        return false;
    }
    Memory& mem = rt.Mem();
    const uint32_t headerSize = mem.Read16(p.sel, p.off) | (uint32_t(mem.Read16(p.sel, uint16_t(p.off + 2))) << 16);
    if (headerSize != sizeof(BITMAPINFOHEADER)) {
        rt.Note("dibheader", "only BITMAPINFOHEADER DIBs are supported in DIB functions so far");
        return false;
    }
    BITMAPINFOHEADER h{};
    mem.Translate(p.sel, p.off, sizeof(h), Access::Read);
    std::memcpy(&h, mem.SegmentData(p.sel) + p.off, sizeof(h));
    const int bpp = h.biBitCount;
    if (h.biWidth <= 0 || h.biHeight == 0 || h.biPlanes != 1 ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return false;
    uint32_t colors = h.biClrUsed ? h.biClrUsed : (bpp <= 8 ? 1u << bpp : 0);
    if (h.biCompression == BI_BITFIELDS) colors = 3;
    if (colors > 256) return false;
    const uint32_t total = sizeof(h) + colors * 4;
    mem.Translate(p.sel, p.off, total, Access::Read);
    dib.info.assign((total + 3) / 4 + 1, 0);
    std::memcpy(dib.info.data(), mem.SegmentData(p.sel) + p.off, total);
    rowBytes = ((uint32_t(h.biWidth) * uint32_t(bpp) + 31) / 32) * 4;
    height = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    return true;
}

bool ReadDibBits(Runtime& rt, FarPtr bits, uint32_t bytes, Dib& dib) {
    if (bits.IsNull() || bytes == 0) return false;
    Memory& mem = rt.Mem();
    if (uint32_t(bits.off) + bytes > mem.SegmentSize(bits.sel)) {
        rt.Note("hugebits", "DIB bits over 64 KB (huge pointers) aren't supported yet");
        return false;
    }
    mem.Translate(bits.sel, bits.off, bytes, Access::Read);
    const uint8_t* src = mem.SegmentData(bits.sel) + bits.off;
    dib.bits.assign(src, src + bytes);
    return true;
}

uint32_t CompressedOr(const Dib& dib, uint32_t uncompressed) {
    const BITMAPINFOHEADER& h = *reinterpret_cast<const BITMAPINFOHEADER*>(dib.info.data());
    return (h.biCompression == BI_RLE8 || h.biCompression == BI_RLE4) ? h.biSizeImage : uncompressed;
}

// (HDC, xDest, yDest, cx, cy, xSrc, ySrc, startScan, scanLines, bits, BITMAPINFO FAR*, usage) -> lines
void Api_SetDIBitsToDevice(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    Dib dib;
    uint32_t row = 0;
    int height = 0;
    int lines = 0;
    if (dc && ReadDibHeader(rt, a.Ptr(10), a.Word(11), dib, row, height) &&
        ReadDibBits(rt, a.Ptr(9), CompressedOr(dib, row * a.Word(8)), dib)) {
        lines = SetDIBitsToDevice(dc, a.Int(1), a.Int(2), DWORD(a.Word(3)), DWORD(a.Word(4)), a.Int(5), a.Int(6),
                                  a.Word(7), a.Word(8), dib.bits.data(), dib.Info(), DIB_RGB_COLORS);
    }
    Return(cpu, a, uint16_t(lines));
}

// (HDC, xd, yd, wd, hd, xs, ys, ws, hs, bits, BITMAPINFO FAR*, usage, rop) -> lines
void Api_StretchDIBits(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2, 4});
    HDC dc = Dc(rt, a.Word(0));
    Dib dib;
    uint32_t row = 0;
    int height = 0;
    int lines = 0;
    if (dc && ReadDibHeader(rt, a.Ptr(10), a.Word(11), dib, row, height) &&
        ReadDibBits(rt, a.Ptr(9), CompressedOr(dib, row * uint32_t(height)), dib)) {
        lines = StretchDIBits(dc, a.Int(1), a.Int(2), a.Int(3), a.Int(4), a.Int(5), a.Int(6), a.Int(7), a.Int(8),
                              dib.bits.data(), dib.Info(), DIB_RGB_COLORS, a.Long(12));
    }
    Return(cpu, a, uint16_t(lines));
}

// (HDC, HBITMAP, startScan, scanLines, bits, BITMAPINFO FAR*, usage) -> lines
void Api_SetDIBits(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 2, 2, 4, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    HBITMAP bmp = static_cast<HBITMAP>(rt.Graphics().HostObject(a.Word(1), Gdi::Kind::Bitmap));
    Dib dib;
    uint32_t row = 0;
    int height = 0;
    int lines = 0;
    if (dc && bmp && ReadDibHeader(rt, a.Ptr(5), a.Word(6), dib, row, height) &&
        ReadDibBits(rt, a.Ptr(4), CompressedOr(dib, row * a.Word(3)), dib)) {
        lines = SetDIBits(dc, bmp, a.Word(2), a.Word(3), dib.bits.data(), dib.Info(), DIB_RGB_COLORS);
    }
    Return(cpu, a, uint16_t(lines));
}

// (HDC, HBITMAP, startScan, scanLines, bits or NULL, BITMAPINFO FAR*, usage) -> lines
void Api_GetDIBits(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 2, 2, 2, 4, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    HBITMAP bmp = static_cast<HBITMAP>(rt.Graphics().HostObject(a.Word(1), Gdi::Kind::Bitmap));
    const FarPtr bits = a.Ptr(4), bmiPtr = a.Ptr(5);
    Memory& mem = rt.Mem();
    int lines = 0;
    if (dc && bmp && !bmiPtr.IsNull() && a.Word(6) == DIB_RGB_COLORS) {
        // Room for the header and a 256-entry colour table (or bitfields).
        std::vector<uint32_t> info((sizeof(BITMAPINFOHEADER) + 256 * 4) / 4, 0);
        BITMAPINFOHEADER& h = *reinterpret_cast<BITMAPINFOHEADER*>(info.data());
        mem.Translate(bmiPtr.sel, bmiPtr.off, sizeof(h), Access::Write);
        std::memcpy(&h, mem.SegmentData(bmiPtr.sel) + bmiPtr.off, sizeof(h));
        h.biSize = sizeof(h);
        BITMAPINFO* bmi = reinterpret_cast<BITMAPINFO*>(info.data());
        if (bits.IsNull()) {
            lines = GetDIBits(dc, bmp, a.Word(2), a.Word(3), nullptr, bmi, DIB_RGB_COLORS);  // fills the header
        } else if (h.biBitCount) {
            const uint32_t row = ((uint32_t(std::abs(h.biWidth)) * h.biBitCount + 31) / 32) * 4;
            const uint32_t bytes = row * a.Word(3);
            if (bytes && uint32_t(bits.off) + bytes <= mem.SegmentSize(bits.sel)) {
                std::vector<uint8_t> buffer(bytes);
                lines = GetDIBits(dc, bmp, a.Word(2), a.Word(3), buffer.data(), bmi, DIB_RGB_COLORS);
                mem.Translate(bits.sel, bits.off, bytes, Access::Write);
                std::memcpy(mem.SegmentData(bits.sel) + bits.off, buffer.data(), bytes);
            } else {
                rt.Note("hugebits", "DIB bits over 64 KB (huge pointers) aren't supported yet");
            }
        }
        // Header and colour table back to the program (as much as fits the segment).
        const uint32_t colors = h.biBitCount && h.biBitCount <= 8 ? (h.biClrUsed ? h.biClrUsed : 1u << h.biBitCount)
                                                                 : (h.biCompression == BI_BITFIELDS ? 3u : 0u);
        const uint32_t out = std::min<uint32_t>(sizeof(h) + colors * 4, mem.SegmentSize(bmiPtr.sel) - bmiPtr.off);
        mem.Translate(bmiPtr.sel, bmiPtr.off, out, Access::Write);
        std::memcpy(mem.SegmentData(bmiPtr.sel) + bmiPtr.off, info.data(), out);
    }
    Return(cpu, a, uint16_t(lines));
}

// (HDC, BITMAPINFOHEADER FAR*, DWORD init, bits, BITMAPINFO FAR*, usage) -> HBITMAP
void Api_CreateDIBitmap(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {2, 4, 4, 4, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    const FarPtr hp = a.Ptr(1);
    uint16_t result = 0;
    if (dc && !hp.IsNull()) {
        BITMAPINFOHEADER h{};
        rt.Mem().Translate(hp.sel, hp.off, sizeof(h), Access::Read);
        std::memcpy(&h, rt.Mem().SegmentData(hp.sel) + hp.off, sizeof(h));
        HBITMAP bmp = nullptr;
        if (a.Long(2) & CBM_INIT) {
            Dib dib;
            uint32_t row = 0;
            int height = 0;
            if (ReadDibHeader(rt, a.Ptr(4), a.Word(5), dib, row, height) &&
                ReadDibBits(rt, a.Ptr(3), CompressedOr(dib, row * uint32_t(height)), dib)) {
                bmp = CreateDIBitmap(dc, &h, CBM_INIT, dib.bits.data(), dib.Info(), DIB_RGB_COLORS);
            }
        } else {
            bmp = CreateCompatibleBitmap(dc, h.biWidth, std::abs(h.biHeight));
        }
        result = rt.Graphics().Own(bmp, Gdi::Kind::Bitmap);
    }
    Return(cpu, a, result);
}

// --- USER drawing ------------------------------------------------------------------------------

void Api_DrawText(Runtime& rt, Cpu& cpu) {  // (HDC, LPCSTR, int count, RECT FAR*, format) -> height
    const PascalArgs a(cpu, {2, 4, 2, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    int height = 0;
    const FarPtr rp = a.Ptr(3);
    if (dc && !rp.IsNull()) {
        std::wstring text = ReadText(rt, a.Ptr(1), a.Int(2));
        RECT r = ReadRect(rt.Mem(), rp);
        const UINT format = a.Word(4);
        height = DrawTextW(dc, text.data(), int(text.size()), &r, format);
        if (format & DT_CALCRECT) WriteRect(rt.Mem(), rp, r);
    }
    Return(cpu, a, uint16_t(height));
}

void Api_FrameRect(Runtime& rt, Cpu& cpu) {  // (HDC, const RECT FAR*, HBRUSH)
    const PascalArgs a(cpu, {2, 4, 2});
    HDC dc = Dc(rt, a.Word(0));
    HBRUSH brush = static_cast<HBRUSH>(rt.Graphics().HostBrush(a.Word(2)));
    const RECT r = ReadRect(rt.Mem(), a.Ptr(1));
    Return(cpu, a, dc && brush && FrameRect(dc, &r, brush) ? 1 : 0);
}

void Api_InvertRect(Runtime& rt, Cpu& cpu) {  // (HDC, const RECT FAR*)
    const PascalArgs a(cpu, {2, 4});
    HDC dc = Dc(rt, a.Word(0));
    const RECT r = ReadRect(rt.Mem(), a.Ptr(1));
    if (dc) InvertRect(dc, &r);
    cpu.ReturnFar(a.Bytes());
}

}  // namespace

std::vector<ApiFunction> GdiDrawApi() {
    return {
        {3, "SETMAPMODE", Api_SetMapMode},
        {4, "SETROP2", Api_SetROP2},
        {6, "SETPOLYFILLMODE", Api_SetPolyFillMode},
        {7, "SETSTRETCHBLTMODE", Api_SetStretchBltMode},
        {8, "SETTEXTCHARACTEREXTRA", Api_SetTextCharacterExtra},
        {11, "SETWINDOWORG", Api_SetWindowOrg},
        {12, "SETWINDOWEXT", Api_SetWindowExt},
        {13, "SETVIEWPORTORG", Api_SetViewportOrg},
        {14, "SETVIEWPORTEXT", Api_SetViewportExt},
        {19, "LINETO", Api_LineTo},
        {20, "MOVETO", Api_MoveTo},
        {21, "EXCLUDECLIPRECT", Api_ExcludeClipRect},
        {22, "INTERSECTCLIPRECT", Api_IntersectClipRect},
        {23, "ARC", Api_Arc},
        {24, "ELLIPSE", Api_Ellipse},
        {26, "PIE", Api_Pie},
        {28, "ROUNDRECT", Api_RoundRect},
        {30, "SAVEDC", Api_SaveDC},
        {36, "POLYGON", Api_Polygon},
        {37, "POLYLINE", Api_Polyline},
        {39, "RESTOREDC", Api_RestoreDC},
        {50, "CREATEBRUSHINDIRECT", Api_CreateBrushIndirect},
        {56, "CREATEFONT", Api_CreateFont},
        {57, "CREATEFONTINDIRECT", Api_CreateFontIndirect},
        {58, "CREATEHATCHBRUSH", Api_CreateHatchBrush},
        {60, "CREATEPATTERNBRUSH", Api_CreatePatternBrush},
        {62, "CREATEPENINDIRECT", Api_CreatePenIndirect},
        {74, "GETBITMAPBITS", Api_GetBitmapBits},
        {75, "GETBKCOLOR", Api_GetBkColor},
        {76, "GETBKMODE", Api_GetBkMode},
        {77, "GETCLIPBOX", Api_GetClipBox},
        {78, "GETCURRENTPOSITION", Api_GetCurrentPosition},
        {80, "GETDEVICECAPS", Api_GetDeviceCaps},
        {81, "GETMAPMODE", Api_GetMapMode},
        {82, "GETOBJECT", Api_GetObject},
        {91, "GETTEXTEXTENT", Api_GetTextExtent},
        {92, "GETTEXTFACE", Api_GetTextFace},
        {93, "GETTEXTMETRICS", Api_GetTextMetrics},
        {106, "SETBITMAPBITS", Api_SetBitmapBits},
        {154, "GETNEARESTCOLOR", Api_GetNearestColor},
        {345, "GETTEXTALIGN", Api_GetTextAlign},
        {346, "SETTEXTALIGN", Api_SetTextAlign},
        {351, "EXTTEXTOUT", Api_ExtTextOut},
        {439, "STRETCHDIBITS", Api_StretchDIBits},
        {440, "SETDIBITS", Api_SetDIBits},
        {441, "GETDIBITS", Api_GetDIBits},
        {442, "CREATEDIBITMAP", Api_CreateDIBitmap},
        {443, "SETDIBITSTODEVICE", Api_SetDIBitsToDevice},
        {471, "GETTEXTEXTENTPOINT", Api_GetTextExtentPoint},
        {483, "MOVETOEX", Api_MoveToEx},
    };
}

std::vector<ApiFunction> UserDrawApi() {
    return {
        {82, "INVERTRECT", Api_InvertRect},
        {83, "FRAMERECT", Api_FrameRect},
        {85, "DRAWTEXT", Api_DrawText},
    };
}

}  // namespace retro::win16
