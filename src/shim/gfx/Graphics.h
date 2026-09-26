#pragma once

// Graphics interception layer: DirectDraw (ddraw.dll) and Direct3D 9
// (d3d9.dll). Both DLLs are hooked whenever they load (at startup or later
// through LoadLibrary).
//
//   DDraw.cpp  exclusive fullscreen virtualized: DDSCL_NORMAL + a system-memory
//              "primary" in the game's mode, presented (8-bit palettes
//              converted to 32-bit) into the managed window through the
//              shared pacer and integer scaling.
//   D3D9.cpp   fullscreen devices created windowed inside the managed window,
//              Present paced and letterboxed; optional Direct3DCreate9On12.

#include <windows.h>

#include <cstdint>

#include "retro/ShimProtocol.h"

namespace retro::shim::gfx {

// "ddraw" / "d3d9": are that DLL's entry points hooked right now?
bool IsApiHooked(const char* name);

// Called from the message pump: presents DirectDraw frames that were drawn
// straight onto the primary but not yet shown (rate-limited updates).
void PresentPendingFrames();

void GetPresentStats(PresentStats& out);

namespace ddraw {
void OnLoaded(HMODULE module);
void OnUnloaded(const void* base, size_t size);
LONG Unhook();  // inside the caller's Detours transaction
bool Hooked();
void PresentPending();
void GetStats(PresentStats& out);
}  // namespace ddraw

namespace d3d9 {
void OnLoaded(HMODULE module);
void OnUnloaded(const void* base, size_t size);
LONG Unhook();
bool Hooked();
}  // namespace d3d9

}  // namespace retro::shim::gfx
