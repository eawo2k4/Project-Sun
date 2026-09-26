#pragma once

// Minimal test harness shared by the unit test executables.

#include <cstdio>

namespace test {

inline int g_failures = 0;

struct Case {
    const char* name;
    void (*fn)();
};

template <size_t N>
int RunAll(const Case (&cases)[N]) {
    for (const Case& c : cases) {
        std::printf("[ RUN ] %s\n", c.name);
        std::fflush(stdout);  // a crash in a test still shows which one
        c.fn();
        std::fflush(stdout);
    }
    std::printf(g_failures ? "%d check(s) FAILED\n" : "All tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}

}  // namespace test

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++test::g_failures;                                             \
        }                                                                   \
    } while (0)
