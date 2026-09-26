#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

#include "retro/ShimProtocol.h"

namespace retro {

struct LaunchRequest {
    std::filesystem::path exePath;     // absolute path of the program
    std::wstring arguments;            // already quoted, appended to the command line
    std::filesystem::path workingDir;  // usually the game's own folder
    std::filesystem::path shimPath;    // absolute path of RetroShim.dll
    bool injectShim = true;
    ShimConfig config{};
};

// Creates the process suspended, injects the shim with Detours, hands the
// shim its config payload, then resumes the main thread. On success the
// caller owns pi.hProcess / pi.hThread.
bool LaunchProcess(const LaunchRequest& request, PROCESS_INFORMATION& pi, std::string& error);

// Quotes one argument per the MSVC CommandLineToArgvW rules.
std::wstring QuoteArgument(const std::wstring& arg);

std::string Win32ErrorMessage(DWORD code);

}  // namespace retro
