// USER: window classes, windows, the message queue, and their API.

#include "win16/User.h"

#include <algorithm>
#include <cctype>

#include "retro/DisplayMath.h"
#include "retro/FramePacing.h"
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

int64_t MsToQpc(uint32_t ms) { return int64_t(ms) * QpcFrequency() / 1000; }

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

void HeadlessHost::Present(uint64_t window, const uint32_t* pixels, int width, int height) {
    lastFrame.window = window;
    lastFrame.width = width;
    lastFrame.height = height;
    lastFrame.pixels.assign(pixels, pixels + size_t(width) * height);
    ++presents;
}

void HeadlessHost::Destroy(uint64_t window) {
    for (Record& r : windows) {
        if (r.id == window) r.destroyed = true;
    }
}

bool HeadlessHost::Pump(const Deliver& deliver, int64_t until) {
    // Queued input arrives when the task waits for it, like a user acting
    // while the program idles in GetMessage. That keeps tests deterministic.
    if (until == kPoll) return true;
    if (events.empty()) {
        if (until == kForever) return false;  // nothing will ever arrive
        PreciseWaiter().WaitUntil(until);
        return true;
    }
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
        rt_.Graphics().CreateSurface(hwnd, win->cx, win->cy);
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
    rt_.Graphics().DestroySurface(hwnd);
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.hwnd == hwnd; }),
                  timers_.end());
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
        if (show) Invalidate(hwnd, nullptr, true);  // newly visible: paint it all
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

uint32_t User::DefProc(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t) {
    switch (msg) {
    case wm::NcCreate: return 1;
    case wm::Close: Destroy(hwnd); return 0;
    case wm::Paint: {
        // Nobody painted: validate, or WM_PAINT would come back forever.
        Runtime::Scratch ps(rt_, 32);
        BeginPaint(hwnd, ps.Selector(), ps.Offset());
        EndPaint(hwnd, ps.Selector(), ps.Offset());
        return 0;
    }
    case wm::EraseBkgnd: {  // fill with the class background brush
        const Window* w = Find(hwnd);
        const WindowClass* c = w ? FindClass(w->className) : nullptr;
        if (!c || !c->hbrBackground) return 0;
        return rt_.Graphics().Fill(wParam, ClientRect(hwnd), c->hbrBackground) ? 1 : 0;
    }
    default: return 0;
    }
}

Rect16 User::ClientRect(uint16_t hwnd) const {
    const Window* w = Find(hwnd);
    return w ? Rect16{0, 0, w->cx, w->cy} : Rect16{};
}

void User::Invalidate(uint16_t hwnd, const Rect16* rect, bool erase) {
    Window* w = FindMutable(hwnd);
    if (!w) return;
    Rect16 r = rect ? *rect : ClientRect(hwnd);
    r.left = std::max<int16_t>(r.left, 0);
    r.top = std::max<int16_t>(r.top, 0);
    r.right = std::min<int16_t>(r.right, w->cx);
    r.bottom = std::min<int16_t>(r.bottom, w->cy);
    if (r.Empty()) return;
    if (w->update.Empty()) {
        w->update = r;
    } else {  // keep a bounding box
        w->update.left = std::min(w->update.left, r.left);
        w->update.top = std::min(w->update.top, r.top);
        w->update.right = std::max(w->update.right, r.right);
        w->update.bottom = std::max(w->update.bottom, r.bottom);
    }
    w->erase = w->erase || erase;
}

void User::Validate(uint16_t hwnd, const Rect16* rect) {
    Window* w = FindMutable(hwnd);
    if (!w) return;
    // Bounding-box region: validating a part that covers it clears it.
    const bool coversAll = !rect || (rect->left <= w->update.left && rect->top <= w->update.top &&
                                     rect->right >= w->update.right && rect->bottom >= w->update.bottom);
    if (coversAll) {
        w->update = {};
        w->erase = false;
    }
}

bool User::Update(uint16_t hwnd) {
    const Window* w = Find(hwnd);
    if (!w) return false;
    if (w->visible && !w->update.Empty()) Send(hwnd, wm::Paint, 0, 0);
    return true;
}

uint16_t User::BeginPaint(uint16_t hwnd, uint16_t sel, uint16_t off) {
    Window* w = FindMutable(hwnd);
    if (!w) return 0;
    const Rect16 paint = w->update;
    const bool erase = w->erase;
    w->update = {};  // BeginPaint validates
    w->erase = false;

    const uint16_t hdc = rt_.Graphics().GetWindowDc(hwnd);
    if (!hdc) return 0;
    rt_.Graphics().ClipTo(hdc, paint);  // drawing is limited to the invalid area
    const bool erased = erase && Send(hwnd, wm::EraseBkgnd, hdc, 0) != 0;

    // PAINTSTRUCT (Win16): hdc, fErase, rcPaint (4 ints), fRestore,
    // fIncUpdate, rgbReserved[16] = 32 bytes.
    Memory& mem = rt_.Mem();
    mem.Write16(sel, off, hdc);
    mem.Write16(sel, uint16_t(off + 2), erase && !erased ? 1 : 0);  // TRUE: the app must erase
    mem.Write16(sel, uint16_t(off + 4), uint16_t(paint.left));
    mem.Write16(sel, uint16_t(off + 6), uint16_t(paint.top));
    mem.Write16(sel, uint16_t(off + 8), uint16_t(paint.right));
    mem.Write16(sel, uint16_t(off + 10), uint16_t(paint.bottom));
    for (uint16_t i = 12; i < 32; i += 2) mem.Write16(sel, uint16_t(off + i), 0);
    return hdc;
}

bool User::EndPaint(uint16_t hwnd, uint16_t sel, uint16_t off) {
    if (!Find(hwnd)) return false;
    return rt_.Graphics().ReleaseWindowDc(rt_.Mem().Read16(sel, off));
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

bool User::PumpHost(int64_t until) {
    return host_->Pump(
        [this](uint16_t hwnd16, uint16_t message, uint16_t wParam, uint32_t lParam) {
            if (Find(hwnd16)) Post(hwnd16, message, wParam, lParam);
        },
        until);
}

uint16_t User::StartTimer(uint16_t hwnd, uint16_t id, uint16_t elapseMs, uint16_t procSel,
                          uint16_t procOff) {
    if (hwnd && !Find(hwnd)) return 0;
    if (procSel || procOff) {
        const Descriptor* d = rt_.Mem().Lookup(procSel);
        if (!d || d->kind != SegmentKind::Code) {
            rt_.Print("SetTimer failed: the timer procedure is not in a code segment");
            return 0;
        }
    }
    auto find = [&](uint16_t h, uint16_t i) {
        return std::find_if(timers_.begin(), timers_.end(),
                            [&](const Timer& t) { return t.hwnd == h && t.id == i; });
    };
    if (!hwnd) {  // the id argument is ignored: every such timer gets a new one
        do {
            id = nextTimerId_++;
        } while (id == 0 || find(0, id) != timers_.end());
    }
    const uint32_t interval = std::max<uint32_t>(elapseMs, kMinTimerMs);
    const Timer t{hwnd, id, procSel, procOff, interval, QpcNow() + MsToQpc(interval)};
    if (const auto it = find(hwnd, id); it != timers_.end()) {
        *it = t;
    } else {
        timers_.push_back(t);
    }
    return id ? id : 1;
}

bool User::StopTimer(uint16_t hwnd, uint16_t id) {
    const auto it = std::find_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.hwnd == hwnd && t.id == id; });
    if (it == timers_.end()) return false;
    timers_.erase(it);
    return true;
}

bool User::IsTimerProc(uint16_t sel, uint16_t off) const {
    return (sel || off) && std::any_of(timers_.begin(), timers_.end(), [&](const Timer& t) {
               return t.procSel == sel && t.procOff == off;
           });
}

User::Timer* User::DueTimer(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg, int64_t now) {
    Timer* due = nullptr;
    for (Timer& t : timers_) {
        if (t.due <= now && Matches(Msg16{t.hwnd, wm::Timer}, hwnd, minMsg, maxMsg) &&
            (!due || t.due < due->due))
            due = &t;
    }
    return due;
}

int64_t User::NextTimerDue(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const {
    int64_t next = WindowHost::kForever;
    for (const Timer& t : timers_) {
        if (Matches(Msg16{t.hwnd, wm::Timer}, hwnd, minMsg, maxMsg)) next = std::min(next, t.due);
    }
    return next;
}

User::Fetch User::Next(Msg16& out, uint16_t hwndFilter, uint16_t minMsg, uint16_t maxMsg,
                       bool remove, bool wait) {
    // The message pump is the frame boundary: show what was drawn since the
    // last call, paced to the frame cap.
    rt_.Graphics().PresentPending();
    for (;;) {
        PumpHost(WindowHost::kPoll);
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
        // Then WM_PAINT for a window with an invalid area. It stays "in the
        // queue" until BeginPaint (or ValidateRect) validates it.
        for (const auto& [h, w] : windows_) {
            if (w.visible && !w.update.Empty() && (!hwndFilter || h == hwndFilter) &&
                Matches(Msg16{h, wm::Paint}, hwndFilter, minMsg, maxMsg)) {
                out = Msg16{h, wm::Paint, 0, 0, rt_.TickCount(), 0, 0};
                return Fetch::Message;
            }
        }
        // Last, WM_TIMER for the timer that has been due longest.
        const int64_t now = QpcNow();
        if (Timer* t = DueTimer(hwndFilter, minMsg, maxMsg, now)) {
            out = Msg16{t->hwnd, wm::Timer, t->id, (uint32_t(t->procSel) << 16) | t->procOff,
                        rt_.TickCount(), 0, 0};
            if (remove) {
                // Keep the cadence; after a stall, skip the missed ticks
                // (one WM_TIMER, like Windows) rather than firing a burst.
                const int64_t period = MsToQpc(t->intervalMs);
                t->due += period;
                if (t->due <= now) t->due = now + period;
            }
            return Fetch::Message;
        }
        if (!wait) return Fetch::Empty;
        // Sleep until input arrives or the next timer is due.
        if (!PumpHost(NextTimerDue(hwndFilter, minMsg, maxMsg))) return Fetch::NoInput;
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
    uint32_t result = 0;
    if (m.message == wm::Timer && m.lParam) {
        // WM_TIMER of a timer with a TIMERPROC: call it instead of the window
        // procedure, (hwnd, WM_TIMER, idTimer, dwTime), DS = the task's DGROUP.
        // Only live timer procedures: lParam is just a number in the message.
        const uint16_t sel = uint16_t(m.lParam >> 16), off = uint16_t(m.lParam);
        if (rt.Windows().IsTimerProc(sel, off)) {
            const uint32_t now = rt.TickCount();
            result = rt.Processor().CallFar(sel, off,
                                            {m.hwnd, m.message, m.wParam, uint16_t(now >> 16), uint16_t(now)},
                                            rt.Module().dgroup);
        }
    } else if (m.hwnd) {
        result = rt.Windows().Send(m.hwnd, m.message, m.wParam, m.lParam);
    }
    SetResult(cpu, result);
    cpu.ReturnFar(4);
}

void SetTimer(Runtime& rt, Cpu& cpu) {  // (HWND, UINT id, UINT elapse, TIMERPROC or NULL) -> id
    const PascalArgs a(cpu, {2, 2, 2, 4});
    const FarPtr proc = a.Ptr(3);
    cpu.Regs().r[AX] = rt.Windows().StartTimer(a.Word(0), a.Word(1), a.Word(2), proc.sel, proc.off);
    cpu.ReturnFar(a.Bytes());
}

void KillTimer(Runtime& rt, Cpu& cpu) {  // (HWND, UINT id) -> BOOL
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = rt.Windows().StopTimer(a.Word(0), a.Word(1)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void LoadBitmap(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR name) -> HBITMAP
    const PascalArgs a(cpu, {2, 4});
    const FarPtr namePtr = a.Ptr(1);
    uint16_t bitmap = 0;
    if (a.Word(0) == 0) {
        rt.Print("LoadBitmap: system bitmaps (OBM_xxx) are not available yet");
    } else {
        const ResourceId name = ResourceId::FromFarPtr(rt.Mem(), namePtr.sel, namePtr.off);
        const NeResource* r = rt.Resource().Lookup(ResourceId{res::Bitmap, {}}, name);
        if (!r) {
            rt.Print("LoadBitmap: no bitmap resource " + name.Describe());
        } else if (!(bitmap = rt.Graphics().CreateBitmapFromDib(r->data.data(), r->data.size()))) {
            rt.Print("LoadBitmap: bitmap resource " + name.Describe() + " is not a valid DIB");
        }
    }
    cpu.Regs().r[AX] = bitmap;
    cpu.ReturnFar(a.Bytes());
}

void LoadString(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, UINT id, LPSTR buffer, int max) -> length
    const PascalArgs a(cpu, {2, 2, 4, 2});
    const FarPtr buffer = a.Ptr(2);
    const int max = a.Int(3);
    std::string s;
    uint16_t copied = 0;
    if (max > 0 && !buffer.IsNull()) {
        if (rt.Resource().String(a.Word(1), s)) {
            copied = uint16_t(std::min<size_t>(s.size(), size_t(max - 1)));
            for (uint16_t i = 0; i < copied; ++i)
                rt.Mem().Write8(buffer.sel, uint16_t(buffer.off + i), uint8_t(s[i]));
        }
        rt.Mem().Write8(buffer.sel, uint16_t(buffer.off + copied), 0);
    }
    cpu.Regs().r[AX] = copied;
    cpu.ReturnFar(a.Bytes());
}

void UpdateWindow(Runtime& rt, Cpu& cpu) {  // (HWND)
    const PascalArgs a(cpu, {2});
    cpu.Regs().r[AX] = rt.Windows().Update(a.Word(0)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

// RECT (Win16): left, top, right, bottom as ints.
Rect16 ReadRect(const Memory& mem, FarPtr p) {
    return {int16_t(mem.Read16(p.sel, p.off)), int16_t(mem.Read16(p.sel, uint16_t(p.off + 2))),
            int16_t(mem.Read16(p.sel, uint16_t(p.off + 4))), int16_t(mem.Read16(p.sel, uint16_t(p.off + 6)))};
}

void GetClientRect(Runtime& rt, Cpu& cpu) {  // (HWND, RECT FAR*)
    const PascalArgs a(cpu, {2, 4});
    const Rect16 r = rt.Windows().ClientRect(a.Word(0));
    const FarPtr p = a.Ptr(1);
    Memory& mem = rt.Mem();
    mem.Write16(p.sel, p.off, uint16_t(r.left));
    mem.Write16(p.sel, uint16_t(p.off + 2), uint16_t(r.top));
    mem.Write16(p.sel, uint16_t(p.off + 4), uint16_t(r.right));
    mem.Write16(p.sel, uint16_t(p.off + 6), uint16_t(r.bottom));
    cpu.ReturnFar(a.Bytes());
}

void BeginPaint(Runtime& rt, Cpu& cpu) {  // (HWND, PAINTSTRUCT FAR*) -> HDC
    const PascalArgs a(cpu, {2, 4});
    const FarPtr ps = a.Ptr(1);
    cpu.Regs().r[AX] = rt.Windows().BeginPaint(a.Word(0), ps.sel, ps.off);
    cpu.ReturnFar(a.Bytes());
}

void EndPaint(Runtime& rt, Cpu& cpu) {  // (HWND, const PAINTSTRUCT FAR*)
    const PascalArgs a(cpu, {2, 4});
    const FarPtr ps = a.Ptr(1);
    rt.Windows().EndPaint(a.Word(0), ps.sel, ps.off);
    cpu.ReturnFar(a.Bytes());
}

void GetDC(Runtime& rt, Cpu& cpu) {  // (HWND) -> HDC
    const PascalArgs a(cpu, {2});
    const uint16_t hwnd = a.Word(0);
    cpu.Regs().r[AX] = hwnd && !rt.Windows().Find(hwnd) ? 0 : rt.Graphics().GetWindowDc(hwnd);
    cpu.ReturnFar(a.Bytes());
}

void ReleaseDC(Runtime& rt, Cpu& cpu) {  // (HWND, HDC)
    const PascalArgs a(cpu, {2, 2});
    cpu.Regs().r[AX] = rt.Graphics().ReleaseWindowDc(a.Word(1)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void FillRect(Runtime& rt, Cpu& cpu) {  // (HDC, const RECT FAR*, HBRUSH)
    const PascalArgs a(cpu, {2, 4, 2});
    cpu.Regs().r[AX] = rt.Graphics().Fill(a.Word(0), ReadRect(rt.Mem(), a.Ptr(1)), a.Word(2)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void InvalidateRect(Runtime& rt, Cpu& cpu) {  // (HWND, const RECT FAR* or NULL, BOOL erase)
    const PascalArgs a(cpu, {2, 4, 2});
    const FarPtr p = a.Ptr(1);
    const Rect16 r = p.IsNull() ? Rect16{} : ReadRect(rt.Mem(), p);
    rt.Windows().Invalidate(a.Word(0), p.IsNull() ? nullptr : &r, a.Word(2) != 0);
    cpu.ReturnFar(a.Bytes());
}

void ValidateRect(Runtime& rt, Cpu& cpu) {  // (HWND, const RECT FAR* or NULL)
    const PascalArgs a(cpu, {2, 4});
    const FarPtr p = a.Ptr(1);
    const Rect16 r = p.IsNull() ? Rect16{} : ReadRect(rt.Mem(), p);
    rt.Windows().Validate(a.Word(0), p.IsNull() ? nullptr : &r);
    cpu.ReturnFar(a.Bytes());
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
        {10, "SETTIMER", SetTimer},
        {12, "KILLTIMER", KillTimer},
        {13, "GETTICKCOUNT", GetTickCount},
        {33, "GETCLIENTRECT", GetClientRect},
        {39, "BEGINPAINT", BeginPaint},
        {40, "ENDPAINT", EndPaint},
        {41, "CREATEWINDOW", CreateWindow},
        {42, "SHOWWINDOW", ShowWindow},
        {53, "DESTROYWINDOW", DestroyWindow},
        {57, "REGISTERCLASS", RegisterClass},
        {66, "GETDC", GetDC},
        {68, "RELEASEDC", ReleaseDC},
        {81, "FILLRECT", FillRect},
        {107, "DEFWINDOWPROC", DefWindowProc},
        {108, "GETMESSAGE", GetMessage},
        {109, "PEEKMESSAGE", PeekMessage},
        {110, "POSTMESSAGE", PostMessage},
        {111, "SENDMESSAGE", SendMessage},
        {113, "TRANSLATEMESSAGE", TranslateMessage},
        {114, "DISPATCHMESSAGE", DispatchMessage},
        {124, "UPDATEWINDOW", UpdateWindow},
        {125, "INVALIDATERECT", InvalidateRect},
        {127, "VALIDATERECT", ValidateRect},
        {173, "LOADCURSOR", LoadCursor},
        {174, "LOADICON", LoadIcon},
        {175, "LOADBITMAP", LoadBitmap},
        {176, "LOADSTRING", LoadString},
        {179, "GETSYSTEMMETRICS", GetSystemMetrics},
        {452, "CREATEWINDOWEX", CreateWindowEx},
    };
}

}  // namespace retro::win16
