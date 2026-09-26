#pragma once

// Presentation-level frame pacing.
//
// FrameScheduler is the pure cadence logic: given "now", when may the next
// frame be presented? PreciseWaiter does the actual waiting: a
// high-resolution waitable timer for the bulk of the wait, then a short
// QueryPerformanceCounter spin for the last stretch, rather than Sleep(1)
// (which is only as good as the system timer resolution).

#include <cstdint>

namespace retro {

class FrameScheduler {
public:
    // fps == 0 disables pacing (NextPresentTime always returns `now`).
    FrameScheduler(uint32_t fps, int64_t ticksPerSecond);

    // Returns the time at which the frame being presented now should go out
    // (>= now), and advances the schedule. Keeps a steady cadence for fast
    // games, never "catches up" with a burst of frames after a hitch, and
    // stays out of the way of games slower than the cap.
    int64_t NextPresentTime(int64_t now);

    int64_t Period() const { return period_; }

private:
    int64_t period_ = 0;
    int64_t next_ = 0;
    bool started_ = false;
};

// True if a blit of blitW x blitH onto a target of targetW x targetH looks like
// presenting a whole frame (>= 75% of the target), as opposed to drawing a
// sprite, cursor or dirty rectangle.
bool IsFramePresent(int32_t blitW, int32_t blitH, int32_t targetW, int32_t targetH);

int64_t QpcNow();
int64_t QpcFrequency();

// Waits until a QPC deadline with sub-millisecond accuracy. One per thread:
// it owns a waitable timer handle.
class PreciseWaiter {
public:
    PreciseWaiter();
    ~PreciseWaiter();
    PreciseWaiter(const PreciseWaiter&) = delete;
    PreciseWaiter& operator=(const PreciseWaiter&) = delete;

    void WaitUntil(int64_t qpcDeadline);
    bool HighResolution() const { return highResolution_; }

private:
    void* timer_ = nullptr;
    bool highResolution_ = false;
    int64_t spinTicks_ = 0;
};

}  // namespace retro
