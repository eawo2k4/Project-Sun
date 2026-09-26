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

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace retro::win16 {

class Runtime;

namespace wm {
constexpr uint16_t Create = 0x0001, Destroy = 0x0002, Move = 0x0003, Size = 0x0005, Close = 0x0010,
                   Quit = 0x0012, ShowWindow = 0x0018, NcCreate = 0x0081, NcDestroy = 0x0082,
                   KeyDown = 0x0100, KeyUp = 0x0101, Char = 0x0102, MouseMove = 0x0200,
                   LButtonDown = 0x0201, LButtonUp = 0x0202, RButtonDown = 0x0204,
                   RButtonUp = 0x0205, User = 0x0400;
}  // namespace wm

namespace ws {
constexpr uint32_t Popup = 0x80000000, Child = 0x40000000, Visible = 0x10000000;
}  // namespace ws

constexpr int16_t kScreenWidth = 640;  // the 16-bit desktop (VGA)
constexpr int16_t kScreenHeight = 480;
constexpr int16_t kUseDefault = -32768;  // CW_USEDEFAULT (8000h)

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

    virtual ~WindowHost() = default;
    virtual uint64_t Create(const WindowInfo& info) = 0;  // 0 = failed
    virtual void Show(uint64_t window, bool show) = 0;
    virtual void Destroy(uint64_t window) = 0;
    // Delivers pending host input as 16-bit messages. With `wait`, blocks until
    // something arrives; returns false if nothing ever can (so waiting would
    // hang the task).
    virtual bool Pump(const Deliver& deliver, bool wait) = 0;
};

// No real windows: records them, and delivers input that tests queue up
// (addressed to host windows, like real input) once the task waits for input.
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

    uint64_t Create(const WindowInfo& info) override;
    void Show(uint64_t window, bool show) override;
    void Destroy(uint64_t window) override;
    bool Pump(const Deliver& deliver, bool wait) override;

    std::vector<Record> windows;
    std::deque<Event> events;  // delivered on the next Pump
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
    enum class Fetch { Message, Quit, Empty, NoInput };

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

    const Window* Find(uint16_t hwnd) const;
    const WindowClass* FindClass(const std::string& name) const;
    uint16_t HwndForHost(uint64_t host) const;
    uint64_t HostForHwnd(uint16_t hwnd) const;
    size_t WindowCount() const { return windows_.size(); }
    size_t QueueLength() const { return queue_.size(); }

private:
    Window* FindMutable(uint16_t hwnd);
    bool Matches(const Msg16& m, uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const;
    void PumpHost(bool wait, bool& canWait);

    Runtime& rt_;
    HeadlessHost headless_;
    WindowHost* host_ = &headless_;
    std::vector<WindowClass> classes_;
    std::map<uint16_t, Window> windows_;
    std::map<uint64_t, uint16_t> hostToHwnd_;
    std::deque<Msg16> queue_;
    bool quitPending_ = false;
    uint16_t quitCode_ = 0;
    uint16_t nextHwnd_ = 0x2004;
    uint16_t nextAtom_ = 0xC000;
    int cascade_ = 0;
};

}  // namespace retro::win16
