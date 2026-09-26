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

# --- End-to-end tests: RetroLaunch -> RetroShim -> ShimProbe ----------------------

add_executable(ShimProbe ShimProbe.cpp ProbeDisplay.cpp ProbeCommon.h)
target_link_libraries(ShimProbe PRIVATE RetroCore shell32 ole32 opengl32)

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
