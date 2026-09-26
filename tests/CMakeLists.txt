# --- Unit tests ------------------------------------------------------------------

add_executable(ExeFormatTests ExeFormatTests.cpp Check.h)
target_link_libraries(ExeFormatTests PRIVATE RetroCore)
add_test(NAME ExeFormat COMMAND ExeFormatTests)

add_executable(ClampPolicyTests ClampPolicyTests.cpp Check.h)
target_link_libraries(ClampPolicyTests PRIVATE RetroCore)
add_test(NAME ClampPolicy COMMAND ClampPolicyTests)

add_executable(DisplayMathTests DisplayMathTests.cpp Check.h)
target_link_libraries(DisplayMathTests PRIVATE RetroCore)
add_test(NAME DisplayMath COMMAND DisplayMathTests)

add_executable(FramePacingTests FramePacingTests.cpp Check.h)
target_link_libraries(FramePacingTests PRIVATE RetroCore)
add_test(NAME FramePacing COMMAND FramePacingTests)

add_executable(PixelConvertTests PixelConvertTests.cpp Check.h)
target_link_libraries(PixelConvertTests PRIVATE RetroCore)
add_test(NAME PixelConvert COMMAND PixelConvertTests)

# --- End-to-end tests: RetroLaunch -> RetroShim -> ShimProbe ----------------------

# ddraw is linked (loaded at startup); d3d9 is loaded with LoadLibrary at run
# time, to cover both ways games pull in DirectX.
add_executable(ShimProbe ShimProbe.cpp ProbeDisplay.cpp ProbeGraphics.cpp ProbeD3D8.cpp
    ProbeCommon.h)
target_link_libraries(ShimProbe PRIVATE RetroCore shell32 ole32 opengl32 ddraw dxguid)
# D3D8 interface declarations only (the SDK dropped d3d8.h); not the bridge itself.
target_include_directories(ShimProbe PRIVATE ${PROJECT_SOURCE_DIR}/third_party/d3d8to9/source)

set(LAUNCH $<TARGET_FILE:RetroLaunch> --wait)
set(PROBE $<TARGET_FILE:ShimProbe>)

# Injection itself.
add_test(NAME ShimInjection COMMAND ${LAUNCH} ${PROBE})
add_test(NAME ShimProbeWithoutShim COMMAND ${LAUNCH} --no-shim ${PROBE})
set_tests_properties(ShimProbeWithoutShim PROPERTIES WILL_FAIL TRUE)

# Storage clamping (default and custom caps).
add_test(NAME StorageClampDefault COMMAND ${LAUNCH} ${PROBE} disk 8192)
add_test(NAME StorageClampLowCap COMMAND ${LAUNCH} --disk-cap 512 ${PROBE} disk 512)

# Memory clamping. A non-default cap proves the config payload arrived.
add_test(NAME MemoryClampDefault COMMAND ${LAUNCH} ${PROBE} memory 1024)
add_test(NAME MemoryClampCustom COMMAND ${LAUNCH} --mem-cap 768 ${PROBE} memory 768)
add_test(NAME MemoryClampDisabled COMMAND ${LAUNCH} --no-clamp-mem ${PROBE} memory 1024)
set_tests_properties(MemoryClampDisabled PROPERTIES WILL_FAIL TRUE)

# Child process propagation: the child must see the custom cap, which it can
# only get from the config payload the parent's shim forwarded.
add_test(NAME ChildViaCreateProcessW COMMAND ${LAUNCH} --mem-cap 768 ${PROBE} spawn-w memory 768)
add_test(NAME ChildViaCreateProcessA COMMAND ${LAUNCH} --mem-cap 768 ${PROBE} spawn-a memory 768)
add_test(NAME ChildViaShellExecute COMMAND ${LAUNCH} --mem-cap 768 ${PROBE} spawn-shell memory 768)
add_test(NAME GrandchildPropagation COMMAND ${LAUNCH} --mem-cap 768 ${PROBE} spawn-w spawn-a memory 768)
add_test(NAME Child64BitUnaffected COMMAND ${LAUNCH} ${PROBE} spawn64)
add_test(NAME ChildPropagationDisabled COMMAND ${LAUNCH} --no-propagate ${PROBE} spawn-w loaded)
set_tests_properties(ChildPropagationDisabled PROPERTIES WILL_FAIL TRUE)

# Display sandbox. These probes refuse to call ChangeDisplaySettings unless
# the shim reports the sandbox active, so they can never change the real
# desktop resolution. Test windows stay hidden.
add_test(NAME DisplayModulesDefault COMMAND ${LAUNCH} ${PROBE} module display 1)
add_test(NAME DisplayModulesDisabled COMMAND ${LAUNCH} --no-display-sandbox ${PROBE} module display 0)
add_test(NAME DisplayModeSandbox COMMAND ${LAUNCH} ${PROBE} display)
add_test(NAME WindowSandboxFullscreen COMMAND ${LAUNCH} ${PROBE} window)
add_test(NAME WindowSandboxWindowed COMMAND ${LAUNCH} --windowed ${PROBE} window windowed)
add_test(NAME WindowAdoption COMMAND ${LAUNCH} ${PROBE} adopt)

# Frame pacing at presentation (GDI blits, SwapBuffers, sandboxed DIB presents).
add_test(NAME FramePacing60 COMMAND ${LAUNCH} ${PROBE} render 60)
add_test(NAME FramePacing30 COMMAND ${LAUNCH} --fps-cap 30 ${PROBE} render 30)
add_test(NAME FramePacingOff COMMAND ${LAUNCH} --fps-cap 0 ${PROBE} render 0)
add_test(NAME FramePacingWithoutSandbox COMMAND ${LAUNCH} --no-display-sandbox ${PROBE} render 60)

# DirectDraw / Direct3D 9 containment. The probes refuse to go fullscreen
# unless the shim reports both the API hooks and the display sandbox active.
add_test(NAME DirectDraw8BitPalettized COMMAND ${LAUNCH} ${PROBE} ddraw 60)
add_test(NAME DirectDraw7At16Bit COMMAND ${LAUNCH} ${PROBE} ddraw7 60)
add_test(NAME DirectDrawPacing30 COMMAND ${LAUNCH} --fps-cap 30 ${PROBE} ddraw 30)
add_test(NAME DirectDrawUnpaced COMMAND ${LAUNCH} --fps-cap 0 ${PROBE} ddraw 0)
add_test(NAME Direct3D9Contained COMMAND ${LAUNCH} ${PROBE} d3d9 60)
add_test(NAME Direct3D9On12 COMMAND ${LAUNCH} --d3d9on12 ${PROBE} d3d9 60 on12)
add_test(NAME Direct3D9Unpaced COMMAND ${LAUNCH} --fps-cap 0 ${PROBE} d3d9 0)

# Direct3D 8: native containment, and the d3d8to9 bridge onto D3D9 / D3D9On12.
# The probe renders a frame and reads it back, so a bridge that "succeeds" but
# draws nothing fails.
add_test(NAME Direct3D8Native COMMAND ${LAUNCH} ${PROBE} d3d8 60 native)
add_test(NAME Direct3D8Bridge COMMAND ${LAUNCH} --d3d8to9 ${PROBE} d3d8 60 bridge)
add_test(NAME Direct3D8BridgeOn12 COMMAND ${LAUNCH} --d3d8to9 --d3d9on12 ${PROBE} d3d8 60 bridge12)
add_test(NAME Direct3D8Unpaced COMMAND ${LAUNCH} --fps-cap 0 ${PROBE} d3d8 0 native)

# --- Win16 engine -----------------------------------------------------------------

add_executable(Win16CpuTests win16/Win16CpuTests.cpp Check.h)
target_link_libraries(Win16CpuTests PRIVATE RetroWin16)
add_test(NAME Win16Cpu COMMAND Win16CpuTests)

add_executable(Win16EngineTests win16/Win16EngineTests.cpp win16/Asm16.h win16/NeBuilder.h
    win16/TestPrograms.h Check.h)
target_link_libraries(Win16EngineTests PRIVATE RetroWin16)
add_test(NAME Win16Engine COMMAND Win16EngineTests)

# End to end: generate real NE executables, then run them through RetroLaunch.
add_executable(MakeWin16Samples win16/MakeWin16Samples.cpp win16/Asm16.h win16/NeBuilder.h
    win16/TestPrograms.h)
set(WIN16_SAMPLES ${CMAKE_CURRENT_BINARY_DIR}/win16_samples)
file(MAKE_DIRECTORY ${WIN16_SAMPLES})
add_test(NAME Win16MakeSamples COMMAND MakeWin16Samples ${WIN16_SAMPLES})
set_tests_properties(Win16MakeSamples PROPERTIES FIXTURES_SETUP win16_samples)

add_test(NAME Win16InspectRoutesToEngine COMMAND $<TARGET_FILE:RetroLaunch> --inspect ${WIN16_SAMPLES}/win16_selftest.exe)
add_test(NAME Win16SelfTestViaLauncher COMMAND ${LAUNCH} ${WIN16_SAMPLES}/win16_selftest.exe)
add_test(NAME Win16HelloViaLauncher COMMAND ${LAUNCH} ${WIN16_SAMPLES}/win16_hello.exe)
add_test(NAME Win16FatalExitViaLauncher COMMAND ${LAUNCH} ${WIN16_SAMPLES}/win16_fatalexit.exe)
add_test(NAME Win16UnimplementedViaLauncher COMMAND ${LAUNCH} ${WIN16_SAMPLES}/win16_unimplemented.exe)
set_tests_properties(Win16InspectRoutesToEngine Win16SelfTestViaLauncher Win16HelloViaLauncher
    Win16FatalExitViaLauncher Win16UnimplementedViaLauncher PROPERTIES FIXTURES_REQUIRED win16_samples)
set_tests_properties(Win16InspectRoutesToEngine PROPERTIES
    PASS_REGULAR_EXPRESSION "16-bit NE.*Win16 execution engine")
set_tests_properties(Win16HelloViaLauncher PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[win16\\] MessageBox \\[Project Sun\\]: Hello from Win16.*exited, code 0")
set_tests_properties(Win16FatalExitViaLauncher PROPERTIES
    PASS_REGULAR_EXPRESSION "fatal exit, code 7 - FatalExit\\(7\\)")
set_tests_properties(Win16UnimplementedViaLauncher PROPERTIES
    PASS_REGULAR_EXPRESSION "USER\\.7 \\(ExitWindows\\) is not implemented yet \\(returning to [0-9A-F]+:0005\\)")

# Windowing through the real Win32 host (windows created but never shown).
add_test(NAME Win16WindowViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_window.exe)
add_test(NAME Win16FullscreenViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_fullscreen.exe)
set_tests_properties(Win16WindowViaLauncher Win16FullscreenViaLauncher PROPERTIES
    FIXTURES_REQUIRED win16_samples)
set_tests_properties(Win16WindowViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION
    "host window for HWND16 [0-9A-F]+ \"Win16 Window\": 320x200 -> [0-9]+x[0-9]+ \\(x[0-9]+\\), windowed, hidden.*Win16 exit  : exited, code 0")
set_tests_properties(Win16FullscreenViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION
    "host window for HWND16 [0-9A-F]+ \"Win16 Window\": 640x480 -> [0-9]+x[0-9]+ \\(x[0-9]+\\), borderless fullscreen, hidden.*Win16 exit  : exited, code 0")

# GDI: the paint program checks its own pixels (exit 0 = all correct); the
# animation's 30 frames are presented through the real Win32 host at 60 fps.
add_test(NAME Win16PaintViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_paint.exe)
add_test(NAME Win16AnimationViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_anim.exe)
set_tests_properties(Win16PaintViaLauncher Win16AnimationViaLauncher PROPERTIES
    FIXTURES_REQUIRED win16_samples)

# Resources, timers and text: the program checks its own results (exit 0);
# its timers wait in the real host's message loop (a hidden window can't
# get input, but timers still fire).
add_test(NAME Win16ResourcesViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_resources.exe)
set_tests_properties(Win16ResourcesViaLauncher PROPERTIES FIXTURES_REQUIRED win16_samples
    PASS_REGULAR_EXPRESSION "Win16 task  : TESTAPP, 2 segments, 3 resources.*Win16 exit  : exited, code 0")

# Real-program support: C-runtime style startup (local heap, files, INI,
# WIN87EM, imported constants), the API trace, and DLLs that aren't built in
# (SHELL here): a clean stop by name, or --stub-missing carrying on.
add_test(NAME Win16CrtViaLauncher COMMAND ${LAUNCH} --hidden ${WIN16_SAMPLES}/win16_crt.exe)
add_test(NAME Win16TraceViaLauncher COMMAND ${LAUNCH} --trace-win16 ${WIN16_SAMPLES}/win16_hello.exe)
add_test(NAME Win16StubModuleViaLauncher COMMAND ${LAUNCH} ${WIN16_SAMPLES}/win16_shell.exe)
add_test(NAME Win16StubMissingViaLauncher COMMAND ${LAUNCH} --stub-missing ${WIN16_SAMPLES}/win16_shell.exe)
set_tests_properties(Win16CrtViaLauncher Win16TraceViaLauncher Win16StubModuleViaLauncher
    Win16StubMissingViaLauncher PROPERTIES FIXTURES_REQUIRED win16_samples)
set_tests_properties(Win16CrtViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION "Win16 exit  : exited, code 0")
set_tests_properties(Win16TraceViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[trace\\] [0-9A-F]+:[0-9A-F]+ KERNEL\\.91 InitTask.*\\[trace\\] [0-9A-F]+:[0-9A-F]+ USER\\.1 MessageBox\\(0000, \"Hello from Win16\", \"Project Sun\", 0000\\) = 0001")
set_tests_properties(Win16StubModuleViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION
    "SHELL\\.22 \\(ShellAbout\\) is not implemented yet \\(returning to")
# USER breadth, GDI drawing and sound through the real Win32 host: the window
# is re-laid out as borderless fullscreen when the program resizes it to 640x480.
add_test(NAME Win16UiViaLauncher COMMAND ${LAUNCH} --hidden --mute ${WIN16_SAMPLES}/win16_ui.exe)
set_tests_properties(Win16UiViaLauncher PROPERTIES FIXTURES_REQUIRED win16_samples PASS_REGULAR_EXPRESSION
    "host window for HWND16 [0-9A-F]+ \"UI\": 64x48 .*host window for HWND16 [0-9A-F]+ \"UI\": 640x480 -> [0-9]+x[0-9]+ \\(x[0-9]+\\), borderless fullscreen.*Win16 exit  : exited, code 0")
set_tests_properties(Win16StubMissingViaLauncher PROPERTIES PASS_REGULAR_EXPRESSION
    "SHELL\\.22 \\(ShellAbout\\) is not implemented yet: returning 0 \\(--stub-missing\\).*exited, code 51")
