#pragma once

// Graphics interception layer: DirectDraw (ddraw.dll), Direct3D 8 (d3d8.dll)
// and Direct3D 9 (d3d9.dll). Each DLL is hooked whenever it loads (at startup
// or later through LoadLibrary).
//
//   DDraw.cpp  exclusive fullscreen virtualized: DDSCL_NORMAL + a system-memory
//              "primary" in the game's mode, presented (8-bit palettes
//              converted to 32-bit) into the managed window through the
//              shared pacer and integer scaling.
//   D3D9.cpp   fullscreen devices created windowed inside the managed window,
//              Present paced and letterboxed; optional Direct3DCreate9On12.
//   D3D8.cpp   the same containment for native D3D8, or (--d3d8to9) D3D8
//              bridged onto D3D9 by the vendored d3d8to9.

#include <windows.h>

#include <cstdint>

#include "retro/ShimProtocol.h"

namespace retro::shim::gfx {

// "ddraw" / "d3d8" / "d3d9": are that DLL's entry points hooked right now?
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
void AddStats(PresentStats& out);
}  // namespace d3d9

namespace d3d8 {
void OnLoaded(HMODULE module);
void OnUnloaded(const void* base, size_t size);
LONG Unhook();
bool Hooked();
void AddStats(PresentStats& out);
}  // namespace d3d8

}  // namespace retro::shim::gfx
