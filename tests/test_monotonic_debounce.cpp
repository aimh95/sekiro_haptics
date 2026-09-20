// Unit tests for MonotonicDebounce -- the debounce logic shared by
// apps/sekiro_signal_probe/main.cpp's hotkey and controller-guard input
// threads (SEK-PROBE-001E Section 7). Pure logic, no OS dependency.

#include "sekiro_haptics/process/MonotonicDebounce.hpp"
#include "testing.hpp"

using namespace sekiro_haptics::process;

SH_TEST(MonotonicDebounce_FirstCall_AlwaysFires) {
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
}

SH_TEST(MonotonicDebounce_SecondCallBelowThreshold_IsSuppressed) {
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
    SH_CHECK(!debounce.ShouldFire(50'000)); // 50ms later -- below the 120ms debounce
    SH_CHECK(!debounce.ShouldFire(119'999)); // just under the threshold
}

SH_TEST(MonotonicDebounce_CallAtExactlyThreshold_Fires) {
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
    SH_CHECK(debounce.ShouldFire(120'000)); // exactly the debounce interval later
}

SH_TEST(MonotonicDebounce_CallAboveThreshold_Fires) {
    // A genuine rapid re-press separated by more than the debounce interval
    // (e.g. two real parries 150ms apart during a fast combo) must never be
    // swallowed -- this is the exact "does debounce eat real input" concern.
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
    SH_CHECK(debounce.ShouldFire(150'000));
}

SH_TEST(MonotonicDebounce_SuppressedCallDoesNotResetTheWindow) {
    // A suppressed call must not itself become the new "last fired" time --
    // otherwise a steady stream of sub-threshold triggers would keep pushing
    // the window out indefinitely and could swallow a real press that
    // arrives well after the *original* trigger.
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
    SH_CHECK(!debounce.ShouldFire(50'000));
    SH_CHECK(!debounce.ShouldFire(100'000));
    SH_CHECK(debounce.ShouldFire(120'000)); // still measured from t=0, not t=100'000
}

SH_TEST(MonotonicDebounce_AfterFiring_NextWindowMeasuresFromNewTrigger) {
    MonotonicDebounce debounce(120'000);
    SH_CHECK(debounce.ShouldFire(0));
    SH_CHECK(debounce.ShouldFire(200'000));
    SH_CHECK(!debounce.ShouldFire(250'000)); // only 50ms after the t=200'000 trigger
    SH_CHECK(debounce.ShouldFire(320'000)); // 120ms after t=200'000
}

SH_TEST(MonotonicDebounce_IndependentInstances_GuardInputImmediatelyFollowedByOutcomeHotkey_NeitherIsSuppressed) {
    // main.cpp gives the controller guard-watch thread and each hotkey id
    // their own separate MonotonicDebounce instance (see
    // RunControllerGuardWatchThread()/RunMarkerHotkeyThread()) -- a
    // guard_input firing must never consume the debounce window of a
    // completely different input (e.g. the human's F5 press landing 10ms
    // after the auto-detected guard_input). This test proves that
    // separation at the data-structure level: two independent instances,
    // even with an interval well under the shared 120ms debounce constant,
    // never suppress each other.
    MonotonicDebounce guardInputDebounce(120'000);
    MonotonicDebounce f5Debounce(120'000);

    SH_CHECK(guardInputDebounce.ShouldFire(0)); // controller LB press at t=0
    SH_CHECK(f5Debounce.ShouldFire(10'000)); // human's F5 press 10ms later -- still fires
}
