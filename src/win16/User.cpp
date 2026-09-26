// USER: window classes, windows, the message queue, and their API.

#include "win16/User.h"

#include <algorithm>
#include <cctype>

#include "retro/DisplayMath.h"
#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool IsSystemClass(const std::string& name) {
    static const char* const kSystem[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX",
                                          "SCROLLBAR", "MDICLIENT"};
    return std::any_of(std::begin(kSystem), std::end(kSystem),
                       [&](const char* s) { return name == s; });
}

// MSG (Win16): hwnd, message, wParam, lParam (4), time (4), pt.x, pt.y = 18 bytes.
void WriteMsg(Memory& mem, FarPtr p, const Msg16& m) {
    mem.Write16(p.sel, p.off, m.hwnd);
    mem.Write16(p.sel, uint16_t(p.off + 2), m.message);
    mem.Write16(p.sel, uint16_t(p.off + 4), m.wParam);
    mem.Write16(p.sel, uint16_t(p.off + 6), uint16_t(m.lParam));
    mem.Write16(p.sel, uint16_t(p.off + 8), uint16_t(m.lParam >> 16));
    mem.Write16(p.sel, uint16_t(p.off + 10), uint16_t(m.time));
    mem.Write16(p.sel, uint16_t(p.off + 12), uint16_t(m.time >> 16));
    mem.Write16(p.sel, uint16_t(p.off + 14), uint16_t(m.x));
    mem.Write16(p.sel, uint16_t(p.off + 16), uint16_t(m.y));
}

Msg16 ReadMsg(const Memory& mem, FarPtr p) {
    Msg16 m;
    m.hwnd = mem.Read16(p.sel, p.off);
    m.message = mem.Read16(p.sel, uint16_t(p.off + 2));
    m.wParam = mem.Read16(p.sel, uint16_t(p.off + 4));
    m.lParam = mem.Read16(p.sel, uint16_t(p.off + 6)) | (uint32_t(mem.Read16(p.sel, uint16_t(p.off + 8))) << 16);
    return m;
}

uint32_t MakeLong(int16_t lo, int16_t hi) { return uint16_t(lo) | (uint32_t(uint16_t(hi)) << 16); }

}  // namespace

// --- HeadlessHost ----------------------------------------------------------------------------

uint64_t HeadlessHost::Create(const WindowInfo& info) {
    windows.push_back({windows.size() + 1, info, false, false});
    return windows.back().id;
}

void HeadlessHost::Show(uint64_t window, bool show) {
    for (Record& r : windows) {
        if (r.id == window) r.visible = show;
    }
}

void HeadlessHost::Destroy(uint64_t window) {
    for (Record& r : windows) {
        if (r.id == window) r.destroyed = true;
    }
}

bool HeadlessHost::Pump(const Deliver& deliver, bool wait) {
    // Queued input arrives when the task waits for it, like a user acting
    // while the program idles in GetMessage. That keeps tests deterministic.
    if (!wait) return true;
    if (events.empty()) return false;  // nothing will ever arrive
    while (!events.empty()) {
        const Event e = events.front();
        events.pop_front();
        for (const Record& r : windows) {
            if (r.id == e.hostWindow && !r.destroyed) deliver(r.info.hwnd16, e.message, e.wParam, e.lParam);
        }
    }
    return true;
}

// --- User ---------------------------------------------------------------------------------

User::User(Runtime& rt) : rt_(rt) {}

const User::Window* User::Find(uint16_t hwnd) const {
    const auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

User::Window* User::FindMutable(uint16_t hwnd) {
    const auto it = windows_.find(hwnd);
    return it == windows_.end() ? nullptr : &it->second;
}

const User::WindowClass* User::FindClass(const std::string& name) const {
    for (const WindowClass& c : classes_) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

uint16_t User::HwndForHost(uint64_t host) const {
    const auto it = hostToHwnd_.find(host);
    return it == hostToHwnd_.end() ? 0 : it->second;
}

uint64_t User::HostForHwnd(uint16_t hwnd) const {
    const Window* w = Find(hwnd);
    return w ? w->host : 0;
}

uint16_t User::RegisterWindowClass(uint16_t sel, uint16_t off) {
    // WNDCLASS (Win16): style, lpfnWndProc (far), cbClsExtra, cbWndExtra,
    // hInstance, hIcon, hCursor, hbrBackground, lpszMenuName (far),
    // lpszClassName (far) = 26 bytes.
    Memory& mem = rt_.Mem();
    WindowClass c;
    c.style = mem.Read16(sel, off);
    c.procOff = mem.Read16(sel, uint16_t(off + 2));
    c.procSel = mem.Read16(sel, uint16_t(off + 4));
    c.clsExtra = mem.Read16(sel, uint16_t(off + 6));
    c.wndExtra = mem.Read16(sel, uint16_t(off + 8));
    c.hInstance = mem.Read16(sel, uint16_t(off + 10));
    c.hIcon = mem.Read16(sel, uint16_t(off + 12));
    c.hCursor = mem.Read16(sel, uint16_t(off + 14));
    c.hbrBackground = mem.Read16(sel, uint16_t(off + 16));
    c.name = Upper(mem.ReadString(mem.Read16(sel, uint16_t(off + 24)), mem.Read16(sel, uint16_t(off + 22))));

    const Descriptor* proc = mem.Lookup(c.procSel);
    if (c.name.empty() || FindClass(c.name) || !proc || proc->kind != SegmentKind::Code) {
        rt_.Print("RegisterClass(\"" + c.name + "\") failed: " +
                  (c.name.empty() ? "no class name"
                   : FindClass(c.name) ? "class already registered"
                                       : "window procedure is not in a code segment"));
        return 0;
    }
    c.atom = nextAtom_++;
    classes_.push_back(c);
    return c.atom;
}

uint16_t User::Create(const CreateParams& p) {
    Memory& mem = rt_.Mem();
    const WindowClass* cls = nullptr;
    std::string className;
    if (p.classSel == 0) {
        for (const WindowClass& c : classes_) {
            if (c.atom == p.classOff) cls = &c;
        }
        className = "#" + std::to_string(p.classOff);
    } else {
        className = Upper(mem.ReadString(p.classSel, p.classOff));
        cls = FindClass(className);
    }
    if (!cls) {
        rt_.Print("CreateWindow: class \"" + className + "\" is not registered" +
                  (IsSystemClass(className) ? " (system control classes are not implemented yet)" : ""));
        return 0;
    }

    Window w;
    w.hwnd = nextHwnd_;
    nextHwnd_ = uint16_t(nextHwnd_ + 4);
    w.className = cls->name;
    w.title = p.nameSel ? mem.ReadString(p.nameSel, p.nameOff) : "";
    w.style = p.style;
    w.exStyle = p.exStyle;
    w.x = p.x;
    w.y = p.y;
    w.cx = p.cx;
    w.cy = p.cy;
    if (w.x == kUseDefault || w.y == kUseDefault) {
        w.x = w.y = int16_t(16 * (cascade_++ % 8));
    }
    if (w.cx == kUseDefault || w.cy == kUseDefault) {
        w.cx = kScreenWidth * 3 / 4;
        w.cy = kScreenHeight * 3 / 4;
    }
    w.parent = p.parent;
    w.menu = p.menu;
    w.hInstance = p.hInstance ? p.hInstance : cls->hInstance;
    w.procSel = cls->procSel;
    w.procOff = cls->procOff;
    const uint16_t hwnd = w.hwnd;
    windows_[hwnd] = w;

    // CREATESTRUCT (Win16), passed by far pointer with WM_NCCREATE/WM_CREATE:
    // lpCreateParams, hInstance, hMenu, hwndParent, cy, cx, y, x, style,
    // lpszName, lpszClass, dwExStyle = 34 bytes.
    Runtime::Scratch cs(rt_, 34);
    const uint16_t s = cs.Selector(), o = cs.Offset();
    auto put = [&](uint16_t at, uint16_t v) { mem.Write16(s, uint16_t(o + at), v); };
    put(0, p.paramOff);
    put(2, p.paramSel);
    put(4, w.hInstance);
    put(6, p.menu);
    put(8, p.parent);
    put(10, uint16_t(w.cy));
    put(12, uint16_t(w.cx));
    put(14, uint16_t(w.y));
    put(16, uint16_t(w.x));
    put(18, uint16_t(p.style));
    put(20, uint16_t(p.style >> 16));
    put(22, p.nameOff);
    put(24, p.nameSel);
    put(26, p.classOff);
    put(28, p.classSel);
    put(30, uint16_t(p.exStyle));
    put(32, uint16_t(p.exStyle >> 16));
    const uint32_t lParam = (uint32_t(s) << 16) | o;

    if (Send(hwnd, wm::NcCreate, 0, lParam) == 0 ||
        int32_t(Send(hwnd, wm::Create, 0, lParam)) == -1) {
        windows_.erase(hwnd);
        return 0;
    }
    Window* win = FindMutable(hwnd);
    if (!win) return 0;  // destroyed while being created

    if (!(win->style & ws::Child)) {
        WindowHost::WindowInfo info;
        info.hwnd16 = hwnd;
        info.title = win->title;
        info.x = win->x;
        info.y = win->y;
        info.width = win->cx;
        info.height = win->cy;
        info.fullscreen = CoversScreen({win->x, win->y, win->x + win->cx, win->y + win->cy},
                                       {kScreenWidth, kScreenHeight});
        win->host = host_->Create(info);
        if (win->host) hostToHwnd_[win->host] = hwnd;
    }
    const int16_t cx = win->cx, cy = win->cy, x = win->x, y = win->y;
    if (win->style & ws::Visible) Show(hwnd, 1);
    Send(hwnd, wm::Size, 0, MakeLong(cx, cy));
    Send(hwnd, wm::Move, 0, MakeLong(x, y));
    return Find(hwnd) ? hwnd : 0;
}

bool User::Destroy(uint16_t hwnd) {
    Window* w = FindMutable(hwnd);
    if (!w || w->destroying) return false;
    w->destroying = true;

    Send(hwnd, wm::Destroy, 0, 0);
    std::vector<uint16_t> children;
    for (const auto& [h, child] : windows_) {
        if (child.parent == hwnd) children.push_back(h);
    }
    for (uint16_t child : children) Destroy(child);
    Send(hwnd, wm::NcDestroy, 0, 0);

    if (Window* gone = FindMutable(hwnd)) {
        if (gone->host) {
            host_->Destroy(gone->host);
            hostToHwnd_.erase(gone->host);
        }
        windows_.erase(hwnd);
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&](const Msg16& m) { return m.hwnd == hwnd; }),
                 queue_.end());
    return true;
}

bool User::Show(uint16_t hwnd, uint16_t cmdShow) {
    Window* w = FindMutable(hwnd);
    if (!w) return false;
    const bool previous = w->visible;
    const bool show = cmdShow != 0;  // SW_HIDE = 0
    if (show != previous) {
        w->visible = show;
        if (w->host) host_->Show(w->host, show);
        Send(hwnd, wm::ShowWindow, show ? 1 : 0, 0);
    }
    return previous;
}

uint32_t User::Send(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam) {
    const Window* w = Find(hwnd);
    if (!w) return 0;
    if (!w->procSel) return DefProc(hwnd, msg, wParam, lParam);
    // The window procedure runs on the interpreter; Pascal arguments
    // (hwnd, msg, wParam, lParam), DS = the window's instance data.
    return rt_.Processor().CallFar(w->procSel, w->procOff,
                                   {hwnd, msg, wParam, uint16_t(lParam >> 16), uint16_t(lParam)},
                                   w->hInstance);
}

uint32_t User::DefProc(uint16_t hwnd, uint16_t msg, uint16_t, uint32_t) {
    switch (msg) {
    case wm::NcCreate: return 1;
    case wm::Close: Destroy(hwnd); return 0;
    default: return 0;
    }
}

bool User::Post(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam) {
    if (hwnd && !Find(hwnd)) return false;
    queue_.push_back({hwnd, msg, wParam, lParam, rt_.TickCount(), 0, 0});
    return true;
}

void User::PostQuit(uint16_t exitCode) {
    quitPending_ = true;
    quitCode_ = exitCode;
}

bool User::Matches(const Msg16& m, uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const {
    if (hwnd && m.hwnd != hwnd) return false;
    if (minMsg == 0 && maxMsg == 0) return true;
    return m.message >= minMsg && m.message <= maxMsg;
}

void User::PumpHost(bool wait, bool& canWait) {
    canWait = host_->Pump(
        [this](uint16_t hwnd16, uint16_t message, uint16_t wParam, uint32_t lParam) {
            if (Find(hwnd16)) Post(hwnd16, message, wParam, lParam);
        },
        wait);
}

User::Fetch User::Next(Msg16& out, uint16_t hwndFilter, uint16_t minMsg, uint16_t maxMsg,
                       bool remove, bool wait) {
    for (;;) {
        bool canWait = false;
        PumpHost(false, canWait);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (Matches(*it, hwndFilter, minMsg, maxMsg)) {
                out = *it;
                if (remove) queue_.erase(it);
                return Fetch::Message;
            }
        }
        // WM_QUIT comes only once everything else has been retrieved.
        if (quitPending_) {
            out = Msg16{0, wm::Quit, quitCode_, 0, rt_.TickCount(), 0, 0};
            if (remove) quitPending_ = false;
            return Fetch::Quit;
        }
        if (!wait) return Fetch::Empty;
        PumpHost(true, canWait);
        if (!canWait) return Fetch::NoInput;
    }
}

// --- API ------------------------------------------------------------------------------------

namespace {

void MessageBox(Runtime& rt, Cpu& cpu) {
    // (hwnd, text, caption, type)
    const FarPtr caption = ArgPtr(cpu, 2), text = ArgPtr(cpu, 6);
    rt.Print("MessageBox [" + rt.Mem().ReadString(caption.sel, caption.off) +
             "]: " + rt.Mem().ReadString(text.sel, text.off));
    cpu.Regs().r[AX] = 1;  // IDOK
    cpu.ReturnFar(12);
}

void InitApp(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(2);
}

void PostQuitMessage(Runtime& rt, Cpu& cpu) {
    rt.Windows().PostQuit(cpu.StackArg(0));
    cpu.ReturnFar(2);
}

void GetTickCount(Runtime& rt, Cpu& cpu) {
    SetResult(cpu, rt.TickCount());
    cpu.ReturnFar(0);
}

// CreateWindow(lpClassName, lpWindowName, dwStyle, x, y, cx, cy, hwndParent,
// hMenu, hInstance, lpParam); CreateWindowEx adds dwExStyle in front.
User::CreateParams ReadCreateParams(const Cpu& cpu, bool ex) {
    User::CreateParams p;
    const FarPtr param = ArgPtr(cpu, 0), name = ArgPtr(cpu, 22), cls = ArgPtr(cpu, 26);
    p.paramOff = param.off;
    p.paramSel = param.sel;
    p.hInstance = cpu.StackArg(4);
    p.menu = cpu.StackArg(6);
    p.parent = cpu.StackArg(8);
    p.cy = ArgInt(cpu, 10);
    p.cx = ArgInt(cpu, 12);
    p.y = ArgInt(cpu, 14);
    p.x = ArgInt(cpu, 16);
    p.style = ArgLong(cpu, 18);
    p.nameOff = name.off;
    p.nameSel = name.sel;
    p.classOff = cls.off;
    p.classSel = cls.sel;
    if (ex) p.exStyle = ArgLong(cpu, 30);
    return p;
}

void CreateWindow(Runtime& rt, Cpu& cpu) {
    const User::CreateParams p = ReadCreateParams(cpu, false);
    cpu.Regs().r[AX] = rt.Windows().Create(p);
    cpu.ReturnFar(30);
}

void CreateWindowEx(Runtime& rt, Cpu& cpu) {
    const User::CreateParams p = ReadCreateParams(cpu, true);
    cpu.Regs().r[AX] = rt.Windows().Create(p);
    cpu.ReturnFar(34);
}

void ShowWindow(Runtime& rt, Cpu& cpu) {
    cpu.Regs().r[AX] = rt.Windows().Show(cpu.StackArg(2), cpu.StackArg(0)) ? 1 : 0;
    cpu.ReturnFar(4);
}

void DestroyWindow(Runtime& rt, Cpu& cpu) {
    cpu.Regs().r[AX] = rt.Windows().Destroy(cpu.StackArg(0)) ? 1 : 0;
    cpu.ReturnFar(2);
}

void RegisterClass(Runtime& rt, Cpu& cpu) {
    const FarPtr wc = ArgPtr(cpu, 0);
    cpu.Regs().r[AX] = rt.Windows().RegisterWindowClass(wc.sel, wc.off);
    cpu.ReturnFar(4);
}

// (hwnd, msg, wParam, lParam): 10 bytes of arguments.
struct MessageArgs {
    uint16_t hwnd, msg, wParam;
    uint32_t lParam;
};
MessageArgs ReadMessageArgs(const Cpu& cpu) {
    return {cpu.StackArg(8), cpu.StackArg(6), cpu.StackArg(4), ArgLong(cpu, 0)};
}

void DefWindowProc(Runtime& rt, Cpu& cpu) {
    const MessageArgs a = ReadMessageArgs(cpu);
    SetResult(cpu, rt.Windows().DefProc(a.hwnd, a.msg, a.wParam, a.lParam));
    cpu.ReturnFar(10);
}

void SendMessage(Runtime& rt, Cpu& cpu) {
    const MessageArgs a = ReadMessageArgs(cpu);
    SetResult(cpu, rt.Windows().Send(a.hwnd, a.msg, a.wParam, a.lParam));
    cpu.ReturnFar(10);
}

void PostMessage(Runtime& rt, Cpu& cpu) {
    const MessageArgs a = ReadMessageArgs(cpu);
    cpu.Regs().r[AX] = rt.Windows().Post(a.hwnd, a.msg, a.wParam, a.lParam) ? 1 : 0;
    cpu.ReturnFar(10);
}

// GetMessage(lpMsg, hwnd, wMsgFilterMin, wMsgFilterMax)
void GetMessage(Runtime& rt, Cpu& cpu) {
    const FarPtr msgPtr = ArgPtr(cpu, 6);
    Msg16 m;
    const User::Fetch r = rt.Windows().Next(m, cpu.StackArg(4), cpu.StackArg(2), cpu.StackArg(0),
                                            true, true);
    if (r == User::Fetch::NoInput) {
        rt.Exit(TaskExit::Kind::Blocked, 0,
                "GetMessage: the queue is empty and no input can ever arrive");
        return;
    }
    WriteMsg(rt.Mem(), msgPtr, m);
    cpu.Regs().r[AX] = r == User::Fetch::Quit ? 0 : 1;
    cpu.ReturnFar(10);
}

// PeekMessage(lpMsg, hwnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg)
void PeekMessage(Runtime& rt, Cpu& cpu) {
    const FarPtr msgPtr = ArgPtr(cpu, 8);
    Msg16 m;
    const bool remove = (cpu.StackArg(0) & 1) != 0;  // PM_REMOVE
    const User::Fetch r = rt.Windows().Next(m, cpu.StackArg(6), cpu.StackArg(4), cpu.StackArg(2),
                                            remove, false);
    const bool got = r == User::Fetch::Message || r == User::Fetch::Quit;
    if (got) WriteMsg(rt.Mem(), msgPtr, m);
    cpu.Regs().r[AX] = got ? 1 : 0;
    cpu.ReturnFar(12);
}

void TranslateMessage(Runtime&, Cpu& cpu) {
    // Host input already arrives as WM_CHAR where the host translated keys.
    cpu.Regs().r[AX] = 0;
    cpu.ReturnFar(4);
}

void DispatchMessage(Runtime& rt, Cpu& cpu) {
    const Msg16 m = ReadMsg(rt.Mem(), ArgPtr(cpu, 0));
    SetResult(cpu, m.hwnd ? rt.Windows().Send(m.hwnd, m.message, m.wParam, m.lParam) : 0);
    cpu.ReturnFar(4);
}

void UpdateWindow(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 1;  // no painting yet, so nothing to update
    cpu.ReturnFar(2);
}

void LoadCursor(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 0x0F10;  // placeholder handle (no resources yet)
    cpu.ReturnFar(6);
}

void LoadIcon(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 0x0F20;
    cpu.ReturnFar(6);
}

void GetSystemMetrics(Runtime&, Cpu& cpu) {
    int16_t v = 0;
    switch (cpu.StackArg(0)) {
    case 0: v = kScreenWidth; break;   // SM_CXSCREEN
    case 1: v = kScreenHeight; break;  // SM_CYSCREEN
    case 4: v = 20; break;             // SM_CYCAPTION
    case 5: case 6: v = 1; break;      // SM_CXBORDER, SM_CYBORDER
    case 7: case 8: v = 4; break;      // SM_CXDLGFRAME, SM_CYDLGFRAME
    case 15: v = 18; break;            // SM_CYMENU
    case 16: v = kScreenWidth; break;  // SM_CXFULLSCREEN
    case 17: v = kScreenHeight - 20; break;
    default: v = 0; break;
    }
    cpu.Regs().r[AX] = uint16_t(v);
    cpu.ReturnFar(2);
}

}  // namespace

std::vector<ApiFunction> UserApi() {
    return {
        {1, "MESSAGEBOX", MessageBox},
        {5, "INITAPP", InitApp},
        {6, "POSTQUITMESSAGE", PostQuitMessage},
        {13, "GETTICKCOUNT", GetTickCount},
        {41, "CREATEWINDOW", CreateWindow},
        {42, "SHOWWINDOW", ShowWindow},
        {53, "DESTROYWINDOW", DestroyWindow},
        {57, "REGISTERCLASS", RegisterClass},
        {107, "DEFWINDOWPROC", DefWindowProc},
        {108, "GETMESSAGE", GetMessage},
        {109, "PEEKMESSAGE", PeekMessage},
        {110, "POSTMESSAGE", PostMessage},
        {111, "SENDMESSAGE", SendMessage},
        {113, "TRANSLATEMESSAGE", TranslateMessage},
        {114, "DISPATCHMESSAGE", DispatchMessage},
        {124, "UPDATEWINDOW", UpdateWindow},
        {173, "LOADCURSOR", LoadCursor},
        {174, "LOADICON", LoadIcon},
        {179, "GETSYSTEMMETRICS", GetSystemMetrics},
        {452, "CREATEWINDOWEX", CreateWindowEx},
    };
}

}  // namespace retro::win16
