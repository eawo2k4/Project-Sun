# --- Unit tests ------------------------------------------------------------------

add_executable(ExeFormatTests ExeFormatTests.cpp Check.h)
target_link_libraries(ExeFormatTests PRIVATE RetroCore)
add_test(NAME ExeFormat COMMAND ExeFormatTests)

add_executable(ClampPolicyTests ClampPolicyTests.cpp Check.h)
target_link_libraries(ClampPolicyTests PRIVATE RetroCore)
add_test(NAME ClampPolicy COMMAND ClampPolicyTests)

# --- End-to-end tests: RetroLaunch -> RetroShim -> ShimProbe ----------------------

add_executable(ShimProbe ShimProbe.cpp)
target_link_libraries(ShimProbe PRIVATE shell32 ole32)

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
