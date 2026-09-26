#pragma once

#include <filesystem>
#include <string>

namespace retro {

// Exit code when a Win16 task stops without exiting (fault, unimplemented
// API, instruction budget).
inline constexpr int kWin16Stopped = 6;

// Runs a 16-bit NE program in-process on the Win16 engine. Returns the
// program's exit code (INT 21h/4Ch, FatalExit) or kWin16Stopped.
int RunWin16Program(const std::filesystem::path& exe, const std::string& commandLine);

}  // namespace retro
