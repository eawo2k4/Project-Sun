#pragma once

// Shared bits of the ShimProbe test target (ShimProbe.cpp, ProbeDisplay.cpp).

#include <cstdint>
#include <cstdio>

namespace probe {

inline int g_failures = 0;

inline void Expect(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

inline int Result() { return g_failures ? 1 : 0; }

// ProbeDisplay.cpp
int ProbeModule(const char* name, bool expectActive);
int ProbeDisplayModes();
int ProbeWindow(bool windowed);
int ProbeAdoption();
int ProbeRender(uint32_t fps);

}  // namespace probe
