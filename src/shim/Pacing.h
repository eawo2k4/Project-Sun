#pragma once

// The process-wide presentation pacer. Every presentation path (GDI blits,
// SwapBuffers, DirectDraw Flip, Direct3D 9 Present) goes through the same
// FrameScheduler, so a game mixing them still gets one cadence.

#include <cstdint>

namespace retro::shim::pacing {

void Configure(uint32_t fps);  // 0 = disabled
bool Enabled();

// Waits for the next frame slot.
void PaceFrame();

// DirectDraw games often call WaitForVerticalBlank and then Flip in the same
// frame. PaceVerticalBlank paces like PaceFrame; a PaceFlip directly after it
// (within half a period) counts as the same frame. Any other Flip is paced.
void PaceVerticalBlank();
void PaceFlip();

// Time since the last paced present, for presenters that coalesce updates.
bool DueForPresent();

}  // namespace retro::shim::pacing
