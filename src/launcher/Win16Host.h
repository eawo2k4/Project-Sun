#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace retro {

// Exit code when a Win16 task stops without exiting (fault, unimplemented
// API, blocked on input, instruction budget).
inline constexpr int kWin16Stopped = 6;

struct Win16Options {
    // Create the host windows but never show them (automated tests).
    bool hidden = false;
    // Presented frames per second at most (0 = unpaced), as for 32-bit games.
    uint32_t fpsCap = 60;
};

// Runs a 16-bit NE program in-process on the Win16 engine, with its top-level
// windows as real windows. Returns the program's exit code (INT 21h/4Ch,
// FatalExit) or kWin16Stopped.
int RunWin16Program(const std::filesystem::path& exe, const std::string& commandLine,
                    const Win16Options& options);

}  // namespace retro
