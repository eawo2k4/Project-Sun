#pragma once

// Process-wide shim state, owned by dllmain.cpp and read by the hook modules.

#include <windows.h>

#include "retro/ShimProtocol.h"

namespace retro::shim {

// Config received from the launcher (or the parent process), fixed after
// DLL_PROCESS_ATTACH.
const ShimConfig& Config();

// RetroShim.dll's own module handle.
HMODULE Module();

}  // namespace retro::shim
