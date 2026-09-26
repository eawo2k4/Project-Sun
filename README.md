# Project Sun

Project Sun is a software/layer that will let you run Windows 3.x–XP games.

RetroRunner is an in-process compatibility runner for vintage Windows games (Win 3.x → XP) on
Windows 11 x64. It runs games natively: no VM, no whole-PC emulator, no Wine.
32-bit games run under WOW64 with an injected shim DLL. 16-bit NE programs
will go through an embedded Win16 engine (planned).

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
│  ├─ launcher/            RetroLaunch.exe
│  │  ├─ main.cpp                  CLI, inspection, launch-path routing
│  │  └─ ProcessLauncher.{h,cpp}   Detours create-suspended + inject + payload + resume
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
   └─ ShimProbe.cpp, ProbeDisplay.cpp, ProbeGraphics.cpp, ProbeD3D8.cpp
                           x86 target driven by the end-to-end tests (see tests/CMakeLists.txt)
```

Planned modules: `src/win16/` (NE loader + CPU engine + thunks).

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

Exit codes: 2 usage, 3 unreadable image, 4 unsupported format, 5 launch failure.
With `--wait`, the launcher returns the program's own exit code.
