#pragma once

// USER: window classes, windows and the message queue of a Win16 task.
//
// Windows exist on two sides:
//   * the 16-bit side, owned here: HWND16 handles, classes, window
//     procedures at 16-bit CS:IP, a posted-message queue;
//   * the host side, behind the WindowHost interface: a real window per
//     top-level HWND16 (Win32WindowHost in the launcher) or nothing at all
//     (HeadlessHost, for tests). HWND16 <-> host window is a bidirectional map.
//
// Messages for a 16-bit window are handled by calling its window procedure
// on the interpreter (Cpu::CallFar), from SendMessage and DispatchMessage.
// Host input (close button, keys, mouse) comes back as posted messages.
//
// Timers (SetTimer) are kept as due times, not queued messages: like
// WM_PAINT, a WM_TIMER is synthesized when the queue is otherwise empty and a
// timer is due, at most one per timer. GetMessage sleeps until the next due
// time (or input), so an idle task costs nothing and timers fire on time.

#include <cstdint>
#include <deque>
#include <limits>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace retro::win16 {

class Runtime;

namespace wm {
constexpr uint16_t Create = 0x0001, Destroy = 0x0002, Move = 0x0003, Size = 0x0005, Paint = 0x000F,
                   Close = 0x0010, Quit = 0x0012, EraseBkgnd = 0x0014, ShowWindow = 0x0018,
                   NcCreate = 0x0081, NcDestroy = 0x0082,
                   KeyDown = 0x0100, KeyUp = 0x0101, Char = 0x0102, MouseMove = 0x0200,
                   LButtonDown = 0x0201, LButtonUp = 0x0202, RButtonDown = 0x0204,
                   RButtonUp = 0x0205, Timer = 0x0113, User = 0x0400;
}  // namespace wm

namespace ws {
constexpr uint32_t Popup = 0x80000000, Child = 0x40000000, Visible = 0x10000000;
}  // namespace ws

constexpr int16_t kScreenWidth = 640;  // the 16-bit desktop (VGA)
constexpr int16_t kScreenHeight = 480;
constexpr int16_t kUseDefault = -32768;  // CW_USEDEFAULT (8000h)

struct Rect16 {
    int16_t left = 0, top = 0, right = 0, bottom = 0;
    bool Empty() const { return right <= left || bottom <= top; }
};

struct Msg16 {
    uint16_t hwnd = 0;
    uint16_t message = 0;
    uint16_t wParam = 0;
    uint32_t lParam = 0;
    uint32_t time = 0;
    int16_t x = 0;
    int16_t y = 0;
};

// The platform side of windowing.
class WindowHost {
public:
    using Deliver = std::function<void(uint16_t hwnd16, uint16_t message, uint16_t wParam,
                                       uint32_t lParam)>;
    struct WindowInfo {
        uint16_t hwnd16 = 0;
        std::string title;
        int16_t x = 0, y = 0, width = 0, height = 0;  // 16-bit screen coordinates
        bool fullscreen = false;  // covers the 16-bit screen: present it borderless
    };

    // Pump deadlines: QPC times (retro::QpcNow).
    static constexpr int64_t kPoll = 0;
    static constexpr int64_t kForever = std::numeric_limits<int64_t>::max();

    virtual ~WindowHost() = default;
    virtual uint64_t Create(const WindowInfo& info) = 0;  // 0 = failed
    virtual void Show(uint64_t window, bool show) = 0;
    virtual void Destroy(uint64_t window) = 0;
    // A new frame of the window's content: 32-bit BGRA, top-down, width x
    // height (the 16-bit window size). The host scales it onto the screen.
    virtual void Present(uint64_t window, const uint32_t* pixels, int width, int height) = 0;
    // Delivers pending host input as 16-bit messages. If there is none, waits
    // for some until the QPC deadline `until` (kPoll: don't wait; kForever: no
    // deadline). Returns false only for kForever when no input can ever arrive
    // (so waiting would hang the task).
    virtual bool Pump(const Deliver& deliver, int64_t until) = 0;
};

// No real windows: records them, and delivers input that tests queue up
// (addressed to host windows, like real input) once the task waits. A wait
// with a deadline and nothing queued sleeps until the deadline.
class HeadlessHost : public WindowHost {
public:
    struct Record {
        uint64_t id = 0;
        WindowInfo info;
        bool visible = false;
        bool destroyed = false;
    };
    struct Event {
        uint64_t hostWindow;  // Record::id (the first window created is 1)
        uint16_t message, wParam;
        uint32_t lParam;
    };

    struct Frame {
        uint64_t window = 0;
        int width = 0, height = 0;
        std::vector<uint32_t> pixels;
        uint32_t At(int x, int y) const { return pixels[size_t(y) * width + x] & 0x00FFFFFF; }
    };

    uint64_t Create(const WindowInfo& info) override;
    void Show(uint64_t window, bool show) override;
    void Destroy(uint64_t window) override;
    void Present(uint64_t window, const uint32_t* pixels, int width, int height) override;
    bool Pump(const Deliver& deliver, int64_t until) override;

    std::vector<Record> windows;
    std::deque<Event> events;  // delivered on the next Pump
    Frame lastFrame;           // the most recent Present
    uint32_t presents = 0;
};

class User {
public:
    struct WindowClass {
        std::string name;  // upper case
        uint16_t atom = 0;
        uint16_t style = 0;
        uint16_t procSel = 0, procOff = 0;
        uint16_t clsExtra = 0, wndExtra = 0;
        uint16_t hInstance = 0, hIcon = 0, hCursor = 0, hbrBackground = 0;
    };
    struct Window {
        uint16_t hwnd = 0;
        std::string className;
        std::string title;
        uint32_t style = 0, exStyle = 0;
        int16_t x = 0, y = 0, cx = 0, cy = 0;
        uint16_t parent = 0, menu = 0, hInstance = 0;
        uint16_t procSel = 0, procOff = 0;
        bool visible = false;
        bool destroying = false;
        uint64_t host = 0;  // host window (top-level windows only)
        Rect16 update;      // invalid area (bounding box); empty = nothing to paint
        bool erase = false; // WM_ERASEBKGND due at the next BeginPaint
    };
    struct CreateParams {
        uint32_t exStyle = 0;
        uint16_t classSel = 0, classOff = 0;  // selector 0: offset is a class atom
        uint16_t nameSel = 0, nameOff = 0;
        uint32_t style = 0;
        int16_t x = 0, y = 0, cx = 0, cy = 0;
        uint16_t parent = 0, menu = 0, hInstance = 0;
        uint16_t paramSel = 0, paramOff = 0;
    };
    struct Timer {
        uint16_t hwnd = 0, id = 0;
        uint16_t procSel = 0, procOff = 0;  // TIMERPROC, or 0:0 for WM_TIMER to the window
        uint32_t intervalMs = 0;
        int64_t due = 0;  // QPC
    };
    enum class Fetch { Message, Quit, Empty, NoInput };

    // Windows 3.x timers tick with the PC timer (18.2 Hz): nothing fires more
    // often than every 55 ms, and programs written for it rely on that.
    static constexpr uint32_t kMinTimerMs = 55;

    // (Method names avoid the Win32 API names: <windows.h> defines CreateWindow,
    // SendMessage, ... as macros, and hosts include both.)

    explicit User(Runtime& rt);

    void SetHost(WindowHost* host) { host_ = host ? host : &headless_; }
    WindowHost& Host() { return *host_; }

    uint16_t RegisterWindowClass(uint16_t sel, uint16_t off);  // WNDCLASS far pointer; returns the atom
    uint16_t Create(const CreateParams& p);
    bool Destroy(uint16_t hwnd);
    bool Show(uint16_t hwnd, uint16_t cmdShow);  // returns previous visibility

    uint32_t Send(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam);
    uint32_t DefProc(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam);
    bool Post(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam);
    void PostQuit(uint16_t exitCode);
    Fetch Next(Msg16& out, uint16_t hwndFilter, uint16_t minMsg, uint16_t maxMsg, bool remove,
               bool wait);

    // SetTimer: returns the timer id (a new one for hwnd 0), 0 on failure.
    // Setting an existing (hwnd, id) timer again restarts it.
    uint16_t StartTimer(uint16_t hwnd, uint16_t id, uint16_t elapseMs, uint16_t procSel, uint16_t procOff);
    bool StopTimer(uint16_t hwnd, uint16_t id);  // KillTimer
    // True if sel:off is a live timer's TIMERPROC (DispatchMessage only calls those).
    bool IsTimerProc(uint16_t sel, uint16_t off) const;
    size_t TimerCount() const { return timers_.size(); }

    // Painting. `rect` null = the whole client area.
    void Invalidate(uint16_t hwnd, const Rect16* rect, bool erase);
    void Validate(uint16_t hwnd, const Rect16* rect);
    bool Update(uint16_t hwnd);  // UpdateWindow: WM_PAINT now if anything is invalid
    // BeginPaint fills the PAINTSTRUCT at sel:off and returns the window DC.
    uint16_t BeginPaint(uint16_t hwnd, uint16_t sel, uint16_t off);
    bool EndPaint(uint16_t hwnd, uint16_t sel, uint16_t off);
    Rect16 ClientRect(uint16_t hwnd) const;

    const Window* Find(uint16_t hwnd) const;
    const WindowClass* FindClass(const std::string& name) const;
    uint16_t HwndForHost(uint64_t host) const;
    uint64_t HostForHwnd(uint16_t hwnd) const;
    size_t WindowCount() const { return windows_.size(); }
    size_t QueueLength() const { return queue_.size(); }

private:
    Window* FindMutable(uint16_t hwnd);
    bool Matches(const Msg16& m, uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const;
    bool PumpHost(int64_t until);
    Timer* DueTimer(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg, int64_t now);
    int64_t NextTimerDue(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const;

    Runtime& rt_;
    HeadlessHost headless_;
    WindowHost* host_ = &headless_;
    std::vector<WindowClass> classes_;
    std::map<uint16_t, Window> windows_;
    std::map<uint64_t, uint16_t> hostToHwnd_;
    std::deque<Msg16> queue_;
    std::vector<Timer> timers_;
    uint16_t nextTimerId_ = 1;  // for timers without a window
    bool quitPending_ = false;
    uint16_t quitCode_ = 0;
    uint16_t nextHwnd_ = 0x2004;
    uint16_t nextAtom_ = 0xC000;
    int cascade_ = 0;
};

}  // namespace retro::win16
