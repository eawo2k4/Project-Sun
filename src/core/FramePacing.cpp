#include "retro/FramePacing.h"

#include <windows.h>

#include <algorithm>

namespace retro {

FrameScheduler::FrameScheduler(uint32_t fps, int64_t ticksPerSecond)
    : period_(fps > 0 ? ticksPerSecond / fps : 0) {}

int64_t FrameScheduler::NextPresentTime(int64_t now) {
    if (period_ <= 0) return now;

    if (!started_ || now >= next_ + period_) {
        // First frame, or more than a whole frame late (loading screen, alt-tab,
        // slow scene): present immediately and restart the cadence from here.
        started_ = true;
        next_ = now + period_;
        return now;
    }
    // On time or early: wait for the slot. Up to one frame late: present now but
    // keep the original cadence, so the average rate stays exact.
    const int64_t presentAt = std::max(now, next_);
    next_ += period_;
    return presentAt;
}

bool IsFramePresent(int32_t blitW, int32_t blitH, int32_t targetW, int32_t targetH) {
    if (blitW <= 0 || blitH <= 0 || targetW <= 0 || targetH <= 0) return false;
    const int64_t blit = static_cast<int64_t>(blitW) * blitH;
    const int64_t target = static_cast<int64_t>(targetW) * targetH;
    return blit * 4 >= target * 3;
}

int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t QpcFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

PreciseWaiter::PreciseWaiter() {
    // High-resolution timers (Windows 10 1803+) are accurate to well under a
    // millisecond without raising the global timer resolution.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    highResolution_ = timer_ != nullptr;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    // Spin for the final stretch: ~0.5 ms with a high-resolution timer, a
    // couple of scheduler quanta with a legacy one.
    spinTicks_ = QpcFrequency() / (highResolution_ ? 2000 : 250);
}

PreciseWaiter::~PreciseWaiter() {
    if (timer_) CloseHandle(timer_);
}

void PreciseWaiter::WaitUntil(int64_t deadline) {
    const int64_t freq = QpcFrequency();
    for (;;) {
        const int64_t remaining = deadline - QpcNow();
        if (remaining <= 0) return;

        if (remaining > spinTicks_ && timer_) {
            // Relative due time in 100 ns units (negative = relative).
            const int64_t sleepTicks = remaining - spinTicks_;
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, sleepTicks * 10'000'000 / freq);
            if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(timer_, INFINITE);
                continue;
            }
        }
        while (QpcNow() < deadline) YieldProcessor();
        return;
    }
}

}  // namespace retro
