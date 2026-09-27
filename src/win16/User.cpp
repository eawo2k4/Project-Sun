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

WindowHost::WindowInfo InfoFor(const User::Window& w) {
    WindowHost::WindowInfo info;
    info.hwnd16 = w.hwnd;
    info.title = w.title;
    info.x = w.x;
    info.y = w.y;
    info.width = w.cx;
    info.height = w.cy;
    info.fullscreen = CoversScreen({w.x, w.y, w.x + w.cx, w.y + w.cy}, {kScreenWidth, kScreenHeight});
    return info;
}

bool IsMouseMessage(uint16_t m) { return m >= wm::MouseMove && m <= wm::MButtonUp; }

}  // namespace

uint32_t ClassicSysColor(int index) {
    // Windows 3.1 "Windows Default" scheme, COLOR_SCROLLBAR .. COLOR_BTNHIGHLIGHT.
    static const uint32_t kColors[] = {
        0xC0C0C0, 0xC0C0C0, 0x800000, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x000000, 0x000000,
        0x000000, 0xFFFFFF, 0xC0C0C0, 0xC0C0C0, 0xFFFFFF, 0x800000, 0xFFFFFF, 0xC0C0C0,
        0x808080, 0x808080, 0x000000, 0x000000, 0xFFFFFF,
    };
    return index >= 0 && index < int(std::size(kColors)) ? kColors[index] : 0;
}

namespace {

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

void HeadlessHost::Update(uint64_t window, const WindowInfo& info) {
    for (Record& r : windows) {
        if (r.id == window) r.info = info;
    }
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

const User::WindowClass* User::FindClassAtom(uint16_t atom) const {
    for (const WindowClass& c : classes_) {
        if (c.atom == atom) return &c;
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

uint32_t User::AddHook(int16_t type, uint16_t sel, uint16_t off, uint16_t ds) {
    if (type != kCallWndProcHook) {
        rt_.Note("hooks:" + std::to_string(type),
                 "window hooks of type " + std::to_string(type) + " are accepted but never called yet");
    }
    const uint32_t handle = 0x48000000u | nextHook_++;
    hooks_.insert(hooks_.begin(), Hook{handle, type, sel, off, ds});
    return handle;
}

bool User::RemoveHook(uint32_t handle) {
    const auto it = std::find_if(hooks_.begin(), hooks_.end(), [&](const Hook& h) { return h.handle == handle; });
    if (it == hooks_.end()) return false;
    hooks_.erase(it);
    return true;
}

uint32_t User::FindHook(int16_t type, uint16_t sel, uint16_t off) const {
    for (const Hook& h : hooks_) {
        if (h.type == type && h.sel == sel && h.off == off) return h.handle;
    }
    return 0;
}

uint32_t User::FindHookByProc(uint16_t sel, uint16_t off) const {
    for (const Hook& h : hooks_) {
        if (h.sel == sel && h.off == off) return h.handle;
    }
    return 0;
}

uint32_t User::TopHookProc(int16_t type) const {
    for (const Hook& h : hooks_) {
        if (h.type == type) return (uint32_t(h.sel) << 16) | h.off;
    }
    return 0;
}

uint32_t User::CallHook(uint32_t handle, int16_t code, uint16_t wParam, uint32_t lParam) {
    const auto it = std::find_if(hooks_.begin(), hooks_.end(), [&](const Hook& h) { return h.handle == handle; });
    if (it == hooks_.end() || rt_.HasExited()) return 0;
    const Hook h = *it;  // the hook may unhook itself
    // (int code, WPARAM, LPARAM), Pascal; DS = the hook's instance data.
    return rt_.Processor().CallFar(h.sel, h.off,
                                   {uint16_t(code), wParam, uint16_t(lParam >> 16), uint16_t(lParam)}, h.ds);
}

uint32_t User::CallNextHook(uint32_t handle, int16_t code, uint16_t wParam, uint32_t lParam) {
    auto it = std::find_if(hooks_.begin(), hooks_.end(), [&](const Hook& h) { return h.handle == handle; });
    if (it == hooks_.end()) return 0;
    const int16_t type = it->type;
    it = std::find_if(it + 1, hooks_.end(), [&](const Hook& h) { return h.type == type; });
    return it == hooks_.end() ? 0 : CallHook(it->handle, code, wParam, lParam);
}

uint16_t User::RegisterMessageName(const std::string& name) {
    return name.empty() || name[0] == '#' ? 0 : atoms_.Add(name);
}

ScrollBar* User::Scroll(uint16_t hwnd, uint16_t bar) {
    Window* w = FindMutable(hwnd);
    return w && bar <= 2 ? &w->scroll[bar == 1 ? 1 : 0] : nullptr;
}

bool User::SetProperty(uint16_t hwnd, const std::string& key, uint16_t value) {
    Window* w = FindMutable(hwnd);
    if (!w || key.empty()) return false;
    w->props[key] = value;
    return true;
}

uint16_t User::Property(uint16_t hwnd, const std::string& key) const {
    const Window* w = Find(hwnd);
    if (!w) return 0;
    const auto it = w->props.find(key);
    return it == w->props.end() ? 0 : it->second;
}

uint16_t User::RemoveProperty(uint16_t hwnd, const std::string& key) {
    Window* w = FindMutable(hwnd);
    if (!w) return 0;
    const auto it = w->props.find(key);
    if (it == w->props.end()) return 0;
    const uint16_t value = it->second;
    w->props.erase(it);
    return value;
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

    // The window procedure is program code, or a built-in one (DefWindowProc itself, say).
    const Descriptor* proc = mem.Lookup(c.procSel);
    if (c.name.empty() || FindClass(c.name) || !proc || proc->kind == SegmentKind::Data) {
        char address[16];
        std::snprintf(address, sizeof(address), "%04X:%04X", c.procSel, c.procOff);
        rt_.Print("RegisterClass(\"" + c.name + "\") failed: " +
                  (c.name.empty() ? "no class name"
                   : FindClass(c.name) ? "class already registered"
                                       : std::string("window procedure ") + address + " is not in a code segment"));
        return 0;
    }
    c.extra.assign(std::min<uint16_t>(c.clsExtra, 1024), 0);
    c.atom = atoms_.Add(c.name);
    classes_.push_back(c);
    return c.atom;
}

uint16_t User::Create(const CreateParams& p) {
    Memory& mem = rt_.Mem();
    const WindowClass* cls = nullptr;
    std::string className;
    if (p.classSel == 0) {
        cls = FindClassAtom(p.classOff);
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
    w.extra.assign(std::min<uint16_t>(cls->wndExtra, 1024), 0);
    w.enabled = !(p.style & ws::Disabled);
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
        win->host = host_->Create(InfoFor(*win));
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
    if (focus_ == hwnd) focus_ = 0;
    if (active_ == hwnd) active_ = 0;
    if (capture_ == hwnd) SetCaptureTo(0);
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
        // The task's first visible top-level window becomes the active one
        // (WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS), as when Windows starts a program.
        const Window* shown = Find(hwnd);
        if (show && shown && shown->host && !active_) Activate(hwnd);
        if (!show && active_ == hwnd) active_ = 0;
        if (show) Invalidate(hwnd, nullptr, true);  // newly visible: paint it all
    }
    return previous;
}

uint32_t User::Send(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam) {
    if (!Find(hwnd) || rt_.HasExited()) return 0;
    const auto hook = std::find_if(hooks_.begin(), hooks_.end(),
                                   [](const Hook& h) { return h.type == kCallWndProcHook; });
    if (hook != hooks_.end()) {
        // lParam -> CWPSTRUCT {lParam, wParam, message, hwnd}; wParam: sent by this task.
        Runtime::Scratch cwp(rt_, 10);
        const uint16_t s = cwp.Selector(), o = cwp.Offset();
        Memory& mem = rt_.Mem();
        mem.Write16(s, o, uint16_t(lParam));
        mem.Write16(s, uint16_t(o + 2), uint16_t(lParam >> 16));
        mem.Write16(s, uint16_t(o + 4), wParam);
        mem.Write16(s, uint16_t(o + 6), msg);
        mem.Write16(s, uint16_t(o + 8), hwnd);
        CallHook(hook->handle, 0 /* HC_ACTION */, 1, (uint32_t(s) << 16) | o);
    }
    // The hook may have subclassed (or destroyed) the window.
    return Deliver(hwnd, msg, wParam, lParam);
}

uint32_t User::Deliver(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParam) {
    const Window* w = Find(hwnd);
    if (!w) return 0;
    if (!w->procSel) return DefProc(hwnd, msg, wParam, lParam);
    if (rt_.HasExited()) return 0;  // the task has ended: no more calls into it
    // The window procedure runs on the interpreter; Pascal arguments
    // (hwnd, msg, wParam, lParam), DS = its module's data.
    return rt_.Processor().CallFar(w->procSel, w->procOff,
                                   {hwnd, msg, wParam, uint16_t(lParam >> 16), uint16_t(lParam)},
                                   rt_.CallbackData(w->procSel, w->hInstance));
}

uint32_t User::DefProc(uint16_t hwnd, uint16_t msg, uint16_t wParam, uint32_t lParamFull) {
    auto lParamLo = [](uint32_t l) { return uint16_t(l); };
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
    case 0x0020: {  // WM_SETCURSOR over the client area: the class cursor
        const Window* w = Find(hwnd);
        const WindowClass* c = w ? FindClass(w->className) : nullptr;
        if ((lParamLo(lParamFull) == 1) && c && c->hCursor) {
            SetCursorHandle(c->hCursor);
            return 1;
        }
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
    if (hwnd == kDesktopHwnd) return {0, 0, kScreenWidth, kScreenHeight};
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
            if (!Find(hwnd16)) return;
            TrackInput(hwnd16, message, wParam, lParam);
            // Mouse input goes to the window that has captured it.
            Post(capture_ && IsMouseMessage(message) ? capture_ : hwnd16, message, wParam, lParam);
        },
        until);
}

void User::TrackInput(uint16_t hwnd, uint16_t message, uint16_t wParam, uint32_t lParam) {
    const uint8_t vk = uint8_t(wParam);
    switch (message) {
    case wm::KeyDown:
    case wm::SysKeyDown:
        if (!(keys_[vk] & 0x80)) keys_[vk] ^= 1;  // toggles on each press
        keys_[vk] |= 0x80;
        return;
    case wm::KeyUp:
    case wm::SysKeyUp:
        keys_[vk] &= ~0x80;
        return;
    default:
        break;
    }
    if (!IsMouseMessage(message)) return;
    const Rect16 r = WindowRect(hwnd);
    mouseX_ = int16_t(r.left + int16_t(lParam));
    mouseY_ = int16_t(r.top + int16_t(lParam >> 16));
    // VK_LBUTTON, VK_RBUTTON, VK_MBUTTON from the MK_ flags every mouse message carries.
    keys_[1] = uint8_t((keys_[1] & 1) | ((wParam & 0x01) ? 0x80 : 0));
    keys_[2] = uint8_t((keys_[2] & 1) | ((wParam & 0x02) ? 0x80 : 0));
    keys_[4] = uint8_t((keys_[4] & 1) | ((wParam & 0x10) ? 0x80 : 0));
}

int16_t User::KeyState(uint8_t vk) const {
    return int16_t(((keys_[vk] & 0x80) ? 0x8000 : 0) | (keys_[vk] & 1));
}

std::vector<uint16_t> User::Handles() const {
    std::vector<uint16_t> handles;
    for (const auto& [h, w] : windows_) handles.push_back(h);  // handles only grow
    return handles;
}

User::WindowClass* User::EditClass(const std::string& name) {
    for (WindowClass& c : classes_) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

Rect16 User::WindowRect(uint16_t hwnd) const {
    if (hwnd == kDesktopHwnd) return {0, 0, kScreenWidth, kScreenHeight};
    const Window* w = Find(hwnd);
    if (!w) return {};
    int16_t x = w->x, y = w->y;
    for (const Window* p = Find(w->parent); p && (w->style & ws::Child); p = Find(p->parent)) {
        x = int16_t(x + p->x);
        y = int16_t(y + p->y);
        if (!(p->style & ws::Child)) break;
    }
    return {x, y, int16_t(x + w->cx), int16_t(y + w->cy)};
}

bool User::Reposition(uint16_t hwnd, int16_t x, int16_t y, int16_t cx, int16_t cy, uint16_t flags) {
    Window* w = FindMutable(hwnd);
    if (!w) return false;
    if (flags & swp::HideWindow) Show(hwnd, 0);
    if (!(w = FindMutable(hwnd))) return false;
    const int16_t nx = (flags & swp::NoMove) ? w->x : x, ny = (flags & swp::NoMove) ? w->y : y;
    const int16_t ncx = (flags & swp::NoSize) ? w->cx : std::max<int16_t>(cx, 0);
    const int16_t ncy = (flags & swp::NoSize) ? w->cy : std::max<int16_t>(cy, 0);
    const bool moved = nx != w->x || ny != w->y, sized = ncx != w->cx || ncy != w->cy;
    w->x = nx;
    w->y = ny;
    w->cx = ncx;
    w->cy = ncy;
    if (sized && w->host) rt_.Graphics().ResizeSurface(hwnd, ncx, ncy);
    if ((moved || sized) && w->host) host_->Update(w->host, InfoFor(*w));
    if (sized) Send(hwnd, wm::Size, 0, MakeLong(ncx, ncy));
    if (moved && Find(hwnd)) Send(hwnd, wm::Move, 0, MakeLong(nx, ny));
    if (sized && !(flags & swp::NoRedraw) && Find(hwnd)) Invalidate(hwnd, nullptr, true);
    if ((flags & swp::ShowWindow) && Find(hwnd)) Show(hwnd, 5 /* SW_SHOW */);
    return Find(hwnd) != nullptr;
}

bool User::SetText(uint16_t hwnd, const std::string& text) {
    Window* w = FindMutable(hwnd);
    if (!w) return false;
    w->title = text;
    if (w->host) host_->Update(w->host, InfoFor(*w));
    return true;
}

bool User::Enable(uint16_t hwnd, bool enable) {
    Window* w = FindMutable(hwnd);
    if (!w) return false;
    const bool wasDisabled = !w->enabled;
    if (w->enabled != enable) {
        w->enabled = enable;
        Send(hwnd, wm::Enable, enable ? 1 : 0, 0);
    }
    return wasDisabled;
}

uint16_t User::SetFocusTo(uint16_t hwnd) {
    if (hwnd && !Find(hwnd)) return 0;
    const uint16_t previous = focus_;
    if (previous == hwnd) return previous;
    focus_ = hwnd;
    if (previous && Find(previous)) Send(previous, wm::KillFocus, hwnd, 0);
    if (hwnd && Find(hwnd)) Send(hwnd, wm::SetFocus, previous, 0);
    return previous;
}

uint16_t User::Activate(uint16_t hwnd) {
    if (hwnd && !Find(hwnd)) return 0;
    const uint16_t previous = active_;
    if (previous == hwnd) return previous;
    if (!previous && hwnd) Send(hwnd, wm::ActivateApp, 1, 0);  // the task comes to the front
    active_ = hwnd;
    if (previous && Find(previous)) Send(previous, wm::Activate, 0 /* WA_INACTIVE */, hwnd);
    if (hwnd && Find(hwnd)) {
        Send(hwnd, wm::Activate, 1 /* WA_ACTIVE */, previous);
        // What DefWindowProc does with WM_ACTIVATE: focus the window, unless
        // the window procedure already focused one of its children.
        bool focusedInside = false;
        for (const Window* f = Find(focus_); f; f = (f->style & ws::Child) ? Find(f->parent) : nullptr) {
            if (f->hwnd == hwnd) focusedInside = true;
        }
        if (!focusedInside && Find(hwnd)) SetFocusTo(hwnd);
    }
    return previous;
}

uint16_t User::SetCaptureTo(uint16_t hwnd) {
    const uint16_t previous = capture_;
    if (hwnd && !Find(hwnd)) return previous;
    auto topHost = [&](uint16_t h) {
        const Window* w = Find(h);
        while (w && (w->style & ws::Child)) w = Find(w->parent);
        return w ? w->host : 0;
    };
    if (previous && previous != hwnd) {
        if (const uint64_t host = topHost(previous)) host_->Capture(host, false);
    }
    capture_ = hwnd;
    if (hwnd) {
        if (const uint64_t host = topHost(hwnd)) host_->Capture(host, true);
    }
    return previous;
}

uint16_t User::CursorHandle(uint16_t shape) {
    for (const auto& [h, s] : cursors_) {
        if (s == shape) return h;
    }
    const uint16_t h = uint16_t(0x0E10 + 2 * cursors_.size());
    cursors_[h] = shape;
    return h;
}

uint16_t User::SetCursorHandle(uint16_t handle) {
    const uint16_t previous = cursor_;
    cursor_ = handle;
    cursorSet_ = true;
    ApplyCursor();
    return previous;
}

int User::ShowCursorCount(bool show) {
    cursorCount_ += show ? 1 : -1;
    ApplyCursor();
    return cursorCount_;
}

void User::ApplyCursor() {
    uint16_t shape = kArrowCursor;
    if (cursorCount_ < 0 || (cursorSet_ && cursor_ == 0)) {
        shape = 0;  // hidden: SetCursor(NULL), or ShowCursor(FALSE) more than TRUE
    } else if (const auto it = cursors_.find(cursor_); it != cursors_.end()) {
        shape = it->second;
    }
    host_->SetCursorShape(shape);
}

uint16_t User::ShowMessageBox(uint16_t owner, const std::string& caption, const std::string& text,
                              uint16_t type) {
    const Window* w = Find(owner ? owner : active_);
    while (w && (w->style & ws::Child)) w = Find(w->parent);
    const int pressed = host_->ShowMessage(w ? w->host : 0, caption, text, type);
    if (pressed > 0) return uint16_t(pressed);
    // No real box: the default button (MB_DEFBUTTONn) of the MB_xxx button set.
    static const std::vector<uint16_t> kButtons[] = {
        {1}, {1, 2}, {3, 4, 5}, {6, 7, 2}, {6, 7}, {4, 2},  // OK, OKCANCEL, ARI, YNC, YN, RC
    };
    const std::vector<uint16_t>& set = kButtons[std::min<size_t>(type & 0x0F, 5)];
    return set[std::min<size_t>((type >> 8) & 0x0F, set.size() - 1)];
}

uint16_t User::StartMultimediaTimer(uint16_t delayMs, uint16_t procSel, uint16_t procOff, uint32_t user,
                                    bool periodic) {
    const Descriptor* d = rt_.Mem().Lookup(procSel);
    if (!d || d->kind != SegmentKind::Code) return 0;
    uint16_t id = 0;
    do {
        id = nextMmTimerId_++;
    } while (id == 0 || std::any_of(timers_.begin(), timers_.end(),
                                    [&](const Timer& t) { return t.multimedia && t.id == id; }));
    Timer t;
    t.id = id;
    t.procSel = procSel;
    t.procOff = procOff;
    t.intervalMs = std::max<uint32_t>(delayMs, 1);
    t.due = QpcNow() + MsToQpc(t.intervalMs);
    t.multimedia = true;
    t.periodic = periodic;
    t.user = user;
    timers_.push_back(t);
    return id;
}

bool User::StopMultimediaTimer(uint16_t id) {
    const auto it = std::find_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.multimedia && t.id == id; });
    if (it == timers_.end()) return false;
    timers_.erase(it);
    return true;
}

bool User::RunMultimediaTimers(int64_t now) {
    std::vector<uint16_t> due;
    for (const Timer& t : timers_) {
        if (t.multimedia && t.due <= now) due.push_back(t.id);
    }
    for (uint16_t id : due) {
        const auto it = std::find_if(timers_.begin(), timers_.end(),
                                     [&](const Timer& t) { return t.multimedia && t.id == id; });
        if (it == timers_.end()) continue;  // killed by an earlier callback
        const Timer t = *it;
        if (t.periodic) {
            const int64_t period = MsToQpc(t.intervalMs);
            it->due += period;
            if (it->due <= now) it->due = now + period;
        } else {
            timers_.erase(it);
        }
        // void CALLBACK TimeProc(UINT id, UINT msg, DWORD dwUser, DWORD dw1, DWORD dw2)
        rt_.Processor().CallFar(t.procSel, t.procOff,
                                {t.id, 0, uint16_t(t.user >> 16), uint16_t(t.user), 0, 0, 0, 0},
                                rt_.Module().dgroup);
        if (rt_.HasExited()) return true;
    }
    return false;
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
                            [&](const Timer& t) { return !t.multimedia && t.hwnd == h && t.id == i; });
    };
    if (!hwnd) {  // the id argument is ignored: every such timer gets a new one
        do {
            id = nextTimerId_++;
        } while (id == 0 || find(0, id) != timers_.end());
    }
    const uint32_t interval = std::max<uint32_t>(elapseMs, minTimerMs_);
    Timer t;
    t.hwnd = hwnd;
    t.id = id;
    t.procSel = procSel;
    t.procOff = procOff;
    t.intervalMs = interval;
    t.due = QpcNow() + MsToQpc(interval);
    if (const auto it = find(hwnd, id); it != timers_.end()) {
        *it = t;
    } else {
        timers_.push_back(t);
    }
    return id ? id : 1;
}

bool User::StopTimer(uint16_t hwnd, uint16_t id) {
    const auto it = std::find_if(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return !t.multimedia && t.hwnd == hwnd && t.id == id; });
    if (it == timers_.end()) return false;
    timers_.erase(it);
    return true;
}

bool User::IsTimerProc(uint16_t sel, uint16_t off) const {
    return (sel || off) && std::any_of(timers_.begin(), timers_.end(), [&](const Timer& t) {
               return !t.multimedia && t.procSel == sel && t.procOff == off;
           });
}

User::Timer* User::DueTimer(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg, int64_t now) {
    Timer* due = nullptr;
    for (Timer& t : timers_) {
        if (!t.multimedia && t.due <= now && Matches(Msg16{t.hwnd, wm::Timer}, hwnd, minMsg, maxMsg) &&
            (!due || t.due < due->due))
            due = &t;
    }
    return due;
}

int64_t User::NextTimerDue(uint16_t hwnd, uint16_t minMsg, uint16_t maxMsg) const {
    int64_t next = WindowHost::kForever;
    for (const Timer& t : timers_) {
        // Multimedia timers wake the pump whatever the filter: their callbacks run there.
        if (t.multimedia || Matches(Msg16{t.hwnd, wm::Timer}, hwnd, minMsg, maxMsg)) next = std::min(next, t.due);
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
        if (RunMultimediaTimers(QpcNow())) return Fetch::Empty;  // a callback ended the task
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (Matches(*it, hwndFilter, minMsg, maxMsg)) {
                out = *it;
                if (remove) {
                    queue_.erase(it);
                    // Before a mouse message is processed, the window gets to set
                    // the cursor (WM_SETCURSOR, HTCLIENT); DefWindowProc uses the class cursor.
                    if (IsMouseMessage(out.message) && out.hwnd && Find(out.hwnd))
                        Send(out.hwnd, 0x0020, out.hwnd, MakeLong(1, int16_t(out.message)));
                }
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

void MessageBox(Runtime& rt, Cpu& cpu) {  // (hwnd, text, caption, type) -> button
    const PascalArgs a(cpu, {2, 4, 4, 2});
    const FarPtr text = a.Ptr(1), caption = a.Ptr(2);
    const std::string c = caption.IsNull() ? "Error" : rt.Mem().ReadString(caption.sel, caption.off);
    const std::string t = text.IsNull() ? "" : rt.Mem().ReadString(text.sel, text.off);
    rt.Print("MessageBox [" + c + "]: " + t);
    cpu.Regs().r[AX] = rt.Windows().ShowMessageBox(a.Word(0), c, t, a.Word(3));
    cpu.ReturnFar(a.Bytes());
}

void InitApp(Runtime&, Cpu& cpu) {
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(2);
}

void RegisterWindowMessage(Runtime& rt, Cpu& cpu) {  // (LPCSTR) -> message number, 0 on failure
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    cpu.Regs().r[AX] = p.IsNull() ? 0 : rt.Windows().RegisterMessageName(rt.Mem().ReadString(p.sel, p.off));
    cpu.ReturnFar(a.Bytes());
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

// WaitMessage: blocks until a message is waiting (it stays in the queue).
void WaitMessage(Runtime& rt, Cpu& cpu) {
    Msg16 m;
    if (rt.Windows().Next(m, 0, 0, 0, false, true) == User::Fetch::NoInput) {
        rt.Exit(TaskExit::Kind::Blocked, 0, "WaitMessage: the queue is empty and no input can ever arrive");
        return;
    }
    cpu.ReturnFar(0);
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
        result = rt.Windows().Deliver(m.hwnd, m.message, m.wParam, m.lParam);
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
        const NeResource* r = rt.ResourcesFor(a.Word(0)).Lookup(ResourceId{res::Bitmap, {}}, name);
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
        if (rt.ResourcesFor(a.Word(0)).String(a.Word(1), s)) {
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
    const uint16_t hwnd = a.Word(0) == kDesktopHwnd ? 0 : a.Word(0);
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

void LoadCursor(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR name) -> HCURSOR
    const PascalArgs a(cpu, {2, 4});
    const FarPtr name = a.Ptr(1);
    uint16_t shape = kArrowCursor;
    if (a.Word(0) == 0 && name.sel == 0) {
        shape = name.off;  // IDC_xxx
    } else {
        rt.Note("cursor-resource", "cursors from a program's resources show as the arrow for now");
    }
    cpu.Regs().r[AX] = rt.Windows().CursorHandle(shape);
    cpu.ReturnFar(a.Bytes());
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
    std::vector<ApiFunction> api = {
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
        {112, "WAITMESSAGE", WaitMessage},
        {113, "TRANSLATEMESSAGE", TranslateMessage},
        {114, "DISPATCHMESSAGE", DispatchMessage},
        {118, "REGISTERWINDOWMESSAGE", RegisterWindowMessage},
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
    for (auto part : {UserWindowApi(), MenuApi(), UserDrawApi(), UserSoundApi(), UserAtomApi(), UserCharsetApi()})
        api.insert(api.end(), part.begin(), part.end());
    return api;
}

}  // namespace retro::win16
