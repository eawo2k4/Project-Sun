# Project Sun

Project Sun is a software/layer that will let you run Windows 3.x–XP games.

RetroRunner is an in-process compatibility runner for vintage Windows games (Win 3.x → XP) on
Windows 11 x64. It runs games natively: no VM, no whole-PC emulator, no Wine.
32-bit games run under WOW64 with an injected shim DLL. 16-bit NE programs run
in-process on the built-in Win16 engine: an NE loader, a virtual LDT and a
286-class interpreter. The engine is at an early stage; see [Win16 engine](#win16-engine).

## Layout

```
Project Sun/
├─ CMakeLists.txt          x86-only guard, static CRT, Detours import target
├─ CMakePresets.json       "x86" configure preset + debug/release build & test presets
├─ vcpkg.json              dependencies (detours), pinned baseline
├─ src/
│  ├─ core/                RetroCore.lib: shared by launcher, shim and tests
│  │  ├─ include/retro/ExeFormat.h     MZ / NE / LE / LX / PE header parser API
│  │  ├─ include/retro/ClampPolicy.h   pure disk/memory clamping rules (unit-tested)
│  │  ├─ include/retro/DisplayMath.h   modes, 4:3 viewport planning, coordinate mapping
│  │  ├─ include/retro/FramePacing.h   frame scheduler + high-resolution waiter
│  │  ├─ include/retro/PixelConvert.h  8/16/24/32-bit surfaces → 32-bit BGRA, CRC-32
│  │  ├─ include/retro/PathUtil.h      ANSI/UTF-8 path helpers
│  │  ├─ include/retro/ShimProtocol.h  launcher → shim config payload (GUID + struct)
│  │  └─ *.cpp
│  ├─ win16/               RetroWin16.lib: 16-bit Windows engine (no Windows headers)
│  │  ├─ NeImage.cpp               NE parser: segments, relocations, imports, entries, resources
│  │  ├─ NeLoader.cpp              selectors per segment, fixup chains, PSP, initial registers
│  │  ├─ Memory.cpp                virtual LDT + linear arena, #GP-checked selector:offset
│  │  ├─ Cpu.cpp                   286-class 16-bit interpreter
│  │  ├─ ApiCatalog.cpp            generated: every system DLL export (name, params, constants)
│  │  ├─ Kernel.cpp                KERNEL: task, heaps, modules, strings, files, INI, resources
│  │  ├─ LocalHeap.cpp             LocalAlloc & co. inside DGROUP (Win16 handle tables)
│  │  ├─ Files.cpp                 the task's file view (program directory, read-only), INI files
│  │  ├─ Resources.cpp             resource lookup/loading, string tables
│  │  ├─ User.cpp                  USER: classes, windows, message queue, timers, callbacks
│  │  ├─ Gdi.cpp                   GDI: 16-bit handles over host GDI, text, back buffers, presentation
│  │  └─ Runtime.cpp               Win16 task: DLL dispatch, stub modules, trace, INT 21h / 31h
│  ├─ launcher/            RetroLaunch.exe
│  │  ├─ main.cpp                  CLI, inspection, launch-path routing
│  │  ├─ ProcessLauncher.{h,cpp}   Detours create-suspended + inject + payload + resume
│  │  └─ Win16Host.{h,cpp}         runs NE programs on the Win16 engine, real host windows
│  └─ shim/                RetroShim.dll (injected)
│     ├─ dllmain.cpp               attach/detach, config load, per-module transactions
│     ├─ ShimState.h               config + module handle for hook modules
│     ├─ DisplayContext.{h,cpp}    virtual mode, managed windows, cursor, caller filter
│     ├─ Pacing.{h,cpp}            the one frame pacer shared by GDI, DirectDraw and D3D9
│     ├─ gfx/
│     │  ├─ VtableHook.{h,cpp}     lazy COM vtable patching, per interface version
│     │  ├─ ModuleWatch.{h,cpp}    hooks DLLs when they load (static import or LoadLibrary)
│     │  ├─ DDraw.cpp              DirectDraw exclusive-mode containment + virtual primary
│     │  ├─ D3D8.cpp               Direct3D 8 containment + pacing, or the d3d8to9 bridge
│     │  ├─ D3D9.cpp               Direct3D 9 fullscreen containment, Present pacing, 9On12
│     │  ├─ D3DCommon.h            containment shared by the D3D8 and D3D9 hooks
│     │  └─ Graphics.{h,cpp}       module entry points, diagnostics
│     ├─ Log.{h,cpp}               DllMain-safe logger
│     ├─ RetroShim.def             exports ordinal #1 (required by Detours)
│     └─ hooks/
│        ├─ Hooks.h                module entry points, type-checked attach helpers
│        ├─ StorageHooks.cpp       GetDiskFreeSpace(Ex)A/W
│        ├─ MemoryHooks.cpp        GlobalMemoryStatus(Ex)
│        ├─ ProcessHooks.cpp       CreateProcessA/W + CreateProcessInternalW → child injection
│        ├─ DisplayHooks.cpp       display modes, window sandbox, cursor, input coordinates
│        └─ RenderHooks.cpp        GDI scaling + presentation frame pacing
├─ third_party/
│  └─ d3d8to9/             crosire/d3d8to9 (BSD-2), unmodified sources + our CMake wrapper
└─ tests/
   ├─ Check.h              tiny test harness
   ├─ ExeFormatTests.cpp   parser unit tests (synthetic images + real system DLLs)
   ├─ ClampPolicyTests.cpp clamping rules: known values, property sweeps, idempotence
   ├─ DisplayMathTests.cpp viewports, aspect, mapping round-trips, window classification
   ├─ FramePacingTests.cpp scheduler with a fake clock + one real-time measurement
   ├─ PixelConvertTests.cpp format conversion, channel expansion, CRC
   ├─ win16/               Win16 engine: CPU instruction tests, NE/loader/runtime tests,
   │                       Asm16 + NeBuilder (synthetic NE executables), sample generator
   └─ ShimProbe.cpp, ProbeDisplay.cpp, ProbeGraphics.cpp, ProbeD3D8.cpp
                           x86 target driven by the end-to-end tests (see tests/CMakeLists.txt)
```

## Win16 engine

`RetroLaunch program.exe` on a 16-bit NE executable runs it in-process:

- **Loader:** each segment gets an LDT selector (Windows-style values, `0x0107`,
  `0x010F`, …). Segment data is copied from the file (iterated segments are expanded)
  and relocations are applied, including chained fixups, internal/movable targets, and
  imports by ordinal or name. DGROUP is laid out like Windows does it (static data,
  then the stack, then the local heap). A PSP carries the command line and the DOS
  environment, and `hModule` is a copy of the NE header. The task starts with the
  registers the Windows loader sets (DS = DGROUP, DI = hInstance, BX/CX = stack/heap
  sizes, ES = PSP).
- **Memory:** every `selector:offset` access goes through the virtual LDT and is
  checked like on a 286. A null or absent selector, an offset past the limit, a
  write to code or an execute from data raises #GP.
- **CPU:** a 286-class interpreter covering the 8086/80186 integer instruction set
  with protected-mode segment loads. Faults stop the task with the exact `CS:IP`
  and cause.
- **System DLLs:** KERNEL, USER and GDI are built in (`Kernel.cpp`, `User.cpp`,
  `Gdi.cpp`), plus WIN87EM's `__fpMath` start-up and shut-down calls. Imports are
  resolved to `module-selector:ordinal` on a *host* segment, so a far call runs the C++
  implementation.
  - **API catalog:** every export of the Windows 3.x system DLLs is catalogued, with
    its name, calling convention, parameters and constants (`ApiCatalog.cpp`,
    generated from Wine's `.spec` files by `tools/gen_win16_catalog.py`; only these
    interface facts are used). The catalog covers KERNEL, USER, GDI, KEYBOARD, SOUND,
    MMSYSTEM, SHELL, COMMDLG, WIN87EM, LZEXPAND, VER, TOOLHELP, SYSTEM, DDEML, WINSOCK,
    DISPLAY, MOUSE, COMM and WING.
  - **Other DLLs load as stubs:** a program importing a DLL that isn't built in
    (SHELL, MMSYSTEM, or the game's own) still loads, and only a call into it stops
    the task.
  - **Missing APIs stop cleanly, by name:** calling an API that isn't implemented yet
    stops the task with e.g.
    `USER.216 (GetDlgItem) is not implemented yet (returning to 0127:04A2)`, rather
    than crashing.
  - **Imported constants:** `__AHINCR`, `__AHSHIFT` and `__WINFLAGS` resolve to their
    values. The real-mode memory selectors (`__A000H`, `__0040H`, …) resolve to null,
    with a note.
  - `GetProcAddress` only finds implemented functions, so a program that probes for an
    API sees it missing.

  Implemented so far:
  - KERNEL: `InitTask`, `FatalExit`, `FatalAppExit`, `GetVersion`, `GetWinFlags`
    (286, standard mode, no coprocessor), `WaitEvent`, `Yield`, `DOS3Call`.
    - Global heap: `GlobalAlloc`, `GlobalReAlloc`, `GlobalLock`, `GlobalUnlock`,
      `GlobalFree`, `GlobalSize`, `GlobalHandle`, `GlobalFlags`, `GlobalCompact`,
      `GetFreeSpace`, `LockSegment`/`UnlockSegment`.
    - Local heap: `LocalInit`, `LocalAlloc`, `LocalReAlloc`, `LocalFree`, `LocalLock`,
      `LocalUnlock`, `LocalSize`, `LocalHandle`, `LocalFlags`, `LocalCompact`.
    - Modules: `GetModuleHandle`, `GetModuleFileName`, `GetProcAddress`,
      `MakeProcInstance`/`FreeProcInstance`, `LoadLibrary` (built-in modules only),
      `FreeLibrary`, `GetInstanceData`, `GetCurrentTask`, `GetCurrentPDB`,
      `GetDOSEnvironment`.
    - Strings: `lstrcpy`, `lstrcpyn`, `lstrcat`, `lstrlen`, `OutputDebugString`.
    - Files: `OpenFile`, `_lopen`, `_lread`, `_llseek`, `_lclose` (`_lcreat`/`_lwrite`
      are refused), plus `GetWindowsDirectory`, `GetSystemDirectory` and `SetErrorMode`.
    - INI files: `GetProfileInt`/`String`, `GetPrivateProfileInt`/`String`,
      `WriteProfileString`, `WritePrivateProfileString`.
    - Resources: `FindResource`, `LoadResource`, `LockResource`, `FreeResource`,
      `SizeofResource`.
  - USER: `RegisterClass`, `CreateWindow`/`CreateWindowEx`, `ShowWindow`,
    `UpdateWindow`, `DestroyWindow`, `DefWindowProc`, `GetMessage`, `PeekMessage`,
    `PostMessage`, `SendMessage`, `TranslateMessage`, `DispatchMessage`,
    `PostQuitMessage`, `GetSystemMetrics` (a 640×480 screen), `GetTickCount`,
    `SetTimer`, `KillTimer`, `LoadBitmap`, `LoadString`, `LoadIcon`/`LoadCursor`
    (placeholder handles), `InitApp`, `MessageBox` (printed to the console).
  - USER painting: `BeginPaint`/`EndPaint` (real `PAINTSTRUCT`), `GetDC`/`ReleaseDC`,
    `InvalidateRect`/`ValidateRect`, `GetClientRect`, `FillRect`.
  - GDI: `CreateCompatibleDC`, `DeleteDC`, `CreateBitmap`, `CreateCompatibleBitmap`,
    `CreateSolidBrush`, `CreatePen`, `SelectObject`, `DeleteObject`, `GetStockObject`,
    `BitBlt`, `StretchBlt`, `PatBlt`, `Rectangle`, `SetPixel`, `GetPixel`, `TextOut`,
    `SetTextColor`, `GetTextColor`, `SetBkColor`, `SetBkMode`.
- **Global heap:** each `GlobalAlloc` block is its own LDT segment. As in protected-mode
  Windows 3.x, a fixed block's handle is its selector, and a moveable block's handle is
  the selector with bit 0 cleared.
- **Windows and messages:** window classes, `HWND16` handles and a posted-message queue
  live in the engine. Every top-level window gets a real host window, with a
  bidirectional `HWND16` ↔ host map. `RetroLaunch` lays these out with the same
  `DisplayMath` as the display sandbox: a window covering the 640×480 16-bit screen
  becomes borderless fullscreen with the screen integer-scaled; others become captioned
  windows scaled by a whole number. Closing, keys and mouse come back as 16-bit
  messages, in the 16-bit window's coordinates. `WM_QUIT` is retrieved only after
  everything else, as in Windows.
- **Local heaps:** `LocalAlloc` works on the heap in the caller's DS, normally DGROUP's,
  which starts after the stack. A moveable block's handle is the offset of a real Win16
  handle-table entry (address, flags, lock count), so code that dereferences a handle
  finds the pointer. When the heap is full, DGROUP grows, up to 64 KB, keeping its
  selector (`Memory::Resize`, which `GlobalReAlloc` uses too).
- **Files:** the program sees its own directory, read-only.
  - Relative paths, `C:\WINDOWS\…` and `C:\WINDOWS\SYSTEM\…` all map into that
    directory, and the program's own absolute paths work.
  - Anything that leads outside it is refused, including `..`, other drives and links
    or junctions.
  - Opening for writing, creating, deleting and renaming are refused with "access
    denied" and a note. Saving belongs in a per-game save directory, which comes later.
  - The same layer serves KERNEL's file functions and the C runtime's INT 21h calls:
    open, read, seek, close, device info, attributes, date/time, current directory,
    free space, and find (always empty).
  - INI files are read from disk, and writes are kept in memory for the rest of the run.
- **Resources:** the NE resource table is parsed at load: integer or named types
  (`RT_BITMAP`, `RT_ICON`, `RT_CURSOR`, `RT_MENU`, `RT_STRING`, custom types) and
  names. As in Windows 3.x, an `HRSRC` is the resource's `NAMEINFO` offset in the table,
  and `LoadResource` copies the data into a moveable global block. Loading the same
  resource again shares that block, with a usage count that `FreeResource` decrements.
  Names match case-insensitively, and `"#12"` means id 12. `LoadBitmap` turns a
  packed-DIB resource into a device-dependent bitmap (two-colour DIBs become monochrome
  bitmaps, as in Windows), and refuses malformed or truncated DIBs. `LoadString` reads
  the 16-string blocks.
- **Timers:** `SetTimer` supports both styles: `WM_TIMER` to a window, and a
  `TIMERPROC`, which `DispatchMessage` calls on the interpreter as
  `(hwnd, WM_TIMER, id, dwTime)`. It only calls procedures of live timers, since
  `lParam` is just a number. Timers are due times, not queued messages. Like
  `WM_PAINT`, a `WM_TIMER` is synthesized when nothing else is queued, at most one
  per timer. After a stall, missed ticks are skipped rather than delivered as a burst.
  `GetMessage` sleeps until input arrives or the next timer is due. The Win32 host
  waits on a high-resolution waitable timer together with its message queue, so ticks
  land within about a millisecond, and an idle task uses no CPU. Intervals below 55 ms
  run at 55 ms: Windows 3.x timers ticked with the 18.2 Hz PC timer, and programs
  written for it assume that rate.
- **Text:** `TextOut` draws with the DC's text colour, background colour and mode.
  Strings are converted from code page 1252 (Win16's ANSI), whatever the host's code
  page. Each `GetDC` starts from the default colours, as with Windows' common DCs.
- **Callbacks:** `SendMessage`, `DispatchMessage` and `CreateWindow` (`WM_NCCREATE`/
  `WM_CREATE` with a real `CREATESTRUCT`, then `WM_SIZE`/`WM_MOVE`) call the 16-bit
  window procedure on the interpreter. `Cpu::CallFar` pushes the Pascal arguments and
  a return address into a private trap segment, sets DS = AX = the instance's DGROUP
  (what exported-callback prologues expect), runs until the procedure's `RETF` hits
  the trap, then restores the caller's registers and returns `DX:AX`. Callbacks nest
  (a WndProc can send messages, create or destroy windows). A fault, exit or budget
  stop inside one unwinds cleanly.
- **GDI and painting:**
  - Every 16-bit GDI handle wraps a host (Win32) GDI object, through a bidirectional map.
    That lets `SelectObject` return a proper 16-bit handle even for objects the host made,
    such as a new DC's default bitmap. Win16 and Win32 share ROP codes, COLORREFs and
    bitmap row padding.
  - Win16 rules that modern GDI relaxes are enforced by the engine, for example "an object
    selected into a DC can't be deleted".
  - Each top-level window has a 32-bit back buffer at its 16-bit size. `GetDC` and
    `BeginPaint` draw into it; each call saves the DC state, and `ReleaseDC`/`EndPaint`
    restore it.
  - Windows keep an update region. `WM_PAINT` is generated when nothing else is queued
    and stays pending until validated. `BeginPaint` clips to the region and sends
    `WM_ERASEBKGND`, which `DefWindowProc` answers with the class brush.
- **Presentation:** the message pump is the frame boundary. Back buffers drawn since the
  last call are paced with the same `FrameScheduler` as the display sandbox (`--fps-cap`,
  default 60), then handed to the host. The host scales each one onto the screen with a
  single nearest-neighbour blit into the integer-scaled viewport, so there's no tearing.
  A `PeekMessage` game loop therefore runs at the cap.
- **Interrupts:** INT 21h (exit, version, console output, drive, PSP), INT 20h, and
  INT 31h DPMI queries (segment base, version).

Launcher exit code: the program's own (INT 21h/4Ch, `FatalExit`, the `WM_QUIT` code if
the program exits with it), or 6 if the task stopped on a fault, an unimplemented API, or
while waiting for input that can never arrive. `--hidden` creates a 16-bit program's host
windows without showing them; the tests use it.

### Running a real 16-bit program

```
RetroLaunch --windowed C:\Games\SKI\SKIFREE.EXE
RetroLaunch --trace-win16 C:\Games\SKI\SKIFREE.EXE > trace.txt
```

- `--inspect` shows the NE header without running anything.
- `--trace-win16` logs every API call: the return address, `MODULE.ordinal`, the name,
  the arguments decoded from the catalog (strings quoted, `MAKEINTRESOURCE` ids as
  `#n`) and the result. Calls made from inside a callback are indented under the call
  that led to them:
  ```
  [trace] 012F:0134 USER.41 CreateWindow("RetroWin", "Win16 Window", 00CF0000, -32768, ...)
  [trace]   012F:02BF USER.107 DefWindowProc(2004, 0081, 0000, 010F0000) = 0000:0001
  [trace]   = 2004
  ```
- When the program reaches something the engine doesn't have, the last line says what,
  with its catalog name:
  `Win16 exit  : stopped at an unimplemented API - SHELL.22 (ShellAbout) is not implemented yet (returning to 0127:04A2)`.
- `--stub-missing` is for triage: a missing Pascal function with known parameters
  returns 0 and the program carries on, with one note per function. Functions whose
  arguments can't be removed safely still stop: register-based ones, and those with
  unknown parameters. The program may misbehave afterwards, but you see everything it
  needs in one run instead of one run per gap.
- `--exact-timers` makes `SetTimer` honour intervals below 55 ms.
- Writing files isn't supported yet: a game that saves (high scores, settings in its
  own files) gets "access denied", and the launcher notes it once. INI settings work
  for the run.

Not yet: fonts (`CreateFont`; text uses the host's default font) and text metrics,
lines and other shapes, regions, palettes (8-bit games), mapping modes, non-client areas
(a 16-bit window is all client area), child windows (they get no back buffer), child
controls and system classes (`BUTTON`, `EDIT`, …), menus, dialogs, icons and cursors
from resources (they're parsed, but `LoadIcon`/`LoadCursor` return placeholders),
system bitmaps (`OBM_xxx`), `SetSystemTimer`, sound (MMSYSTEM, SOUND), writing files,
loading NE DLLs (a game's own DLLs load as stubs), huge (> 64 KB) global blocks, 386
instructions (`66h`/`67h` prefixes), 286 system instructions (`0Fh`), floating point
(WIN87EM's emulation interrupts and x87 instructions stop the task with a clear
message), and `Catch`/`Throw`.

## Shim modules

| Module | Hooks | Behaviour |
|---|---|---|
| storage | `GetDiskFreeSpaceA/W` | Keeps the real cluster size, reduces cluster counts so `spc × bps × clusters` fits a signed 32-bit int (≤ 0x7FFF0000). |
| | `GetDiskFreeSpaceExA/W` | Caps at `--disk-cap` (default 8 GiB), then makes each value *truncation-safe*: its low DWORD stays in [1 GiB, 2 GiB), so code that keeps only 32 bits still sees ≥ 1 GiB. The 8 GiB default reports as ~6 GiB. Real values below 2 GiB pass through unchanged, and avail ≤ free ≤ total is enforced. |
| memory | `GlobalMemoryStatus(Ex)` | Physical memory capped at `--mem-cap` (default 1 GiB), page file at 2× that (≤ 2 GiB), address space at 2 GiB. Available figures are scaled by the same ratio, so memory load stays true. The legacy API is sourced from the Ex figures because its own fields saturate on large machines. |
| child-process | `CreateProcessA/W`, `CreateProcessInternalW` | Children are created suspended. Same-architecture (x86) children get RetroShim plus the config payload, other architectures run untouched. The internal hook catches `ShellExecuteEx`, `CreateProcessAsUser`, etc. A thread-local guard makes the outermost hook the only one that acts. |
| display | `ChangeDisplaySettings(Ex)A/W` | Records a *virtual* mode instead of changing the real one, so the desktop and other monitors are never touched. `CDS_TEST` validates, `(NULL, 0)` restores, and the game's windows get `WM_DISPLAYCHANGE`. |
| | `EnumDisplaySettings(Ex)A/W`, `GetSystemMetrics`, `GetDeviceCaps` | Report the virtual mode. The mode list offers classic 8/16/32-bit modes that modern drivers no longer list. |
| | `CreateWindowExA/W`, `SetWindowPos`, `MoveWindow`, `SetWindowLongA/W` | A top-level window covering the virtual screen (at creation, when resized to it, or already covering it when the mode changes) becomes *managed*. It is either borderless and covering the monitor (so Windows hides the taskbar) or, with `--windowed`, a fixed captioned window. It is created per-monitor DPI aware so scaling stays crisp. The game can't move, resize or make it topmost, and swapping its window procedure keeps the sandbox's subclass on top. |
| | `GetWindowRect`, `GetClientRect`, `ClientToScreen`, `ScreenToClient`, `GetMessage`/`PeekMessage` | The window reports itself at (0,0) with the virtual size. Mouse message coordinates are mapped into virtual space (letterbox bars clamp to the edge), and sent `WM_SIZE` carries the virtual size. |
| | `GetCursorPos`, `SetCursorPos`, `ClipCursor`, `GetClipCursor`, `ShowCursor` | The cursor lives in virtual coordinates, and set/get round-trips exactly. The game's clip, or the whole viewport while it hides the cursor, applies only while the game is in the foreground and is released on alt-tab. |
| render | `GetDC`, `GetDCEx`, `GetWindowDC`, `BeginPaint`, `InvalidateRect` | DCs for a managed window get a GDI world transform (virtual → viewport, nearest-neighbour) and a viewport clip, so all GDI drawing is scaled. Integer scale factors are used when they fit, e.g. 640×480 → 4× on 4K. `SetDIBitsToDevice`, which GDI never scales, is rewritten as `StretchDIBits`. |
| | `BitBlt`, `StretchBlt`, `StretchDIBits`, `SetDIBitsToDevice`, `SwapBuffers`, `wglSwapBuffers` | Frame pacing at presentation. A blit covering ≥ 75% of the window, or a buffer swap, waits for the next slot of the `--fps-cap` cadence, using a high-resolution waitable timer plus a sub-millisecond QPC spin. Late frames never trigger a catch-up burst, and a frame through `SwapBuffers` → `wglSwapBuffers` is paced once. |

| graphics: DirectDraw | `DirectDrawCreate(Ex)`, then `IDirectDraw`/`2`/`4`/`7` and `IDirectDrawSurface`…`7` methods | `DDSCL_EXCLUSIVE \| DDSCL_FULLSCREEN` becomes `DDSCL_NORMAL`, and `SetDisplayMode` sets the virtual mode, so the real desktop never changes. The primary and back buffer are system-memory surfaces in the virtual format (8-bit palettized included), reported to the game as a flipping primary chain. Surfaces created without a pixel format get the virtual depth, not the desktop's. `EnumDisplayModes` and `GetDisplayMode` report classic and virtual modes. |
| | `Flip`, `Blt`/`BltFast` to the primary, `Unlock`, `ReleaseDC`, `IDirectDrawPalette::SetEntries`, `WaitForVerticalBlank` | Presenting converts the primary to 32-bit BGRA (palette lookup for 8-bit) and draws it into the managed window's integer-scaled viewport. `Flip` and whole-frame blits are paced; partial updates are coalesced to one per frame period and flushed from the message pump. Palette changes (fades) re-present immediately. `WaitForVerticalBlank` is paced to the cap instead of the real 144/165 Hz refresh, and a `Flip` right after it counts as the same frame. |
| graphics: Direct3D 9 | `Direct3DCreate9`, `IDirect3D9::CreateDevice`, `IDirect3DDevice9::Reset`/`Present`/`GetDisplayMode` | A fullscreen device is created windowed in the managed window, and its back buffer becomes the virtual mode. `Present` is paced and aimed at the viewport, with black bars. `--d3d9on12` routes `Direct3DCreate9` through `Direct3DCreate9On12`, so D3D9 runs on D3D12 queues. |
| graphics: Direct3D 8 | `Direct3DCreate8`, `IDirect3D8::CreateDevice`, `IDirect3DDevice8::Reset`/`Present`/`GetDisplayMode` | By default native `d3d8.dll` gets the same treatment as D3D9: fullscreen becomes windowed in the sandbox, and `Present` is paced and letterboxed. With `--d3d8to9`, `Direct3DCreate8` returns the vendored [d3d8to9](third_party/d3d8to9) bridge, which implements D3D8 on D3D9. It gets its D3D9 through the hooked `Direct3DCreate9`, so the D3D9 containment and pacing apply, and `--d3d8to9 --d3d9on12` runs a D3D8 game on D3D12. |

The display and render hooks only virtualize for **game code**. Calls coming from
DLLs under the Windows directory (user32 internals, DirectDraw's own GDI use) see
the real system. DirectDraw and Direct3D 9 are handled by vtable hooks on the
objects the game creates. `ddraw.dll` and `d3d9.dll` are hooked whenever they
load, including a `LoadLibrary` long after startup. Diagnostics exports:
`RetroShimIsModuleActive("display" | "ddraw" | "d3d9" | …)` and
`RetroShimGetPresentStats` (frames presented and a CRC of the last one).

Known limits:
- A DirectDraw primary created with `DDSCAPS_3DDEVICE` (Direct3D 3–7 rendering straight
  to the primary) can't live in system memory. That game gets the real exclusive mode
  it asked for, and the log says so. Direct3D 9Ex isn't intercepted yet.
- The d3d8to9 bridge translates D3D8 shaders with D3DX (`d3dx9_43.dll`, from the DirectX
  End-User Runtime). If it's missing, the bridge shows its own message box, and games
  that use shaders won't render correctly. Fixed-function games don't need it.
- DirectDraw presents through GDI (`StretchDIBits`) on the CPU: fine for 640×480-class
  games, but a GPU presenter would be cheaper at 4K.
- Direct3D 9 windowed `Present` stretches with the driver's filter, so the image is
  integer sized but may be slightly soft.
- 8-bit palettized *GDI* games are reported the mode but not palette-emulated (DirectDraw
  8-bit is).
- A window created before the mode change and not DPI aware is scaled by DWM on top of
  the integer scale when the desktop is above 100% scaling (slightly soft).
- `MapWindowPoints` and `SystemParametersInfo(SPI_GETWORKAREA)` are not yet virtualized.

Each module attaches in its own Detours transaction, so a failure in one doesn't
block the others. The shim log (`logs/<game>.log`) records each module's status and the
first real → clamped values seen by every hook.

## Requirements

- Visual Studio 2022 (17.x) or 2026 (18.x) with the **Desktop development with C++**
  workload (MSVC v143+ x86/x64 tools, Windows 11 SDK, C++ CMake tools, vcpkg)
- CMake 3.25+ (bundled with VS)
- vcpkg: the copy bundled with VS works. `VCPKG_ROOT` must point to it; the VS
  IDE and "Developer PowerShell for VS" set it automatically.

## Build

From a Developer PowerShell for VS:

```powershell
cmake --preset x86                 # first run fetches and builds Detours via vcpkg
cmake --build --preset x86-release
ctest --preset x86-release
```

From a plain shell, set `VCPKG_ROOT` first, for example:

```powershell
$env:VCPKG_ROOT = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg"
```

In the Visual Studio IDE, use **File → Open → Folder** and select the `x86` preset.

Outputs go to `build/x86/bin/<Config>/`: `RetroLaunch.exe` and `RetroShim.dll`
side by side, with shim logs in `logs/`.

### Compiler settings (and why)

| Setting | Value | Reason |
|---|---|---|
| Platform | **Win32 (x86)** | Games are 32-bit. The injected DLL must match the target's bitness, and an x86 launcher injects directly without Detours' rundll32 bitness helper. CMake refuses to configure x64. |
| CRT | `/MT` / `/MTd` (static) | The shim loads from arbitrary game folders, so it has no VC++ redist dependency. This matches the vcpkg triplet `x86-windows-static`. |
| Standard | C++20, `/permissive-` | |
| Warnings | `/W4` | Currently builds clean. |
| Defines | `UNICODE`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `_WIN32_WINNT=0x0A00` | |

## Usage

```
RetroLaunch [options] <program.exe> [program arguments...]
  --inspect        Print executable header info and exit
  --wait           Wait for exit and return the program's exit code
  --hidden         16-bit programs: create their windows but never show them
  --trace-win16    16-bit programs: log every API call (arguments, result, return address)
  --stub-missing   16-bit programs: missing APIs with known parameters return 0
                   instead of stopping the program (logged once each)
  --exact-timers   16-bit programs: timers honour intervals below Windows 3.x's 55 ms
  --no-shim        Launch without injection (baseline comparison)
  --shim <path>    Shim DLL (default: RetroShim.dll beside RetroLaunch)
  --cwd <dir>      Working directory (default: the program's folder)
  --fps-cap <n>    Frame rate cap at presentation, 0 = unlimited (default 60)
  --disk-cap <MiB> Largest disk size/free space reported (64-16777216, default 8192)
  --mem-cap <MiB>  Largest physical memory reported (16-2047, default 1024)
  --no-clamp-disk  Don't hook GetDiskFreeSpace(Ex)
  --no-clamp-mem   Don't hook GlobalMemoryStatus(Ex)
  --no-propagate   Don't inject the shim into child processes
  --windowed       Show fullscreen games in a captioned window, not borderless fullscreen
  --no-integer-scaling    Fill the screen with fractional (still 4:3) scaling
  --no-display-sandbox    Let the game change the real display mode (pacing still applies)
  --d3d9on12       Run Direct3D 9 games on Direct3D 12 (Direct3DCreate9On12)
  --d3d8to9        Run Direct3D 8 games on Direct3D 9 (bundled d3d8to9); with
                   --d3d9on12 they end up on Direct3D 12
```

## Third-party code

- [Microsoft Detours](https://github.com/microsoft/Detours) (MIT), via vcpkg.
- [d3d8to9](https://github.com/crosire/d3d8to9) by Patrick Mours (BSD-2-Clause),
  vendored unmodified in [third_party/d3d8to9](third_party/d3d8to9) at a pinned commit.
  Its licence is in [third_party/d3d8to9/LICENSE.md](third_party/d3d8to9/LICENSE.md).

Exit codes: 2 usage, 3 unreadable image, 4 unsupported format, 5 launch failure,
6 Win16 task stopped (fault, unimplemented API, or blocked waiting for input). With `--wait`, and always for
16-bit programs, the launcher returns the program's own exit code.
