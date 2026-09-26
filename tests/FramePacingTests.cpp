// Unit tests for frame pacing (retro/FramePacing.h): the scheduler with a fake
// clock, the present heuristic, and one real-time measurement of the waiter.

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Check.h"
#include "retro/FramePacing.h"

using namespace retro;

namespace {

constexpr int64_t kFreq = 10'000'000;  // QPC ticks per second on modern Windows
constexpr int64_t kPeriod60 = kFreq / 60;

void TestDisabled() {
    FrameScheduler s(0, kFreq);
    CHECK(s.Period() == 0);
    CHECK(s.NextPresentTime(123) == 123);
    CHECK(s.NextPresentTime(124) == 124);
}

void TestFastGameIsCapped() {
    // A game presenting as fast as it can: frames go out exactly one period
    // apart, starting immediately.
    FrameScheduler s(60, kFreq);
    int64_t now = 1'000'000;
    int64_t previous = s.NextPresentTime(now);
    CHECK(previous == now);
    for (int i = 0; i < 600; ++i) {
        now = previous + 10;  // renders instantly after each present
        const int64_t at = s.NextPresentTime(now);
        CHECK(at == previous + kPeriod60);
        previous = at;
    }
}

void TestSlowGameIsNotDelayed() {
    // A game slower than the cap (40 fps) never waits.
    FrameScheduler s(60, kFreq);
    int64_t now = 0;
    for (int i = 0; i < 100; ++i) {
        CHECK(s.NextPresentTime(now) == now);
        now += kFreq / 40;
    }
}

void TestHitchDoesNotBurst() {
    FrameScheduler s(60, kFreq);
    int64_t t = s.NextPresentTime(0);
    t = s.NextPresentTime(t + 1);  // on cadence: slot 1

    // A 200 ms hitch (loading, alt-tab).
    const int64_t late = t + 2 * kFreq / 10;
    CHECK(s.NextPresentTime(late) == late);  // presented immediately...
    // ...and the next frame waits a full period instead of rushing out
    // several catch-up frames.
    CHECK(s.NextPresentTime(late + 1) == late + kPeriod60);
}

void TestSlightlyLateKeepsCadence() {
    // Up to one frame late: present now, but keep the original slots so the
    // average rate stays exactly at the cap.
    FrameScheduler s(60, kFreq);
    const int64_t t0 = s.NextPresentTime(0);
    const int64_t lateBy = kPeriod60 / 2;
    CHECK(s.NextPresentTime(t0 + kPeriod60 + lateBy) == t0 + kPeriod60 + lateBy);
    CHECK(s.NextPresentTime(t0 + kPeriod60 + lateBy + 1) == t0 + 2 * kPeriod60);
}

void TestPresentedRecently() {
    // WaitForVerticalBlank then Flip in the same frame: the second sync point
    // lands well within half a period of the first.
    FrameScheduler s(60, kFreq);
    CHECK(!s.PresentedRecently(0));  // nothing presented yet
    const int64_t t = s.NextPresentTime(1000);
    CHECK(s.PresentedRecently(t + kPeriod60 / 4));
    CHECK(!s.PresentedRecently(t + kPeriod60 / 2));
    const int64_t t2 = s.NextPresentTime(t + 5);  // waits for the next slot
    CHECK(t2 == t + kPeriod60);
    CHECK(s.PresentedRecently(t2 + 10));
    CHECK(!FrameScheduler(0, kFreq).PresentedRecently(0));
}

void TestFramePresentHeuristic() {
    CHECK(IsFramePresent(640, 480, 640, 480));    // full frame
    CHECK(IsFramePresent(640, 360, 640, 480));    // 75%: letterboxed video counts
    CHECK(!IsFramePresent(640, 359, 640, 480));
    CHECK(!IsFramePresent(32, 32, 640, 480));     // sprite
    CHECK(!IsFramePresent(640, 240, 640, 480));   // half-frame band
    CHECK(IsFramePresent(800, 600, 640, 480));    // oversized blit
    CHECK(!IsFramePresent(0, 480, 640, 480));
    CHECK(!IsFramePresent(640, 480, 0, 0));
}

void TestRealTimePacing() {
    // Drives the real waiter at 60 fps for 30 frames. Bounds are loose to
    // stay reliable on a busy machine; the point is "paced, and not by
    // Sleep(15.6 ms)-granularity waits".
    PreciseWaiter waiter;
    FrameScheduler s(60, QpcFrequency());
    const int frames = 30;
    std::vector<int64_t> stamps;
    for (int i = 0; i < frames; ++i) {
        const int64_t now = QpcNow();
        const int64_t at = s.NextPresentTime(now);
        if (at > now) waiter.WaitUntil(at);
        stamps.push_back(QpcNow());
    }

    const double freq = static_cast<double>(QpcFrequency());
    const double total = (stamps.back() - stamps.front()) / freq;
    const double expected = (frames - 1) / 60.0;
    double worstEarly = 0;
    for (size_t i = 1; i < stamps.size(); ++i) {
        const double dt = (stamps[i] - stamps[i - 1]) / freq;
        worstEarly = std::max(worstEarly, (1.0 / 60.0) - dt);
    }
    std::printf("  %d frames in %.1f ms (ideal %.1f ms), %s timer, worst early %.3f ms\n", frames,
                total * 1000, expected * 1000, waiter.HighResolution() ? "high-res" : "legacy",
                worstEarly * 1000);
    CHECK(total >= expected * 0.99);  // never faster than the cap
    CHECK(total <= expected * 1.25);  // and not wildly slower
    CHECK(worstEarly < 0.0005);       // no frame more than 0.5 ms early
}

}  // namespace

int main() {
    const test::Case cases[] = {
        {"Disabled", TestDisabled},
        {"FastGameIsCapped", TestFastGameIsCapped},
        {"SlowGameIsNotDelayed", TestSlowGameIsNotDelayed},
        {"HitchDoesNotBurst", TestHitchDoesNotBurst},
        {"SlightlyLateKeepsCadence", TestSlightlyLateKeepsCadence},
        {"PresentedRecently", TestPresentedRecently},
        {"FramePresentHeuristic", TestFramePresentHeuristic},
        {"RealTimePacing", TestRealTimePacing},
    };
    return test::RunAll(cases);
}
