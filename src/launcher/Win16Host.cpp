#include "Win16Host.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "retro/DisplayMath.h"
#include "retro/FramePacing.h"
#include "retro/PathUtil.h"
#include "win16/Runtime.h"

namespace retro {
namespace {

// Real windows for a Win16 task's top-level windows, laid out with the same
// DisplayMath the display sandbox uses: a window covering the 16-bit screen
// becomes borderless fullscreen with the 640x480 screen integer-scaled in the
// middle of the monitor; any other window becomes a captioned window whose
// client area is the 16-bit window integer-scaled. Input comes back in the
// 16-bit window's coordinates.
class Win32WindowHost : public win16::WindowHost {
public:
    explicit Win32WindowHost(bool hidden) : hidden_(hidden) {}

    ~Win32WindowHost() override {
        for (const Entry& e : windows_) DestroyWindow(e.hwnd);
        if (timer_) CloseHandle(timer_);
    }

    uint64_t Create(const WindowInfo& info) override {
        RegisterClassOnce();
        const DPI_AWARENESS_CONTEXT previous =
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const Rect monitor{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom};
        const Rect work{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom};

        const Size virt = info.fullscreen ? Size{win16::kScreenWidth, win16::kScreenHeight}
                                          : Size{info.width, info.height};
        DWORD style;
        WindowLayout layout;
        if (info.fullscreen) {
            style = WS_POPUP;
            layout = PlanFullscreenLayout(virt, monitor, {});
        } else {
            style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT frame{0, 0, 0, 0};
            AdjustWindowRectExForDpi(&frame, style, FALSE, 0, GetDpiForSystem());
            layout = PlanWindowedLayout(virt, work, {-frame.left, -frame.top, frame.right, frame.bottom}, {});
        }

        const std::wstring title = Widen(info.title);
        HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, layout.window.left,
                                    layout.window.top, layout.window.Width(), layout.window.Height(),
                                    nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetThreadDpiAwarenessContext(previous);
        if (!hwnd) return 0;

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        const Rect vp{layout.viewport.left - layout.client.left, layout.viewport.top - layout.client.top,
                      layout.viewport.right - layout.client.left,
                      layout.viewport.bottom - layout.client.top};
        windows_.push_back({hwnd, info.hwnd16, ViewportMap(vp, virt), info.fullscreen});

        const std::string scale = layout.integerScale ? "x" + std::to_string(layout.integerScale)
                                                      : std::string("fractional");
        std::printf("[win16] host window for HWND16 %04X \"%s\": %dx%d -> %dx%d (%s), %s%s\n",
                    info.hwnd16, info.title.c_str(), virt.w, virt.h, layout.viewport.Width(),
                    layout.viewport.Height(), scale.c_str(),
                    info.fullscreen ? "borderless fullscreen" : "windowed", hidden_ ? ", hidden" : "");
        std::fflush(stdout);
        return reinterpret_cast<uint64_t>(hwnd);
    }

    void Show(uint64_t window, bool show) override {
        Entry* e = Find(reinterpret_cast<HWND>(window));
        if (!e || e->visible == show) return;
        e->visible = show;
        if (!hidden_) ShowWindow(e->hwnd, show ? SW_SHOW : SW_HIDE);
    }

    // A complete frame from the window's back buffer: one nearest-neighbour
    // blit into the integer-scaled viewport, composited by DWM (no tearing).
    void Present(uint64_t window, const uint32_t* pixels, int width, int height) override {
        Entry* e = Find(reinterpret_cast<HWND>(window));
        if (!e) return;
        e->frame.assign(pixels, pixels + size_t(width) * height);
        e->frameWidth = width;
        e->frameHeight = height;
        if (HDC dc = GetDC(e->hwnd)) {
            Draw(*e, dc);
            ReleaseDC(e->hwnd, dc);
        }
    }

    void Destroy(uint64_t window) override {
        const HWND hwnd = reinterpret_cast<HWND>(window);
        for (auto it = windows_.begin(); it != windows_.end(); ++it) {
            if (it->hwnd == hwnd) {
                windows_.erase(it);
                break;
            }
        }
        DestroyWindow(hwnd);
    }

    bool Pump(const Deliver& deliver, int64_t until) override {
        MSG msg;
        if (until != kPoll && !PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
            if (until == kForever) {
                // Only a user can produce input: with no visible window (or
                // hidden mode) waiting would hang forever.
                if (hidden_ || !AnyVisible()) return false;
                WaitMessage();
            } else {
                WaitForInputUntil(until);
            }
        }
        deliver_ = &deliver;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        deliver_ = nullptr;
        return true;
    }

private:
    static constexpr const wchar_t* kClassName = L"RetroWin16Window";

    struct Entry {
        HWND hwnd;
        uint16_t hwnd16;
        ViewportMap map;  // client coordinates <-> 16-bit window coordinates
        bool fullscreen;
        bool visible = false;
        std::vector<uint32_t> frame;  // last presented frame (for WM_PAINT)
        int frameWidth = 0, frameHeight = 0;
    };

    // Sleeps until input arrives or the QPC deadline passes (a 16-bit timer is
    // due). A high-resolution waitable timer keeps that accurate: the timeout
    // of MsgWaitForMultipleObjects alone is only as good as the system timer
    // resolution (15.6 ms by default).
    void WaitForInputUntil(int64_t until) {
        const int64_t remaining = until - QpcNow();
        if (remaining <= 0) return;
        if (!timer_) {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
            if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        LARGE_INTEGER due;  // relative, in 100 ns units
        due.QuadPart = -std::max<int64_t>(1, remaining * 10'000'000 / QpcFrequency());
        if (timer_ && SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            MsgWaitForMultipleObjectsEx(1, &timer_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            CancelWaitableTimer(timer_);  // input may have come first
        } else {
            const DWORD ms = DWORD((remaining * 1000 + QpcFrequency() - 1) / QpcFrequency());
            MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    static void Draw(const Entry& e, HDC dc) {
        if (e.frame.empty()) return;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = e.frameWidth;
        bmi.bmiHeader.biHeight = -e.frameHeight;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        const Rect& vp = e.map.RealViewport();
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, vp.left, vp.top, vp.Width(), vp.Height(), 0, 0, e.frameWidth,
                      e.frameHeight, e.frame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    static std::wstring Widen(const std::string& s) {  // 16-bit strings are ANSI
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    static void RegisterClassOnce() {
        static const ATOM atom = [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = kClassName;
            return RegisterClassExW(&wc);
        }();
        (void)atom;
    }

    Entry* Find(HWND hwnd) {
        for (Entry& e : windows_) {
            if (e.hwnd == hwnd) return &e;
        }
        return nullptr;
    }

    bool AnyVisible() const {
        for (const Entry& e : windows_) {
            if (e.visible) return true;
        }
        return false;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32WindowHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        Entry* e = self ? self->Find(hwnd) : nullptr;
        if (e && msg == WM_PAINT) {  // uncovered: show the last frame again (bars stay black)
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Draw(*e, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (!e || !self->deliver_) return DefWindowProcW(hwnd, msg, wp, lp);
        const Deliver& deliver = *self->deliver_;

        switch (msg) {
        case WM_CLOSE:  // the user closed it: the 16-bit program decides what happens
            deliver(e->hwnd16, win16::wm::Close, 0, 0);
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
            deliver(e->hwnd16, uint16_t(msg), uint16_t(wp), uint32_t(lp));
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            const Point p = e->map.ToVirtual({short(LOWORD(lp)), short(HIWORD(lp))});
            deliver(e->hwnd16, uint16_t(msg), uint16_t(wp),
                    uint32_t(uint16_t(p.x)) | (uint32_t(uint16_t(p.y)) << 16));
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    bool hidden_;
    HANDLE timer_ = nullptr;  // for WaitForInputUntil
    const Deliver* deliver_ = nullptr;
    std::vector<Entry> windows_;
};

}  // namespace

int RunWin16Program(const std::filesystem::path& exe, const std::string& commandLine,
                    const Win16Options& options) {
    std::ifstream in(exe, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (file.empty()) {
        std::fputs("error: cannot read the program\n", stderr);
        return kWin16Stopped;
    }

    Win32WindowHost host(options.hidden);
    win16::Runtime runtime;
    runtime.SetWindowHost(&host);
    runtime.SetFrameCap(options.fpsCap);
    runtime.SetTrace(options.trace);
    runtime.SetStubMissing(options.stubMissing);
    runtime.SetExactTimers(options.exactTimers);
    runtime.SetProgram(exe);
    runtime.SetOutput([](const std::string& line) {
        std::printf("[win16] %s\n", line.c_str());
        std::fflush(stdout);
    });

    std::string error;
    if (!runtime.Load(file, commandLine, error)) {
        std::fprintf(stderr, "error: Win16 load failed: %s\n", error.c_str());
        return kWin16Stopped;
    }
    const win16::NeImage& image = runtime.Image();
    std::printf("Win16 task  : %s, %zu segments, %zu resources, entry %u:%04X\n",
                image.moduleName.c_str(), image.segments.size(), image.resources.size(),
                image.entrySegment, image.entryIp);
    std::fflush(stdout);

    const win16::TaskExit result = runtime.Run();
    std::printf("Win16 exit  : %s", win16::ToString(result.kind));
    if (result.kind == win16::TaskExit::Kind::Exited || result.kind == win16::TaskExit::Kind::FatalExit)
        std::printf(", code %u", result.code);
    if (!result.message.empty()) std::printf(" - %s", result.message.c_str());
    std::printf(" (%llu instructions)\n", static_cast<unsigned long long>(result.instructions));

    const win16::Registers& r = result.registers;
    if (result.kind == win16::TaskExit::Kind::Fault) {
        std::printf("              AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X SP=%04X\n"
                    "              CS:IP=%04X:%04X DS=%04X ES=%04X SS=%04X FLAGS=%04X\n",
                    r.r[win16::AX], r.r[win16::BX], r.r[win16::CX], r.r[win16::DX], r.r[win16::SI],
                    r.r[win16::DI], r.r[win16::BP], r.r[win16::SP], r.s[win16::CS], r.ip,
                    r.s[win16::DS], r.s[win16::ES], r.s[win16::SS], r.flags);
    }

    switch (result.kind) {
    case win16::TaskExit::Kind::Exited:
    case win16::TaskExit::Kind::FatalExit:
        return result.code;
    default:
        return kWin16Stopped;
    }
}

}  // namespace retro
