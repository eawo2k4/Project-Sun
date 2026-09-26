#include "Pacing.h"

#include <windows.h>

#include <memory>

#include "retro/FramePacing.h"

namespace retro::shim::pacing {
namespace {

std::unique_ptr<FrameScheduler> g_scheduler;
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_enabled = false;
int64_t g_lastRelease = 0;     // QPC time of the last frame released (paced or not)
bool g_vblankPending = false;  // the last release was a WaitForVerticalBlank

enum class Source { Frame, VerticalBlank, Flip };

void WaitForSlot(Source source) {
    thread_local PreciseWaiter waiter;
    const int64_t now = QpcNow();
    int64_t presentAt = now;
    AcquireSRWLockExclusive(&g_lock);
    // Only a Flip right after a vertical-blank wait joins that frame; a Flip
    // after a Flip is the next frame and waits for its own slot.
    const bool coalesce =
        source == Source::Flip && g_vblankPending && g_scheduler->PresentedRecently(now);
    if (!coalesce) presentAt = g_scheduler->NextPresentTime(now);
    g_vblankPending = source == Source::VerticalBlank;
    g_lastRelease = presentAt;
    ReleaseSRWLockExclusive(&g_lock);
    if (presentAt > now) waiter.WaitUntil(presentAt);
}

}  // namespace

void Configure(uint32_t fps) {
    AcquireSRWLockExclusive(&g_lock);
    g_scheduler = std::make_unique<FrameScheduler>(fps, QpcFrequency());
    g_enabled = fps > 0;
    ReleaseSRWLockExclusive(&g_lock);
}

bool Enabled() { return g_enabled; }

void PaceFrame() {
    if (g_enabled) WaitForSlot(Source::Frame);
}

void PaceVerticalBlank() {
    if (g_enabled) WaitForSlot(Source::VerticalBlank);
}

void PaceFlip() {
    if (g_enabled) WaitForSlot(Source::Flip);
}

bool DueForPresent() {
    AcquireSRWLockShared(&g_lock);
    const int64_t period = g_scheduler ? g_scheduler->Period() : 0;
    const bool due = period <= 0 || QpcNow() - g_lastRelease >= period;
    ReleaseSRWLockShared(&g_lock);
    return due;
}

}  // namespace retro::shim::pacing
