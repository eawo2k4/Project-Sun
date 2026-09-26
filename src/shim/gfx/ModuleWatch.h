#pragma once

// Calls back when a named DLL is loaded or unloaded, including DLLs a game
// loads on demand with LoadLibrary long after the shim attached (common for
// ddraw.dll and d3d9.dll). Uses the loader's DLL notifications; callbacks
// run under the loader lock, so they must only do loader-safe work (patching
// code, GetProcAddress, logging).

#include <windows.h>

namespace retro::shim::gfx {

struct ModuleCallbacks {
    const wchar_t* baseName;                        // e.g. L"ddraw.dll"
    void (*loaded)(HMODULE module);
    void (*unloaded)(const void* base, size_t size);
};

// Starts watching; `loaded` is called immediately for modules already present.
bool StartModuleWatch(const ModuleCallbacks* callbacks, size_t count);
void StopModuleWatch();

}  // namespace retro::shim::gfx
