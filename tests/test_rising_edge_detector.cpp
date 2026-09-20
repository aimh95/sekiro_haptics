// Unit tests for RisingEdgeDetector -- turns raw per-poll controller button
// state into one event per physical press, used by
// apps/sekiro_signal_probe/main.cpp's RunControllerGuardWatchThread().

#include "sekiro_haptics/process/RisingEdgeDetector.hpp"
#include "testing.hpp"

using namespace sekiro_haptics::process;

SH_TEST(RisingEdgeDetector_InitiallyReleased_FirstPressRisesOnce) {
    RisingEdgeDetector detector;
    SH_CHECK(detector.Update(true));
}

SH_TEST(RisingEdgeDetector_HeldPressed_OnlyFirstPollRises) {
    RisingEdgeDetector detector;
    SH_CHECK(detector.Update(true));
    SH_CHECK(!detector.Update(true));
    SH_CHECK(!detector.Update(true));
    SH_CHECK(!detector.Update(true));
}

SH_TEST(RisingEdgeDetector_NeverPressed_NeverRises) {
    RisingEdgeDetector detector;
    SH_CHECK(!detector.Update(false));
    SH_CHECK(!detector.Update(false));
}

SH_TEST(RisingEdgeDetector_PressHoldReleaseRepress_RisesExactlyTwice) {
    // The exact scenario the ticket asks to verify: a full physical
    // press-hold-release-repress cycle must produce exactly 2 rising edges,
    // not 1 (missing the repress) and not more (re-firing while held).
    RisingEdgeDetector detector;
    int riseCount = 0;
    bool sequence[] = {true, true, true, false, true};
    for (bool pressed : sequence) {
        if (detector.Update(pressed)) {
            ++riseCount;
        }
    }
    SH_CHECK(riseCount == 2);
}

SH_TEST(RisingEdgeDetector_RepeatedReleaseAndPress_RisesOncePerPress) {
    RisingEdgeDetector detector;
    int riseCount = 0;
    bool sequence[] = {true, false, true, false, true, false, true};
    for (bool pressed : sequence) {
        if (detector.Update(pressed)) {
            ++riseCount;
        }
    }
    SH_CHECK(riseCount == 4);
}
