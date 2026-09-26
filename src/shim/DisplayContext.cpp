#include "DisplayContext.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "Log.h"
#include "ShimState.h"

namespace retro::shim::display {

LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace {

struct ManagedEntry {
    ManagedView view;
    LONG originalStyle = 0;
    LONG originalExStyle = 0;
};

struct SubclassEntry {
    HWND hwnd = nullptr;
    WNDPROC original = nullptr;  // W-callable procedure we forward to
};

struct CallerRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
    bool system = false;
};

// All mutable state lives behind g_lock. Rule: never call anything that can
// send a window message while holding it (SetWindowPos, SendMessage, ...): the
// message lands in SubclassProc, which takes the lock again.
struct State {
    bool modeActive = false;
    DisplayMode mode{};
    std::vector<ManagedEntry> managed;
    std::vector<SubclassEntry> subclassed;
    bool hasGameClip = false;
    Rect gameClip{};
    bool cursorHidden = false;
    bool clipApplied = false;
    std::vector<CallerRange> callers;
};

State g_state;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<int> g_managedCount{0};

RealApi g_real;
ScalingOptions g_scaling;
bool g_windowed = false;

class SharedLock {
public:
    SharedLock() { AcquireSRWLockShared(&g_lock); }
    ~SharedLock() { ReleaseSRWLockShared(&g_lock); }
};

class ExclusiveLock {
public:
    ExclusiveLock() { AcquireSRWLockExclusive(&g_lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&g_lock); }
};

Rect FromRECT(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }
RECT ToRECT(const Rect& r) { return {r.left, r.top, r.right, r.bottom}; }

Rect Intersect(const Rect& a, const Rect& b) {
    Rect r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
           std::min(a.bottom, b.bottom)};
    if (r.right < r.left) r.right = r.left;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

ManagedEntry* FindEntryLocked(HWND hwnd) {
    for (ManagedEntry& e : g_state.managed) {
        if (e.view.hwnd == hwnd) return &e;
    }
    return nullptr;
}

WNDPROC OriginalProc(HWND hwnd) {
    SharedLock lock;
    for (const SubclassEntry& s : g_state.subclassed) {
        if (s.hwnd == hwnd) return s.original;
    }
    return nullptr;
}

bool IsOwnTopLevel(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId() && GetAncestor(hwnd, GA_ROOT) == hwnd &&
           !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD);
}

// Re-reads the real client rectangle and re-plans the viewport inside it.
void RefreshGeometry(HWND hwnd) {
    ManagedView v;
    if (!FindManaged(hwnd, v)) return;

    RECT wr{}, cr{};
    POINT origin{0, 0};
    {
        DpiScope scope(v.dpi);
        g_real.GetWindowRect(hwnd, &wr);
        g_real.GetClientRect(hwnd, &cr);
        g_real.ClientToScreen(hwnd, &origin);
    }
    const Rect client{origin.x, origin.y, origin.x + cr.right, origin.y + cr.bottom};
    if (client.Width() <= 0 || client.Height() <= 0) return;  // minimized

    const Viewport vp = PlanViewport(v.virt, client, g_scaling);
    ExclusiveLock lock;
    if (ManagedEntry* e = FindEntryLocked(hwnd)) {
        e->view.window = FromRECT(wr);
        e->view.client = client;
        e->view.viewport = vp.rect;
    }
}

// Sizes a managed window per the configured presentation (fullscreen or windowed).
void Relayout(HWND hwnd) {
    ManagedView v;
    if (!FindManaged(hwnd, v)) return;

    WindowLayout layout;
    {
        DpiScope scope(v.dpi);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);

        if (g_windowed) {
            RECT frame{0, 0, 0, 0};
            AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)),
                                     FALSE,
                                     static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)),
                                     GetDpiForWindow(hwnd));
            const Insets insets{-frame.left, -frame.top, frame.right, frame.bottom};
            layout = PlanWindowedLayout(v.virt, FromRECT(mi.rcWork), insets, g_scaling);
        } else {
            layout = PlanFullscreenLayout(v.virt, FromRECT(mi.rcMonitor), g_scaling);
        }

        g_real.SetWindowPos(hwnd, nullptr, layout.window.left, layout.window.top,
                            layout.window.Width(), layout.window.Height(),
                            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    RefreshGeometry(hwnd);

    FindManaged(hwnd, v);
    log::Write("display: window %p -> %dx%d virtual in %dx%d viewport at (%d,%d), scale %s%d",
               hwnd, v.virt.w, v.virt.h, v.viewport.Width(), v.viewport.Height(),
               v.viewport.left, v.viewport.top, layout.integerScale ? "x" : "fractional ",
               layout.integerScale);
}

void SendVirtualSize(HWND hwnd, Size virt) {
    if (!IsIconic(hwnd)) SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(virt.w, virt.h));
}

void Unsubclass(HWND hwnd) {
    WNDPROC original = nullptr;
    {
        ExclusiveLock lock;
        auto& subs = g_state.subclassed;
        auto it = std::find_if(subs.begin(), subs.end(),
                               [&](const SubclassEntry& s) { return s.hwnd == hwnd; });
        if (it == subs.end()) return;
        original = it->original;
        subs.erase(it);
    }
    if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(SubclassProc)) {
        g_real.SetWindowLongW(hwnd, GWLP_WNDPROC,
                              static_cast<LONG>(reinterpret_cast<LONG_PTR>(original)));
    }
}

void Forget(HWND hwnd) {
    {
        ExclusiveLock lock;
        auto& m = g_state.managed;
        const auto before = m.size();
        m.erase(std::remove_if(m.begin(), m.end(),
                               [&](const ManagedEntry& e) { return e.view.hwnd == hwnd; }),
                m.end());
        g_managedCount -= static_cast<int>(before - m.size());
    }
    Unsubclass(hwnd);
}

void BroadcastDisplayChange(const DisplayMode& mode) {
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) {
                const auto* m = reinterpret_cast<const DisplayMode*>(lp);
                // Synchronous for our own thread's windows, async for others (no
                // cross-thread deadlocks).
                SendNotifyMessageW(hwnd, WM_DISPLAYCHANGE, m->bitsPerPixel,
                                   MAKELPARAM(m->width, m->height));
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&mode));
}

void AdoptFullscreenWindows(Size virt) {
    struct Ctx {
        Size virt;
        std::vector<HWND> found;
    } ctx{virt, {}};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (!IsOwnTopLevel(hwnd)) return TRUE;
            RECT r{};
            {
                DpiScope scope(GetWindowDpiAwarenessContext(hwnd));
                g_real.GetWindowRect(hwnd, &r);
            }
            const auto style = static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_STYLE));
            const auto exStyle = static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_EXSTYLE));
            if (LooksLikeFullscreenWindow(style, exStyle, FromRECT(r), c->virt))
                c->found.push_back(hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    for (HWND hwnd : ctx.found) Manage(hwnd);
}

bool StartsWithInsensitive(const wchar_t* s, const wchar_t* prefix, int prefixLen) {
    return static_cast<int>(wcslen(s)) >= prefixLen &&
           CompareStringOrdinal(s, prefixLen, prefix, prefixLen, TRUE) == CSTR_EQUAL;
}

}  // namespace

// Forwards everything to the game's procedure; rewrites the few sent
// messages that carry real geometry.
LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC original = OriginalProc(hwnd);
    if (!original) return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == WM_NCDESTROY) {
        const LRESULT r = CallWindowProcW(original, hwnd, msg, wp, lp);
        Forget(hwnd);
        UpdateCursorClip(-1);
        return r;
    }

    ManagedView v;
    if (FindManaged(hwnd, v)) {
        switch (msg) {
        case WM_WINDOWPOSCHANGED:
            RefreshGeometry(hwnd);
            break;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) lp = MAKELPARAM(v.virt.w, v.virt.h);
            break;
        case WM_ACTIVATEAPP: {
            const LRESULT r = CallWindowProcW(original, hwnd, msg, wp, lp);
            UpdateCursorClip(wp ? 1 : 0);
            return r;
        }
        case WM_ACTIVATE: {
            const LRESULT r = CallWindowProcW(original, hwnd, msg, wp, lp);
            UpdateCursorClip(LOWORD(wp) != WA_INACTIVE ? 1 : 0);
            return r;
        }
        default:
            break;
        }
    }
    return CallWindowProcW(original, hwnd, msg, wp, lp);
}

RealApi& Real() { return g_real; }

void FillLetterbox(HDC hdc, const ManagedView& v) {
    const Rect vp = v.ClientViewport();
    const int32_t w = v.client.Width(), h = v.client.Height();
    const HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const RECT bars[] = {{0, 0, w, vp.top},
                         {0, vp.bottom, w, h},
                         {0, vp.top, vp.left, vp.bottom},
                         {vp.right, vp.top, w, vp.bottom}};
    for (const RECT& r : bars) {
        if (r.right > r.left && r.bottom > r.top) FillRect(hdc, &r, black);
    }
}

void Configure(const ShimConfig& config) {
    g_windowed = (config.displayFlags & DisplayFlag_Windowed) != 0;
    g_scaling.integerScaling = (config.displayFlags & DisplayFlag_NoIntegerScaling) == 0;
    g_scaling.classicAspect = true;
}

bool Windowed() { return g_windowed; }
const ScalingOptions& Scaling() { return g_scaling; }

bool GetVirtualMode(DisplayMode& out) {
    SharedLock lock;
    out = g_state.mode;
    return g_state.modeActive;
}

DisplayMode RealMode() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    DpiScope scope(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!g_real.EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return {};
    return {dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency};
}

Size PrimaryMonitorSize() {
    const DisplayMode m = RealMode();
    return {static_cast<int32_t>(m.width), static_cast<int32_t>(m.height)};
}

void SetVirtualMode(const DisplayMode& mode) {
    const Size virt{static_cast<int32_t>(mode.width), static_cast<int32_t>(mode.height)};
    std::vector<HWND> relayout;
    {
        ExclusiveLock lock;
        g_state.modeActive = true;
        g_state.mode = mode;
        for (ManagedEntry& e : g_state.managed) {
            e.view.virt = virt;
            relayout.push_back(e.view.hwnd);
        }
    }
    log::Write("display: virtual mode %ux%u %u bpp %u Hz", mode.width, mode.height,
               mode.bitsPerPixel, mode.refreshHz);
    if (mode.bitsPerPixel <= 8)
        log::Write("display: %u bpp palettized modes are not emulated yet; colours may be wrong",
                   mode.bitsPerPixel);

    for (HWND hwnd : relayout) {
        Relayout(hwnd);
        SendVirtualSize(hwnd, virt);
    }
    AdoptFullscreenWindows(virt);
    BroadcastDisplayChange(mode);
    UpdateCursorClip(-1);
}

void ClearVirtualMode() {
    std::vector<ManagedEntry> released;
    {
        ExclusiveLock lock;
        g_state.modeActive = false;
        released.swap(g_state.managed);
        g_managedCount = 0;
    }
    log::Write("display: back to desktop mode");

    // The windows stay subclassed (SubclassProc just forwards now); the game
    // repositions them for its windowed/desktop state itself.
    for (const ManagedEntry& e : released) {
        // Restore the frame, but keep the window's current state bits.
        constexpr LONG kState = WS_VISIBLE | WS_MINIMIZE | WS_DISABLED;
        const LONG current = GetWindowLongW(e.view.hwnd, GWL_STYLE);
        g_real.SetWindowLongW(e.view.hwnd, GWL_STYLE,
                              (e.originalStyle & ~kState) | (current & kState));
        g_real.SetWindowLongW(e.view.hwnd, GWL_EXSTYLE, e.originalExStyle);
        g_real.SetWindowPos(e.view.hwnd, nullptr, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                SWP_FRAMECHANGED);
    }
    BroadcastDisplayChange(RealMode());
    UpdateCursorClip(-1);
}

bool AnyManaged() { return g_managedCount.load(std::memory_order_relaxed) > 0; }

bool FindManaged(HWND hwnd, ManagedView& out) {
    if (!AnyManaged() || !hwnd) return false;
    SharedLock lock;
    for (const ManagedEntry& e : g_state.managed) {
        if (e.view.hwnd == hwnd) {
            out = e.view;
            return true;
        }
    }
    return false;
}

bool FirstManaged(ManagedView& out) {
    if (!AnyManaged()) return false;
    SharedLock lock;
    if (g_state.managed.empty()) return false;
    out = g_state.managed.front().view;
    return true;
}

bool Manage(HWND hwnd) {
    DisplayMode mode;
    if (!GetVirtualMode(mode) || !IsWindow(hwnd)) return false;

    ManagedView existing;
    if (FindManaged(hwnd, existing)) return true;

    ManagedEntry entry;
    entry.view.hwnd = hwnd;
    entry.view.virt = {static_cast<int32_t>(mode.width), static_cast<int32_t>(mode.height)};
    entry.view.dpi = GetWindowDpiAwarenessContext(hwnd);
    entry.originalStyle = GetWindowLongW(hwnd, GWL_STYLE);
    entry.originalExStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    bool needSubclass = true;
    {
        ExclusiveLock lock;
        g_state.managed.push_back(entry);
        ++g_managedCount;
        for (const SubclassEntry& s : g_state.subclassed) {
            if (s.hwnd == hwnd) needSubclass = false;
        }
    }

    if (needSubclass) {
        // Register the forwarding target before installing the subclass, so
        // SubclassProc can always find it.
        const LONG_PTR original = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
        {
            ExclusiveLock lock;
            g_state.subclassed.push_back({hwnd, reinterpret_cast<WNDPROC>(original)});
        }
        g_real.SetWindowLongW(hwnd, GWLP_WNDPROC,
                              static_cast<LONG>(reinterpret_cast<LONG_PTR>(SubclassProc)));
    }

    const auto style = static_cast<uint32_t>(entry.originalStyle);
    const auto exStyle = static_cast<uint32_t>(entry.originalExStyle);
    g_real.SetWindowLongW(hwnd, GWL_STYLE, static_cast<LONG>(SandboxStyle(style, g_windowed)));
    g_real.SetWindowLongW(hwnd, GWL_EXSTYLE,
                          static_cast<LONG>(SandboxExStyle(exStyle, g_windowed)));

    const bool awareV2 =
        AreDpiAwarenessContextsEqual(entry.view.dpi, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    log::Write("display: managing window %p (style 0x%08X -> 0x%08X, %s)", hwnd, style,
               SandboxStyle(style, g_windowed),
               awareV2 ? "per-monitor DPI aware" : "DPI virtualized by Windows");

    Relayout(hwnd);
    SendVirtualSize(hwnd, entry.view.virt);
    UpdateCursorClip(-1);
    return true;
}

bool AdoptIfFullscreen(HWND hwnd, const Rect& requestedVirtualRect) {
    ManagedView v;
    if (FindManaged(hwnd, v)) return true;

    DisplayMode mode;
    if (!GetVirtualMode(mode) || !IsOwnTopLevel(hwnd)) return false;

    const Size virt{static_cast<int32_t>(mode.width), static_cast<int32_t>(mode.height)};
    const auto style = static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_STYLE));
    const auto exStyle = static_cast<uint32_t>(GetWindowLongW(hwnd, GWL_EXSTYLE));
    if (!LooksLikeFullscreenWindow(style, exStyle, requestedVirtualRect, virt)) return false;
    return Manage(hwnd);
}

bool ReplaceGameWndProc(HWND hwnd, LONG newProc, bool ansi, LONG& previous) {
    const WNDPROC ours = OriginalProc(hwnd);
    if (!ours) return false;

    // Temporarily unhook so the game's setter returns the previous procedure
    // in the form (ANSI/Unicode) it expects, then put our subclass back on top.
    g_real.SetWindowLongW(hwnd, GWLP_WNDPROC, static_cast<LONG>(reinterpret_cast<LONG_PTR>(ours)));
    previous = ansi ? g_real.SetWindowLongA(hwnd, GWL_WNDPROC, newProc)
                    : g_real.SetWindowLongW(hwnd, GWL_WNDPROC, newProc);
    const LONG wide = g_real.SetWindowLongW(
        hwnd, GWLP_WNDPROC, static_cast<LONG>(reinterpret_cast<LONG_PTR>(SubclassProc)));

    ExclusiveLock lock;
    for (SubclassEntry& s : g_state.subclassed) {
        if (s.hwnd == hwnd) s.original = reinterpret_cast<WNDPROC>(static_cast<LONG_PTR>(wide));
    }
    return true;
}

void SetGameClip(const RECT* virtualRect) {
    ExclusiveLock lock;
    g_state.hasGameClip = virtualRect != nullptr;
    if (virtualRect) g_state.gameClip = FromRECT(*virtualRect);
}

bool GetGameClip(Rect& virtualRect) {
    SharedLock lock;
    virtualRect = g_state.gameClip;
    return g_state.hasGameClip;
}

void SetCursorHidden(bool hidden) {
    ExclusiveLock lock;
    g_state.cursorHidden = hidden;
}

void UpdateCursorClip(int active) {
    ManagedView v;
    const bool haveWindow = FirstManaged(v);

    bool hasGameClip, cursorHidden, clipApplied;
    Rect gameClip;
    {
        SharedLock lock;
        hasGameClip = g_state.hasGameClip;
        gameClip = g_state.gameClip;
        cursorHidden = g_state.cursorHidden;
        clipApplied = g_state.clipApplied;
    }

    bool want = false;
    Rect target{};
    if (haveWindow) {
        const bool isActive = active >= 0 ? active == 1 : GetForegroundWindow() == v.hwnd;
        if (isActive && hasGameClip) {
            target = Intersect(v.Map().ToReal(gameClip), v.viewport);
            want = true;
        } else if (isActive && cursorHidden) {
            // A hidden cursor in borderless mode would otherwise wander onto
            // another monitor, and the next click would deactivate the game.
            target = v.viewport;
            want = true;
        }
    }

    if (want) {
        const RECT r = ToRECT(target);
        DpiScope scope(v.dpi);
        g_real.ClipCursor(&r);
    } else if (clipApplied) {
        g_real.ClipCursor(nullptr);
    }
    if (want != clipApplied) {
        ExclusiveLock lock;
        g_state.clipApplied = want;
    }
}

bool IsSystemCaller(const void* returnAddress) {
    const auto addr = reinterpret_cast<uintptr_t>(returnAddress);
    {
        SharedLock lock;
        for (const CallerRange& r : g_state.callers) {
            if (addr >= r.begin && addr < r.end) return r.system;
        }
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(returnAddress), &module)) {
        return false;  // JIT/heap code: treat as the game
    }

    bool system = module == Module();
    if (!system) {
        static wchar_t windowsDir[MAX_PATH];
        static int windowsDirLen = [] {
            UINT n = GetSystemWindowsDirectoryW(windowsDir, MAX_PATH);
            if (n > 0 && n < MAX_PATH - 1 && windowsDir[n - 1] != L'\\') {
                windowsDir[n++] = L'\\';
                windowsDir[n] = L'\0';
            }
            return static_cast<int>(n);
        }();
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
        system = n > 0 && n < MAX_PATH && windowsDirLen > 0 &&
                 StartsWithInsensitive(path, windowsDir, windowsDirLen);
    }

    // Cache by address range (from the PE header) to avoid the loader lock on
    // hot paths like PeekMessage.
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    ExclusiveLock lock;
    if (g_state.callers.size() < 256)
        g_state.callers.push_back({base, base + nt->OptionalHeader.SizeOfImage, system});
    return system;
}

}  // namespace retro::shim::display
