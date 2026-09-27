// USER: window geometry, window/class words, focus and activation, window
// text, rectangles, input state, the cursor, system colours, wsprintf.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "win16/Api.h"
#include "win16/Runtime.h"

namespace retro::win16 {
namespace {

// --- Structures in 16-bit memory ---

Rect16 ReadRect(const Memory& mem, FarPtr p) {
    return {int16_t(mem.Read16(p.sel, p.off)), int16_t(mem.Read16(p.sel, uint16_t(p.off + 2))),
            int16_t(mem.Read16(p.sel, uint16_t(p.off + 4))), int16_t(mem.Read16(p.sel, uint16_t(p.off + 6)))};
}

void WriteRect(Memory& mem, FarPtr p, const Rect16& r) {
    mem.Write16(p.sel, p.off, uint16_t(r.left));
    mem.Write16(p.sel, uint16_t(p.off + 2), uint16_t(r.top));
    mem.Write16(p.sel, uint16_t(p.off + 4), uint16_t(r.right));
    mem.Write16(p.sel, uint16_t(p.off + 6), uint16_t(r.bottom));
}

uint16_t CopyString(Runtime& rt, FarPtr dst, const std::string& s, int size) {
    if (dst.IsNull() || size <= 0) return 0;
    const uint16_t n = uint16_t(std::min<size_t>(s.size(), size_t(size - 1)));
    for (uint16_t i = 0; i < n; ++i) rt.Mem().Write8(dst.sel, uint16_t(dst.off + i), uint8_t(s[i]));
    rt.Mem().Write8(dst.sel, uint16_t(dst.off + n), 0);
    return n;
}

void Return(Cpu& cpu, const PascalArgs& a, uint32_t value) {
    SetResult(cpu, value);
    cpu.ReturnFar(a.Bytes());
}

// --- Geometry ---

void GetWindowRect(Runtime& rt, Cpu& cpu) {  // (HWND, RECT FAR*)
    const PascalArgs a(cpu, {2, 4});
    WriteRect(rt.Mem(), a.Ptr(1), rt.Windows().WindowRect(a.Word(0)));
    cpu.ReturnFar(a.Bytes());
}

void SetWindowPos(Runtime& rt, Cpu& cpu) {  // (HWND, HWND after, x, y, cx, cy, flags) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2, 2});
    Return(cpu, a, rt.Windows().Reposition(a.Word(0), a.Int(2), a.Int(3), a.Int(4), a.Int(5), a.Word(6)) ? 1 : 0);
}

void MoveWindow(Runtime& rt, Cpu& cpu) {  // (HWND, x, y, cx, cy, BOOL repaint) -> BOOL
    const PascalArgs a(cpu, {2, 2, 2, 2, 2, 2});
    const uint16_t flags = uint16_t(swp::NoZOrder | (a.Word(5) ? 0 : swp::NoRedraw));
    Return(cpu, a, rt.Windows().Reposition(a.Word(0), a.Int(1), a.Int(2), a.Int(3), a.Int(4), flags) ? 1 : 0);
}

void ClientToScreen(Runtime& rt, Cpu& cpu) {  // (HWND, POINT FAR*)
    const PascalArgs a(cpu, {2, 4});
    const FarPtr p = a.Ptr(1);
    const Rect16 r = rt.Windows().WindowRect(a.Word(0));
    rt.Mem().Write16(p.sel, p.off, uint16_t(int16_t(rt.Mem().Read16(p.sel, p.off)) + r.left));
    rt.Mem().Write16(p.sel, uint16_t(p.off + 2), uint16_t(int16_t(rt.Mem().Read16(p.sel, uint16_t(p.off + 2))) + r.top));
    cpu.ReturnFar(a.Bytes());
}

void ScreenToClient(Runtime& rt, Cpu& cpu) {  // (HWND, POINT FAR*)
    const PascalArgs a(cpu, {2, 4});
    const FarPtr p = a.Ptr(1);
    const Rect16 r = rt.Windows().WindowRect(a.Word(0));
    rt.Mem().Write16(p.sel, p.off, uint16_t(int16_t(rt.Mem().Read16(p.sel, p.off)) - r.left));
    rt.Mem().Write16(p.sel, uint16_t(p.off + 2), uint16_t(int16_t(rt.Mem().Read16(p.sel, uint16_t(p.off + 2))) - r.top));
    cpu.ReturnFar(a.Bytes());
}

// --- Window and class words ---

constexpr int16_t GWW_HINSTANCE = -6, GWW_HWNDPARENT = -8, GWW_ID = -12, GWL_WNDPROC = -4,
                  GWL_STYLE = -16, GWL_EXSTYLE = -20;
constexpr int16_t GCW_HBRBACKGROUND = -10, GCW_HCURSOR = -12, GCW_HICON = -14, GCW_HMODULE = -16,
                  GCW_CBWNDEXTRA = -18, GCW_CBCLSEXTRA = -20, GCL_WNDPROC = -24, GCW_STYLE = -26;

// A word/long of extra bytes; false if outside them.
bool ExtraGet(const std::vector<uint8_t>& extra, int16_t index, int bytes, uint32_t& out) {
    if (index < 0 || size_t(index) + size_t(bytes) > extra.size()) return false;
    out = 0;
    for (int i = bytes - 1; i >= 0; --i) out = (out << 8) | extra[size_t(index) + size_t(i)];
    return true;
}

bool ExtraSet(std::vector<uint8_t>& extra, int16_t index, int bytes, uint32_t value) {
    if (index < 0 || size_t(index) + size_t(bytes) > extra.size()) return false;
    for (int i = 0; i < bytes; ++i) extra[size_t(index) + size_t(i)] = uint8_t(value >> (8 * i));
    return true;
}

void GetWindowWord(Runtime& rt, Cpu& cpu) {  // (HWND, int index) -> WORD
    const PascalArgs a(cpu, {2, 2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    uint32_t v = 0;
    if (w) {
        switch (a.Int(1)) {
        case GWW_HINSTANCE: v = w->hInstance; break;
        case GWW_HWNDPARENT: v = w->parent; break;
        case GWW_ID: v = w->menu; break;
        default: ExtraGet(w->extra, a.Int(1), 2, v); break;
        }
    }
    Return(cpu, a, v);
}

void SetWindowWord(Runtime& rt, Cpu& cpu) {  // (HWND, int index, WORD) -> previous
    const PascalArgs a(cpu, {2, 2, 2});
    User::Window* w = rt.Windows().Edit(a.Word(0));
    uint32_t previous = 0;
    if (w) {
        switch (a.Int(1)) {
        case GWW_HINSTANCE: previous = w->hInstance; w->hInstance = a.Word(2); break;
        case GWW_HWNDPARENT: previous = w->parent; w->parent = a.Word(2); break;
        case GWW_ID: previous = w->menu; w->menu = a.Word(2); break;
        default:
            if (ExtraGet(w->extra, a.Int(1), 2, previous)) ExtraSet(w->extra, a.Int(1), 2, a.Word(2));
            break;
        }
    }
    Return(cpu, a, previous);
}

void GetWindowLong(Runtime& rt, Cpu& cpu) {  // (HWND, int index) -> LONG
    const PascalArgs a(cpu, {2, 2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    uint32_t v = 0;
    if (w) {
        switch (a.Int(1)) {
        case GWL_WNDPROC: v = (uint32_t(w->procSel) << 16) | w->procOff; break;
        case GWL_STYLE: v = w->style | (w->visible ? ws::Visible : 0) | (w->enabled ? 0 : ws::Disabled); break;
        case GWL_EXSTYLE: v = w->exStyle; break;
        default: ExtraGet(w->extra, a.Int(1), 4, v); break;
        }
    }
    Return(cpu, a, v);
}

void SetWindowLong(Runtime& rt, Cpu& cpu) {  // (HWND, int index, LONG) -> previous
    const PascalArgs a(cpu, {2, 2, 4});
    User::Window* w = rt.Windows().Edit(a.Word(0));
    uint32_t previous = 0;
    const uint32_t value = a.Long(2);
    if (w) {
        switch (a.Int(1)) {
        case GWL_WNDPROC:  // subclassing: messages now go to the new procedure
            previous = (uint32_t(w->procSel) << 16) | w->procOff;
            w->procSel = uint16_t(value >> 16);
            w->procOff = uint16_t(value);
            break;
        case GWL_STYLE:
            previous = w->style;
            w->style = value & ~(ws::Visible | ws::Disabled);  // those follow ShowWindow/EnableWindow
            break;
        case GWL_EXSTYLE: previous = w->exStyle; w->exStyle = value; break;
        default:
            if (ExtraGet(w->extra, a.Int(1), 4, previous)) ExtraSet(w->extra, a.Int(1), 4, value);
            break;
        }
    }
    Return(cpu, a, previous);
}

User::WindowClass* ClassOf(Runtime& rt, uint16_t hwnd) {
    const User::Window* w = rt.Windows().Find(hwnd);
    return w ? rt.Windows().EditClass(w->className) : nullptr;
}

void GetClassWord(Runtime& rt, Cpu& cpu) {  // (HWND, int index) -> WORD
    const PascalArgs a(cpu, {2, 2});
    const User::WindowClass* c = ClassOf(rt, a.Word(0));
    uint32_t v = 0;
    if (c) {
        switch (a.Int(1)) {
        case GCW_HBRBACKGROUND: v = c->hbrBackground; break;
        case GCW_HCURSOR: v = c->hCursor; break;
        case GCW_HICON: v = c->hIcon; break;
        case GCW_HMODULE: v = c->hInstance; break;
        case GCW_CBWNDEXTRA: v = c->wndExtra; break;
        case GCW_CBCLSEXTRA: v = c->clsExtra; break;
        case GCW_STYLE: v = c->style; break;
        default: ExtraGet(c->extra, a.Int(1), 2, v); break;
        }
    }
    Return(cpu, a, v);
}

void SetClassWord(Runtime& rt, Cpu& cpu) {  // (HWND, int index, WORD) -> previous
    const PascalArgs a(cpu, {2, 2, 2});
    User::WindowClass* c = ClassOf(rt, a.Word(0));
    uint32_t previous = 0;
    const uint16_t v = a.Word(2);
    if (c) {
        switch (a.Int(1)) {
        case GCW_HBRBACKGROUND: previous = c->hbrBackground; c->hbrBackground = v; break;
        case GCW_HCURSOR: previous = c->hCursor; c->hCursor = v; break;
        case GCW_HICON: previous = c->hIcon; c->hIcon = v; break;
        case GCW_STYLE: previous = c->style; c->style = v; break;
        default:
            if (ExtraGet(c->extra, a.Int(1), 2, previous)) ExtraSet(c->extra, a.Int(1), 2, v);
            break;
        }
    }
    Return(cpu, a, previous);
}

void GetClassLong(Runtime& rt, Cpu& cpu) {  // (HWND, int index) -> LONG
    const PascalArgs a(cpu, {2, 2});
    const User::WindowClass* c = ClassOf(rt, a.Word(0));
    uint32_t v = 0;
    if (c) {
        if (a.Int(1) == GCL_WNDPROC) {
            v = (uint32_t(c->procSel) << 16) | c->procOff;
        } else {
            ExtraGet(c->extra, a.Int(1), 4, v);
        }
    }
    Return(cpu, a, v);
}

// --- Window state ---

void IsWindow(Runtime& rt, Cpu& cpu) {  // (HWND) -> BOOL
    const PascalArgs a(cpu, {2});
    Return(cpu, a, a.Word(0) == kDesktopHwnd || rt.Windows().Find(a.Word(0)) ? 1 : 0);
}

void IsWindowVisible(Runtime& rt, Cpu& cpu) {  // (HWND) -> BOOL: it and its parents are visible
    const PascalArgs a(cpu, {2});
    bool visible = a.Word(0) == kDesktopHwnd;
    for (const User::Window* w = rt.Windows().Find(a.Word(0)); w;
         w = (w->style & ws::Child) ? rt.Windows().Find(w->parent) : nullptr) {
        visible = w->visible;
        if (!visible) break;
    }
    Return(cpu, a, visible ? 1 : 0);
}

void ReturnFalse1(Runtime&, Cpu& cpu) {  // (HWND) -> FALSE: IsIconic, IsZoomed
    const PascalArgs a(cpu, {2});
    Return(cpu, a, 0);
}

void GetParent(Runtime& rt, Cpu& cpu) {  // (HWND) -> HWND
    const PascalArgs a(cpu, {2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    Return(cpu, a, w ? w->parent : 0);
}

void GetWindow(Runtime& rt, Cpu& cpu) {  // (HWND, GW_xxx) -> HWND
    const PascalArgs a(cpu, {2, 2});
    const User& u = rt.Windows();
    const uint16_t hwnd = a.Word(0), cmd = a.Word(1);
    const User::Window* w = u.Find(hwnd);
    uint16_t result = 0;
    if (w) {
        // Siblings (same parent) and children, in creation order.
        std::vector<uint16_t> siblings, children;
        for (uint16_t h : u.Handles()) {
            const User::Window* x = u.Find(h);
            if (x->parent == w->parent) siblings.push_back(h);
            if (x->parent == hwnd && (x->style & ws::Child)) children.push_back(h);
        }
        const auto me = std::find(siblings.begin(), siblings.end(), hwnd);
        switch (cmd) {
        case 0: result = siblings.front(); break;                                    // GW_HWNDFIRST
        case 1: result = siblings.back(); break;                                     // GW_HWNDLAST
        case 2: result = me + 1 < siblings.end() ? *(me + 1) : 0; break;             // GW_HWNDNEXT
        case 3: result = me > siblings.begin() ? *(me - 1) : 0; break;               // GW_HWNDPREV
        case 4: result = (w->style & ws::Child) ? 0 : w->parent; break;              // GW_OWNER
        case 5: result = children.empty() ? 0 : children.front(); break;             // GW_CHILD
        default: break;
        }
    }
    Return(cpu, a, result);
}

bool Contains(const Rect16& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

bool IsChildOf(const User::Window& w, uint16_t parent) {
    return parent ? (w.parent == parent && (w.style & ws::Child)) : !(w.style & ws::Child);
}

// The deepest visible window at screen point (x, y) under `parent` (0: the
// top-level ones), the most recently created first.
uint16_t WindowAt(const User& u, uint16_t parent, int x, int y) {
    const std::vector<uint16_t> handles = u.Handles();
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const User::Window* w = u.Find(*it);
        if (!IsChildOf(*w, parent) || !w->visible || !Contains(u.WindowRect(*it), x, y)) continue;
        const uint16_t child = WindowAt(u, *it, x, y);
        return child ? child : *it;
    }
    return 0;
}

void WindowFromPoint(Runtime& rt, Cpu& cpu) {  // (POINT, screen) -> HWND or NULL
    const uint32_t pt = ArgLong(cpu, 0);
    cpu.Regs().r[AX] = WindowAt(rt.Windows(), 0, int16_t(pt), int16_t(pt >> 16));
    cpu.ReturnFar(4);
}

// ChildWindowFromPoint: the parent's child (any, hidden ones too) at a point in
// the parent's client coordinates; the parent itself if no child is there;
// NULL outside the parent.
void ChildWindowFromPoint(Runtime& rt, Cpu& cpu) {  // (HWND parent, POINT) -> HWND
    const PascalArgs a(cpu, {2, 4});
    const User& u = rt.Windows();
    const uint16_t parent = a.Word(0);
    const Rect16 area = u.WindowRect(parent);
    const int x = area.left + int16_t(a.Long(1)), y = area.top + int16_t(a.Long(1) >> 16);
    uint16_t result = 0;
    if (u.Find(parent) && Contains(area, x, y)) {
        result = parent;
        const std::vector<uint16_t> handles = u.Handles();
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            if (IsChildOf(*u.Find(*it), parent) && parent && Contains(u.WindowRect(*it), x, y)) {
                result = *it;
                break;
            }
        }
    }
    cpu.Regs().r[AX] = result;
    cpu.ReturnFar(a.Bytes());
}

// EnumWindows / EnumTaskWindows (top-level windows: all the task's) and
// EnumChildWindows (all descendants), in creation order. The callback,
// BOOL (HWND, LPARAM), returns FALSE to stop.
void EnumerateWindows(Runtime& rt, Cpu& cpu, const PascalArgs& a, size_t procArg, uint16_t parent) {
    const FarPtr proc = a.Ptr(procArg);
    const uint32_t lParam = a.Long(procArg + 1);
    User& u = rt.Windows();
    auto isDescendant = [&](uint16_t h) {
        for (const User::Window* w = u.Find(h); w && w->parent; w = u.Find(w->parent)) {
            if (w->parent == parent && (w->style & ws::Child)) return true;
            if (!(w->style & ws::Child)) break;
        }
        return false;
    };
    std::vector<uint16_t> list;  // taken first: callbacks may create or destroy windows
    for (uint16_t h : u.Handles()) {
        const User::Window* w = u.Find(h);
        if (parent ? isDescendant(h) : !(w->style & ws::Child)) list.push_back(h);
    }
    const uint16_t ds = rt.CallbackData(proc.sel, 0);
    for (uint16_t h : list) {
        if (!u.Find(h) || rt.HasExited()) continue;
        if (uint16_t(rt.Processor().CallFar(proc.sel, proc.off, {h, uint16_t(lParam >> 16), uint16_t(lParam)}, ds)) == 0)
            break;
    }
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(a.Bytes());
}

void EnumWindows(Runtime& rt, Cpu& cpu) {  // (WNDENUMPROC, LPARAM) -> BOOL
    EnumerateWindows(rt, cpu, PascalArgs(cpu, {4, 4}), 0, 0);
}

void EnumTaskWindows(Runtime& rt, Cpu& cpu) {  // (HTASK, WNDENUMPROC, LPARAM) -> BOOL: one task, so all of them
    EnumerateWindows(rt, cpu, PascalArgs(cpu, {2, 4, 4}), 1, 0);
}

void EnumChildWindows(Runtime& rt, Cpu& cpu) {  // (HWND parent, WNDENUMPROC, LPARAM) -> BOOL
    const PascalArgs a(cpu, {2, 4, 4});
    if (!rt.Windows().Find(a.Word(0))) {
        cpu.Regs().r[AX] = 0;
        cpu.ReturnFar(a.Bytes());
        return;
    }
    EnumerateWindows(rt, cpu, a, 1, a.Word(0));
}

// (LPCSTR class or MAKEINTATOM or NULL, LPCSTR title or NULL) -> the first
// top-level window matching both (case-insensitively), or NULL.
// lstrcmp / lstrcmpi, the way Windows' language driver orders strings: letters
// compare regardless of case first; lstrcmp then puts lowercase before uppercase.
int CompareStrings(const std::string& a, const std::string& b, bool ignoreCase) {
    auto upper = [](char c) { return std::toupper(static_cast<unsigned char>(c)); };
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int ua = upper(a[i]), ub = upper(b[i]);
        if (ua != ub) return ua < ub ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    if (ignoreCase) return 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return std::islower(static_cast<unsigned char>(a[i])) ? -1 : 1;
    }
    return 0;
}

void CompareStringArgs(Runtime& rt, Cpu& cpu, bool ignoreCase) {  // (LPCSTR, LPCSTR) -> int
    const PascalArgs a(cpu, {4, 4});
    const FarPtr s1 = a.Ptr(0), s2 = a.Ptr(1);
    cpu.Regs().r[AX] = uint16_t(int16_t(CompareStrings(rt.Mem().ReadString(s1.sel, s1.off, 0xFFFF),
                                                       rt.Mem().ReadString(s2.sel, s2.off, 0xFFFF), ignoreCase)));
    cpu.ReturnFar(a.Bytes());
}

void lstrcmp(Runtime& rt, Cpu& cpu) { CompareStringArgs(rt, cpu, false); }
void lstrcmpi(Runtime& rt, Cpu& cpu) { CompareStringArgs(rt, cpu, true); }

// Hooks. The Windows 3.0 functions: SetWindowsHook returns the previous
// hook's procedure, which the new one passes on through DefHookProc.
void SetWindowsHook(Runtime& rt, Cpu& cpu) {  // (int id, HOOKPROC) -> the previous hook procedure
    const PascalArgs a(cpu, {2, 4});
    const FarPtr proc = a.Ptr(1);
    const uint32_t previous = rt.Windows().TopHookProc(a.Int(0));
    rt.Windows().AddHook(a.Int(0), proc.sel, proc.off, rt.CallbackData(proc.sel, 0));
    SetResult(cpu, previous);
    cpu.ReturnFar(a.Bytes());
}

void UnhookWindowsHook(Runtime& rt, Cpu& cpu) {  // (int id, HOOKPROC) -> BOOL
    const PascalArgs a(cpu, {2, 4});
    const FarPtr proc = a.Ptr(1);
    cpu.Regs().r[AX] = rt.Windows().RemoveHook(rt.Windows().FindHook(a.Int(0), proc.sel, proc.off)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void DefHookProc(Runtime& rt, Cpu& cpu) {  // (int code, WPARAM, LPARAM, HOOKPROC FAR* next) -> result
    const PascalArgs a(cpu, {2, 2, 4, 4});
    const FarPtr at = a.Ptr(3);
    uint32_t result = 0;
    if (!at.IsNull()) {
        const uint16_t off = rt.Mem().Read16(at.sel, at.off), sel = rt.Mem().Read16(at.sel, uint16_t(at.off + 2));
        const uint32_t next = rt.Windows().FindHookByProc(sel, off);
        if (next) result = rt.Windows().CallHook(next, a.Int(0), a.Word(1), a.Long(2));
    }
    SetResult(cpu, result);
    cpu.ReturnFar(a.Bytes());
}

// The Windows 3.1 functions, with HHOOK handles.
void SetWindowsHookEx(Runtime& rt, Cpu& cpu) {  // (int id, HOOKPROC, HINSTANCE, HTASK) -> HHOOK
    const PascalArgs a(cpu, {2, 4, 2, 2});
    const FarPtr proc = a.Ptr(1);
    SetResult(cpu, rt.Windows().AddHook(a.Int(0), proc.sel, proc.off, rt.CallbackData(proc.sel, a.Word(2))));
    cpu.ReturnFar(a.Bytes());
}

void UnhookWindowsHookEx(Runtime& rt, Cpu& cpu) {  // (HHOOK) -> BOOL
    const PascalArgs a(cpu, {4});
    cpu.Regs().r[AX] = rt.Windows().RemoveHook(a.Long(0)) ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

void CallNextHookEx(Runtime& rt, Cpu& cpu) {  // (HHOOK, int code, WPARAM, LPARAM) -> result
    const PascalArgs a(cpu, {4, 2, 2, 4});
    SetResult(cpu, rt.Windows().CallNextHook(a.Long(0), a.Int(1), a.Word(2), a.Long(3)));
    cpu.ReturnFar(a.Bytes());
}

// GetClassInfo: a registered class's WNDCLASS. The class name pointer is the
// caller's own; the menu name isn't kept, so it comes back NULL.
void GetClassInfo(Runtime& rt, Cpu& cpu) {  // (HINSTANCE, LPCSTR class or atom, WNDCLASS FAR*) -> BOOL
    const PascalArgs a(cpu, {2, 4, 4});
    const FarPtr name = a.Ptr(1), out = a.Ptr(2);
    std::string className = name.sel ? rt.Mem().ReadString(name.sel, name.off) : "";
    for (char& ch : className) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    const User::WindowClass* c =
        name.sel == 0 ? rt.Windows().FindClassAtom(name.off) : rt.Windows().FindClass(className);
    if (c && !out.IsNull()) {
        const uint16_t words[13] = {c->style,     c->procOff, c->procSel, c->clsExtra,     c->wndExtra,
                                    c->hInstance, c->hIcon,   c->hCursor, c->hbrBackground, 0,
                                    0,            name.off,   name.sel};
        for (uint16_t i = 0; i < 13; ++i) rt.Mem().Write16(out.sel, uint16_t(out.off + 2 * i), words[i]);
    }
    cpu.Regs().r[AX] = c ? 1 : 0;
    cpu.ReturnFar(a.Bytes());
}

// Scroll bars: ranges and positions are kept (programs read them back), but
// the bars aren't drawn yet.
void SetScrollPos(Runtime& rt, Cpu& cpu) {  // (HWND, int bar, int pos, BOOL redraw) -> previous position
    const PascalArgs a(cpu, {2, 2, 2, 2});
    ScrollBar* s = rt.Windows().Scroll(a.Word(0), a.Word(1));
    int16_t previous = 0;
    if (s) {
        previous = s->pos;
        s->pos = std::clamp<int16_t>(a.Int(2), std::min(s->min, s->max), std::max(s->min, s->max));
    }
    cpu.Regs().r[AX] = uint16_t(previous);
    cpu.ReturnFar(a.Bytes());
}

void GetScrollPos(Runtime& rt, Cpu& cpu) {  // (HWND, int bar) -> position
    const PascalArgs a(cpu, {2, 2});
    const ScrollBar* s = rt.Windows().Scroll(a.Word(0), a.Word(1));
    cpu.Regs().r[AX] = s ? uint16_t(s->pos) : 0;
    cpu.ReturnFar(a.Bytes());
}

void SetScrollRange(Runtime& rt, Cpu& cpu) {  // (HWND, int bar, int min, int max, BOOL redraw)
    const PascalArgs a(cpu, {2, 2, 2, 2, 2});
    if (ScrollBar* s = rt.Windows().Scroll(a.Word(0), a.Word(1))) {
        s->min = a.Int(2);
        s->max = a.Int(3);
        s->pos = std::clamp<int16_t>(s->pos, std::min(s->min, s->max), std::max(s->min, s->max));
    }
    cpu.Regs().r[AX] = 0;
    cpu.ReturnFar(a.Bytes());
}

void GetScrollRange(Runtime& rt, Cpu& cpu) {  // (HWND, int bar, int FAR* min, int FAR* max)
    const PascalArgs a(cpu, {2, 2, 4, 4});
    const ScrollBar* s = rt.Windows().Scroll(a.Word(0), a.Word(1));
    const FarPtr lo = a.Ptr(2), hi = a.Ptr(3);
    if (!lo.IsNull()) rt.Mem().Write16(lo.sel, lo.off, s ? uint16_t(s->min) : 0);
    if (!hi.IsNull()) rt.Mem().Write16(hi.sel, hi.off, s ? uint16_t(s->max) : 0);
    cpu.Regs().r[AX] = 0;
    cpu.ReturnFar(a.Bytes());
}

void ScrollBarNoOp(Runtime&, Cpu& cpu) {  // ShowScrollBar, EnableScrollBar: (HWND, UINT, UINT) -> TRUE
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(6);
}

// SetMessageQueue: the queue grows as needed, so any size is fine.
void SetMessageQueue(Runtime&, Cpu& cpu) {  // (int size) -> BOOL
    cpu.Regs().r[AX] = 1;
    cpu.ReturnFar(2);
}

void FindWindow(Runtime& rt, Cpu& cpu) {
    const PascalArgs a(cpu, {4, 4});
    const FarPtr cls = a.Ptr(0), title = a.Ptr(1);
    auto upper = [](std::string s) {
        for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wantClass = cls.sel ? upper(rt.Mem().ReadString(cls.sel, cls.off, 256)) : "";
    const std::string wantTitle = title.IsNull() ? "" : upper(rt.Mem().ReadString(title.sel, title.off, 256));
    const User& u = rt.Windows();
    uint16_t found = 0;
    for (uint16_t h : u.Handles()) {
        const User::Window* w = u.Find(h);
        if ((w->style & ws::Child) || w->destroying) continue;
        if (cls.sel && w->className != wantClass) continue;
        if (!cls.sel && cls.off) {  // an atom
            const User::WindowClass* c = u.FindClass(w->className);
            if (!c || c->atom != cls.off) continue;
        }
        if (!title.IsNull() && upper(w->title) != wantTitle) continue;
        found = h;
        break;
    }
    Return(cpu, a, found);
}

void GetFocus(Runtime& rt, Cpu& cpu) { SetResult(cpu, rt.Windows().Focus()); cpu.ReturnFar(0); }

void SetFocus(Runtime& rt, Cpu& cpu) {  // (HWND) -> previous
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().SetFocusTo(a.Word(0)));
}

void GetActiveWindow(Runtime& rt, Cpu& cpu) { SetResult(cpu, rt.Windows().Active()); cpu.ReturnFar(0); }

void SetActiveWindow(Runtime& rt, Cpu& cpu) {  // (HWND) -> previous
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().Activate(a.Word(0)));
}

void BringWindowToTop(Runtime& rt, Cpu& cpu) {  // (HWND) -> BOOL
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().Find(a.Word(0)) ? 1 : 0);
}

void GetDesktopWindow(Runtime&, Cpu& cpu) { SetResult(cpu, kDesktopHwnd); cpu.ReturnFar(0); }

void SetWindowText(Runtime& rt, Cpu& cpu) {  // (HWND, LPCSTR)
    const PascalArgs a(cpu, {2, 4});
    const FarPtr p = a.Ptr(1);
    rt.Windows().SetText(a.Word(0), p.IsNull() ? "" : rt.Mem().ReadString(p.sel, p.off));
    cpu.ReturnFar(a.Bytes());
}

void GetWindowText(Runtime& rt, Cpu& cpu) {  // (HWND, LPSTR, int max) -> length
    const PascalArgs a(cpu, {2, 4, 2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    Return(cpu, a, CopyString(rt, a.Ptr(1), w ? w->title : "", a.Int(2)));
}

void GetWindowTextLength(Runtime& rt, Cpu& cpu) {  // (HWND) -> length
    const PascalArgs a(cpu, {2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    Return(cpu, a, w ? uint32_t(w->title.size()) : 0);
}

void EnableWindow(Runtime& rt, Cpu& cpu) {  // (HWND, BOOL) -> TRUE if it was disabled
    const PascalArgs a(cpu, {2, 2});
    Return(cpu, a, rt.Windows().Enable(a.Word(0), a.Word(1) != 0) ? 1 : 0);
}

void IsWindowEnabled(Runtime& rt, Cpu& cpu) {  // (HWND) -> BOOL
    const PascalArgs a(cpu, {2});
    const User::Window* w = rt.Windows().Find(a.Word(0));
    Return(cpu, a, w && w->enabled ? 1 : 0);
}

void CallWindowProc(Runtime& rt, Cpu& cpu) {  // (WNDPROC, HWND, msg, wParam, lParam) -> LRESULT
    const PascalArgs a(cpu, {4, 2, 2, 2, 4});
    const FarPtr proc = a.Ptr(0);
    const uint16_t hwnd = a.Word(1), msg = a.Word(2), wParam = a.Word(3);
    const uint32_t lParam = a.Long(4);
    uint32_t result = 0;
    const Descriptor* d = rt.Mem().Lookup(proc.sel);
    if (d && d->kind == SegmentKind::Code) {
        const User::Window* w = rt.Windows().Find(hwnd);
        result = rt.Processor().CallFar(proc.sel, proc.off,
                                        {hwnd, msg, wParam, uint16_t(lParam >> 16), uint16_t(lParam)},
                                        rt.CallbackData(proc.sel, w ? w->hInstance : 0));
    } else if (d && d->kind == SegmentKind::Host) {
        result = rt.Windows().DefProc(hwnd, msg, wParam, lParam);  // DefWindowProc's own address
    }
    Return(cpu, a, result);
}

void GetSysColor(Runtime&, Cpu& cpu) {  // (int index) -> COLORREF
    const PascalArgs a(cpu, {2});
    Return(cpu, a, ClassicSysColor(a.Int(0)));
}

// --- Rectangles ---

void SetRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR*, l, t, r, b)
    const PascalArgs a(cpu, {4, 2, 2, 2, 2});
    WriteRect(rt.Mem(), a.Ptr(0), {a.Int(1), a.Int(2), a.Int(3), a.Int(4)});
    cpu.ReturnFar(a.Bytes());
}

void SetRectEmpty(Runtime& rt, Cpu& cpu) {  // (RECT FAR*)
    const PascalArgs a(cpu, {4});
    WriteRect(rt.Mem(), a.Ptr(0), {});
    cpu.ReturnFar(a.Bytes());
}

void CopyRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR* dst, const RECT FAR* src)
    const PascalArgs a(cpu, {4, 4});
    WriteRect(rt.Mem(), a.Ptr(0), ReadRect(rt.Mem(), a.Ptr(1)));
    Return(cpu, a, 1);
}

void OffsetRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR*, dx, dy)
    const PascalArgs a(cpu, {4, 2, 2});
    Rect16 r = ReadRect(rt.Mem(), a.Ptr(0));
    r = {int16_t(r.left + a.Int(1)), int16_t(r.top + a.Int(2)), int16_t(r.right + a.Int(1)),
         int16_t(r.bottom + a.Int(2))};
    WriteRect(rt.Mem(), a.Ptr(0), r);
    Return(cpu, a, 1);
}

void InflateRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR*, dx, dy)
    const PascalArgs a(cpu, {4, 2, 2});
    Rect16 r = ReadRect(rt.Mem(), a.Ptr(0));
    r = {int16_t(r.left - a.Int(1)), int16_t(r.top - a.Int(2)), int16_t(r.right + a.Int(1)),
         int16_t(r.bottom + a.Int(2))};
    WriteRect(rt.Mem(), a.Ptr(0), r);
    Return(cpu, a, 1);
}

void IntersectRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR* dst, a, b) -> BOOL not empty
    const PascalArgs a(cpu, {4, 4, 4});
    const Rect16 x = ReadRect(rt.Mem(), a.Ptr(1)), y = ReadRect(rt.Mem(), a.Ptr(2));
    Rect16 r{std::max(x.left, y.left), std::max(x.top, y.top), std::min(x.right, y.right),
             std::min(x.bottom, y.bottom)};
    if (r.Empty()) r = {};
    WriteRect(rt.Mem(), a.Ptr(0), r);
    Return(cpu, a, r.Empty() ? 0 : 1);
}

void UnionRect(Runtime& rt, Cpu& cpu) {  // (RECT FAR* dst, a, b) -> BOOL not empty
    const PascalArgs a(cpu, {4, 4, 4});
    const Rect16 x = ReadRect(rt.Mem(), a.Ptr(1)), y = ReadRect(rt.Mem(), a.Ptr(2));
    Rect16 r;
    if (x.Empty()) {
        r = y;
    } else if (y.Empty()) {
        r = x;
    } else {
        r = {std::min(x.left, y.left), std::min(x.top, y.top), std::max(x.right, y.right),
             std::max(x.bottom, y.bottom)};
    }
    if (r.Empty()) r = {};
    WriteRect(rt.Mem(), a.Ptr(0), r);
    Return(cpu, a, r.Empty() ? 0 : 1);
}

void PtInRect(Runtime& rt, Cpu& cpu) {  // (const RECT FAR*, POINT) -> BOOL
    const PascalArgs a(cpu, {4, 4});
    const Rect16 r = ReadRect(rt.Mem(), a.Ptr(0));
    const int16_t x = int16_t(a.Long(1)), y = int16_t(a.Long(1) >> 16);
    Return(cpu, a, x >= r.left && x < r.right && y >= r.top && y < r.bottom ? 1 : 0);
}

void IsRectEmpty(Runtime& rt, Cpu& cpu) {  // (const RECT FAR*) -> BOOL
    const PascalArgs a(cpu, {4});
    Return(cpu, a, ReadRect(rt.Mem(), a.Ptr(0)).Empty() ? 1 : 0);
}

void EqualRect(Runtime& rt, Cpu& cpu) {  // (const RECT FAR*, const RECT FAR*) -> BOOL
    const PascalArgs a(cpu, {4, 4});
    const Rect16 x = ReadRect(rt.Mem(), a.Ptr(0)), y = ReadRect(rt.Mem(), a.Ptr(1));
    Return(cpu, a, x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom ? 1 : 0);
}

// --- Input state and the cursor ---

void GetKeyState(Runtime& rt, Cpu& cpu) {  // (int vk) -> int
    const PascalArgs a(cpu, {2});
    Return(cpu, a, uint16_t(rt.Windows().KeyState(uint8_t(a.Word(0)))));
}

void GetAsyncKeyState(Runtime& rt, Cpu& cpu) {  // (int vk) -> int: 8000h while down
    const PascalArgs a(cpu, {2});
    Return(cpu, a, uint16_t(rt.Windows().KeyState(uint8_t(a.Word(0))) & 0x8000));
}

void SetCapture(Runtime& rt, Cpu& cpu) {  // (HWND) -> previous
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().SetCaptureTo(a.Word(0)));
}

void ReleaseCapture(Runtime& rt, Cpu& cpu) {
    rt.Windows().SetCaptureTo(0);
    cpu.ReturnFar(0);
}

void GetCapture(Runtime& rt, Cpu& cpu) { SetResult(cpu, rt.Windows().Captured()); cpu.ReturnFar(0); }

void SetCursor(Runtime& rt, Cpu& cpu) {  // (HCURSOR or NULL) -> previous
    const PascalArgs a(cpu, {2});
    Return(cpu, a, rt.Windows().SetCursorHandle(a.Word(0)));
}

void ShowCursor(Runtime& rt, Cpu& cpu) {  // (BOOL) -> display count
    const PascalArgs a(cpu, {2});
    Return(cpu, a, uint16_t(int16_t(rt.Windows().ShowCursorCount(a.Word(0) != 0))));
}

void GetCursorPos(Runtime& rt, Cpu& cpu) {  // (POINT FAR*)
    const PascalArgs a(cpu, {4});
    const FarPtr p = a.Ptr(0);
    rt.Mem().Write16(p.sel, p.off, uint16_t(rt.Windows().MouseX()));
    rt.Mem().Write16(p.sel, uint16_t(p.off + 2), uint16_t(rt.Windows().MouseY()));
    cpu.ReturnFar(a.Bytes());
}

void SetCursorPos(Runtime& rt, Cpu& cpu) {  // (x, y)
    const PascalArgs a(cpu, {2, 2});
    rt.Note("setcursorpos", "SetCursorPos doesn't move the real mouse pointer yet");
    rt.Windows().SetMousePosition(a.Int(0), a.Int(1));
    cpu.ReturnFar(a.Bytes());
}

void ClipCursor(Runtime& rt, Cpu& cpu) {  // (const RECT FAR* or NULL)
    const PascalArgs a(cpu, {4});
    if (!a.Ptr(0).IsNull()) rt.Note("clipcursor", "ClipCursor is ignored (the pointer isn't confined)");
    cpu.ReturnFar(a.Bytes());
}

// --- wsprintf -----------------------------------------------------------------------------------

// Win16 wsprintf: %[-][#][0][width][.precision][l]{c d i u x X s}, with %s
// taking a far pointer. Arguments come as a stream of words.
std::string FormatW16(Runtime& rt, const std::string& format, const std::function<uint16_t()>& next) {
    std::string out;
    for (size_t i = 0; i < format.size() && out.size() < 1024; ++i) {
        const char ch = format[i];
        if (ch != '%') {
            out += ch;
            continue;
        }
        if (++i >= format.size()) break;
        bool left = false, alt = false, zero = false, isLong = false;
        for (;; ++i) {
            if (i >= format.size()) return out;
            if (format[i] == '-') left = true;
            else if (format[i] == '#') alt = true;
            else if (format[i] == '0') zero = true;
            else break;
        }
        size_t width = 0, precision = SIZE_MAX;
        while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
            width = width * 10 + size_t(format[i++] - '0');
        if (i < format.size() && format[i] == '.') {
            precision = 0;
            while (++i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])))
                precision = precision * 10 + size_t(format[i] - '0');
        }
        if (i < format.size() && (format[i] == 'l' || format[i] == 'L')) {
            isLong = true;
            ++i;
        } else if (i < format.size() && format[i] == 'h') {
            ++i;
        }
        if (i >= format.size()) break;
        const char type = format[i];
        std::string field;
        bool numeric = true;
        auto value = [&]() -> uint32_t {
            const uint32_t lo = next();
            return isLong ? lo | (uint32_t(next()) << 16) : lo;
        };
        switch (type) {
        case 'c': field = std::string(1, char(next())); numeric = false; break;
        case 'd':
        case 'i': {
            const uint32_t v = value();
            const int32_t s = isLong ? int32_t(v) : int16_t(v);
            field = std::to_string(s);
            break;
        }
        case 'u': field = std::to_string(value()); break;
        case 'x':
        case 'X': {
            char buf[16];
            std::snprintf(buf, sizeof(buf), type == 'x' ? "%lx" : "%lX", static_cast<unsigned long>(value()));
            field = (alt ? (type == 'x' ? "0x" : "0X") : "") + std::string(buf);
            break;
        }
        case 's': {
            const uint16_t off = next(), sel = next();
            field = (sel || off) ? rt.Mem().ReadString(sel, off, precision == SIZE_MAX ? 1024 : precision)
                                 : std::string("(null)");
            numeric = false;
            break;
        }
        default: field = std::string(1, type); numeric = false; break;  // %% and unknown
        }
        if (field.size() < width) {
            const bool zeros = zero && numeric && !left;
            const std::string pad(width - field.size(), zeros ? '0' : ' ');
            if (left) {
                field += pad;
            } else if (zeros && !field.empty() && field[0] == '-') {
                field = "-" + pad + field.substr(1);
            } else {
                field = pad + field;
            }
        }
        out += field;
    }
    return out.substr(0, 1024);
}

uint16_t WriteFormatted(Runtime& rt, FarPtr dst, const std::string& s) {
    return CopyString(rt, dst, s, int(s.size()) + 1);
}

// int FAR CDECL wsprintf(LPSTR, LPCSTR, ...): the caller removes the arguments.
void wsprintf(Runtime& rt, Cpu& cpu) {
    const FarPtr dst = ArgPtr(cpu, 0), fmt = ArgPtr(cpu, 4);
    uint16_t at = 8;
    const std::string s = FormatW16(rt, rt.Mem().ReadString(fmt.sel, fmt.off, 1024), [&] {
        const uint16_t w = cpu.StackArg(at);
        at = uint16_t(at + 2);
        return w;
    });
    SetResult(cpu, WriteFormatted(rt, dst, s));
    cpu.ReturnFar(0);
}

void wvsprintf(Runtime& rt, Cpu& cpu) {  // (LPSTR, LPCSTR, const void FAR* args)
    const PascalArgs a(cpu, {4, 4, 4});
    const FarPtr fmt = a.Ptr(1), args = a.Ptr(2);
    uint16_t at = args.off;
    const std::string s = FormatW16(rt, rt.Mem().ReadString(fmt.sel, fmt.off, 1024), [&] {
        const uint16_t w = rt.Mem().Read16(args.sel, at);
        at = uint16_t(at + 2);
        return w;
    });
    Return(cpu, a, WriteFormatted(rt, a.Ptr(0), s));
}

}  // namespace

std::vector<ApiFunction> UserWindowApi() {
    return {
        {16, "CLIPCURSOR", ClipCursor},
        {17, "GETCURSORPOS", GetCursorPos},
        {18, "SETCAPTURE", SetCapture},
        {19, "RELEASECAPTURE", ReleaseCapture},
        {22, "SETFOCUS", SetFocus},
        {23, "GETFOCUS", GetFocus},
        {28, "CLIENTTOSCREEN", ClientToScreen},
        {29, "SCREENTOCLIENT", ScreenToClient},
        {30, "WINDOWFROMPOINT", WindowFromPoint},
        {31, "ISICONIC", ReturnFalse1},
        {32, "GETWINDOWRECT", GetWindowRect},
        {34, "ENABLEWINDOW", EnableWindow},
        {35, "ISWINDOWENABLED", IsWindowEnabled},
        {36, "GETWINDOWTEXT", GetWindowText},
        {37, "SETWINDOWTEXT", SetWindowText},
        {38, "GETWINDOWTEXTLENGTH", GetWindowTextLength},
        {45, "BRINGWINDOWTOTOP", BringWindowToTop},
        {46, "GETPARENT", GetParent},
        {47, "ISWINDOW", IsWindow},
        {49, "ISWINDOWVISIBLE", IsWindowVisible},
        {50, "FINDWINDOW", FindWindow},
        {54, "ENUMWINDOWS", EnumWindows},
        {55, "ENUMCHILDWINDOWS", EnumChildWindows},
        {56, "MOVEWINDOW", MoveWindow},
        {62, "SETSCROLLPOS", SetScrollPos},
        {63, "GETSCROLLPOS", GetScrollPos},
        {64, "SETSCROLLRANGE", SetScrollRange},
        {65, "GETSCROLLRANGE", GetScrollRange},
        {59, "SETACTIVEWINDOW", SetActiveWindow},
        {60, "GETACTIVEWINDOW", GetActiveWindow},
        {69, "SETCURSOR", SetCursor},
        {70, "SETCURSORPOS", SetCursorPos},
        {71, "SHOWCURSOR", ShowCursor},
        {72, "SETRECT", SetRect},
        {73, "SETRECTEMPTY", SetRectEmpty},
        {74, "COPYRECT", CopyRect},
        {75, "ISRECTEMPTY", IsRectEmpty},
        {76, "PTINRECT", PtInRect},
        {77, "OFFSETRECT", OffsetRect},
        {78, "INFLATERECT", InflateRect},
        {79, "INTERSECTRECT", IntersectRect},
        {80, "UNIONRECT", UnionRect},
        {106, "GETKEYSTATE", GetKeyState},
        {121, "SETWINDOWSHOOK", SetWindowsHook},
        {122, "CALLWINDOWPROC", CallWindowProc},
        {129, "GETCLASSWORD", GetClassWord},
        {130, "SETCLASSWORD", SetClassWord},
        {131, "GETCLASSLONG", GetClassLong},
        {133, "GETWINDOWWORD", GetWindowWord},
        {134, "SETWINDOWWORD", SetWindowWord},
        {135, "GETWINDOWLONG", GetWindowLong},
        {136, "SETWINDOWLONG", SetWindowLong},
        {180, "GETSYSCOLOR", GetSysColor},
        {191, "CHILDWINDOWFROMPOINT", ChildWindowFromPoint},
        {225, "ENUMTASKWINDOWS", EnumTaskWindows},
        {232, "SETWINDOWPOS", SetWindowPos},
        {234, "UNHOOKWINDOWSHOOK", UnhookWindowsHook},
        {235, "DEFHOOKPROC", DefHookProc},
        {236, "GETCAPTURE", GetCapture},
        {244, "EQUALRECT", EqualRect},
        {249, "GETASYNCKEYSTATE", GetAsyncKeyState},
        {262, "GETWINDOW", GetWindow},
        {266, "SETMESSAGEQUEUE", SetMessageQueue},
        {267, "SHOWSCROLLBAR", ScrollBarNoOp},
        {272, "ISZOOMED", ReturnFalse1},
        {286, "GETDESKTOPWINDOW", GetDesktopWindow},
        {291, "SETWINDOWSHOOKEX", SetWindowsHookEx},
        {292, "UNHOOKWINDOWSHOOKEX", UnhookWindowsHookEx},
        {293, "CALLNEXTHOOKEX", CallNextHookEx},
        {404, "GETCLASSINFO", GetClassInfo},
        {420, "_WSPRINTF", wsprintf},
        {421, "WVSPRINTF", wvsprintf},
        {430, "LSTRCMP", lstrcmp},
        {471, "LSTRCMPI", lstrcmpi},
        {482, "ENABLESCROLLBAR", ScrollBarNoOp},
    };
}

}  // namespace retro::win16
