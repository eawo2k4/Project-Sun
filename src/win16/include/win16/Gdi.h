#pragma once

// GDI for a Win16 task, on top of the host's (Win32) GDI.
//
// Every 16-bit GDI handle (HDC16, HBITMAP16, HBRUSH16, HPEN16, ...) wraps a
// host GDI object, with a bidirectional map: SelectObject returns the host's
// previously selected object, which is found (or wrapped on the fly, e.g. a
// new DC's default 1x1 bitmap) as the matching 16-bit handle. Win16 and Win32
// GDI share ROP codes, COLORREFs and bitmap row padding, so calls translate
// almost one to one.
//
// Each top-level window has a backing surface: a 32-bit top-down DIB of the
// window's 16-bit size, with a host memory DC. Window DCs (GetDC,
// BeginPaint) draw into it; releasing one marks the surface dirty.
// PresentPending(), called at the message pump, paces to the frame cap with
// the same scheduler as the display sandbox and hands dirty surfaces to the
// WindowHost, which scales them onto the screen.
//
// Fonts: the stock fonts (SYSTEM_FONT, ANSI_VAR_FONT, ...) are the engine's
// own 96-DPI equivalents of Windows 3.1's, not the host's (which scale with
// the host's DPI), and every DC starts with SYSTEM_FONT selected. Fonts are
// never antialiased, as in Windows 3.1. System colour brushes (COLOR_xxx + 1)
// use the Windows 3.1 default colour scheme.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace retro {
class FrameScheduler;
class PreciseWaiter;
}  // namespace retro

namespace retro::win16 {

struct Point16 {
    int16_t x = 0, y = 0;
};

class Runtime;
struct Rect16;

class Gdi {
public:
    enum class Kind : uint8_t { Dc, Bitmap, Brush, Pen, Font, Palette, Other };

    explicit Gdi(Runtime& rt);
    ~Gdi();
    Gdi(const Gdi&) = delete;
    Gdi& operator=(const Gdi&) = delete;

    // --- Window surfaces ---
    bool CreateSurface(uint16_t hwnd, int width, int height);
    void DestroySurface(uint16_t hwnd);
    // New size, keeping the picture at the top left (MoveWindow). False if a
    // window DC is out (the DC's saved state holds the old bitmap).
    bool ResizeSurface(uint16_t hwnd, int width, int height);
    // Pixels of a window's surface (32-bit BGRA, top-down), for hosts and tests.
    const uint32_t* SurfacePixels(uint16_t hwnd, int& width, int& height) const;

    // --- Device contexts ---
    // DC for a window's surface (hwnd 0: a 640x480 desktop surface). Each call
    // saves the DC state; ReleaseWindowDc restores it and marks the surface dirty.
    // A child window draws on its top-level window's surface: its DC is a handle
    // of its own onto that surface's DC, with the device origin moved to the
    // child's corner and drawing clipped to the child (and its ancestors).
    uint16_t GetWindowDc(uint16_t hwnd);
    // A child window DC's device origin on the surface ({0, 0} for other DCs).
    // Viewport origins and device coordinates are reported relative to it.
    Point16 DeviceOrigin(uint16_t hdc) const;
    bool ReleaseWindowDc(uint16_t hdc);
    uint16_t CreateCompatibleDc(uint16_t hdc);
    bool DeleteDc(uint16_t hdc);
    void ClipTo(uint16_t hdc, const Rect16& r);

    // --- Objects ---
    uint16_t StockObject(int index);
    uint16_t CreateBitmap(int width, int height, int planes, int bitsPerPixel, const void* bits);
    uint16_t CreateCompatibleBitmap(uint16_t hdc, int width, int height);
    // A device-dependent bitmap from a packed DIB (BITMAPINFOHEADER or
    // BITMAPCOREHEADER, colour table, bits), as stored in RT_BITMAP resources.
    // Two-colour DIBs become monochrome bitmaps, like Windows' LoadBitmap.
    // 0 if the DIB is malformed or truncated.
    uint16_t CreateBitmapFromDib(const uint8_t* dib, size_t size);
    uint16_t CreateSolidBrush(uint32_t color);
    uint16_t CreatePen(int style, int width, uint32_t color);
    uint16_t Select(uint16_t hdc, uint16_t object);  // returns the previous object
    bool Delete(uint16_t object);
    // A host object the program now owns (CreateFont, CreateHatchBrush, ...), or 0.
    uint16_t Own(void* host, Kind kind) { return Wrap(host, kind, true, false); }
    bool KindOf(uint16_t handle, Kind& kind) const;

    // Host objects behind 16-bit handles (nullptr if not that kind).
    void* HostDc(uint16_t hdc) const;
    void* HostObject(uint16_t handle, Kind kind) const;
    // A brush handle as a host brush; also accepts COLOR_xxx + 1 (system colours).
    void* HostBrush(uint16_t brush) const;
    uint16_t HandleForHost(void* host) const;
    size_t ObjectCount() const { return objects_.size(); }

    // Fills a rectangle of a DC with a 16-bit brush (FillRect, background erase).
    bool Fill(uint16_t hdc, const Rect16& r, uint16_t brush);

    // --- Presentation ---
    void SetFrameCap(uint32_t fps);
    void PresentPending();
    uint64_t Presents() const { return presents_; }

private:
    struct Object {
        Kind kind = Kind::Other;
        void* host = nullptr;
        bool owned = false;   // we created it (and delete it)
        bool stock = false;
        uint16_t window = 0;  // DCs of window surfaces: the window (0xFFFF: desktop)
    };
    struct Surface {
        void* dc = nullptr;
        void* dib = nullptr;
        void* previousBitmap = nullptr;
        uint32_t* bits = nullptr;
        int width = 0, height = 0;
        uint16_t hdc16 = 0;
        int saved = 0;  // outstanding GetWindowDc calls
        bool dirty = false;
    };

    uint16_t Wrap(void* host, Kind kind, bool owned, bool stock, uint16_t window = 0);
    void* StockFont(int index);        // our 96-DPI stock font, nullptr if not a font index
    void SelectDefaultFont(void* dc);
    void Unwrap(uint16_t handle);
    const Object* Find(uint16_t handle) const;
    Surface* SurfaceFor(uint16_t hwnd);
    struct ChildDc {
        uint16_t top = 0;  // the top-level window whose surface it draws on
        int16_t x = 0, y = 0;
    };
    std::map<uint16_t, ChildDc> childDcs_;  // by HDC
    bool MakeSurface(Surface& s, int width, int height);
    void FreeSurface(Surface& s);

    Runtime& rt_;
    std::map<uint16_t, Object> objects_;
    std::map<void*, uint16_t> byHost_;
    std::map<uint16_t, Surface> surfaces_;  // by hwnd; key 0xFFFF = desktop
    std::map<int, void*> stockFonts_;       // by stock object index
    mutable std::map<int, void*> sysBrushes_;  // COLOR_xxx -> brush in the classic scheme
    uint16_t nextHandle_ = 0x3004;

    std::unique_ptr<FrameScheduler> scheduler_;
    std::unique_ptr<PreciseWaiter> waiter_;
    uint64_t presents_ = 0;
};

}  // namespace retro::win16
