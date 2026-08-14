// Tests for HapticScheduler's duration-based auto-stop and "latest effect
// wins" policy (OUT-LEGACY-002 follow-up). No fixed sleep is used as a
// synchronization mechanism anywhere here -- every wait is a
// condition-variable wait with a bounded timeout (DurationTestBackend's
// WaitFor*, mirroring MockHapticBackend's existing WaitForEffectCount),
// including "assert this did NOT happen" checks (a wait that times out
// returning false), matching the pattern already established by
// HapticScheduler_Cancel_PreventsDispatch in test_scheduler.cpp. Effect
// durations are short (tens of ms) real time, since HapticScheduler's
// production timing is std::chrono::steady_clock-based, not an injected
// clock -- see HapticScheduler.hpp.

#include "sekiro_haptics/HapticScheduler.hpp"
#include "testing.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace sekiro_haptics;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// Test-only IHapticBackend double: records every SendEffect/Reset call
// with a timestamp, can be told to fail the next call of either kind, and
// exposes condition-variable waits for both -- everything the duration/
// stop test cases below need that MockHapticBackend doesn't (per-call
// timestamps, failure injection).
class DurationTestBackend final : public IHapticBackend {
public:
    HapticBackendResult SendEffect(const HapticEffect& effect) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++sendAttempts_;
        bool fail = failNextSend_;
        failNextSend_ = false;
        if (!fail) {
            sendLog_.emplace_back(Clock::now(), effect.debugLabel);
        }
        cv_.notify_all();
        return fail ? HapticBackendResult::DeviceError : HapticBackendResult::Success;
    }

    HapticBackendResult Reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++resetCount_;
        resetTimestamps_.push_back(Clock::now());
        bool fail = failNextReset_;
        failNextReset_ = false;
        cv_.notify_all();
        return fail ? HapticBackendResult::DeviceError : HapticBackendResult::Success;
    }

    bool IsConnected() const override { return true; }

    void FailNextSend() {
        std::lock_guard<std::mutex> lock(mutex_);
        failNextSend_ = true;
    }

    void FailNextReset() {
        std::lock_guard<std::mutex> lock(mutex_);
        failNextReset_ = true;
    }

    std::size_t SendCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sendLog_.size();
    }

    int SendAttempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sendAttempts_;
    }

    int ResetCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return resetCount_;
    }

    std::vector<std::pair<Clock::time_point, std::string>> SendLog() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sendLog_;
    }

    std::vector<Clock::time_point> ResetTimestamps() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return resetTimestamps_;
    }

    bool WaitForSendCount(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return sendLog_.size() >= count; });
    }

    bool WaitForSendAttempts(int count, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return sendAttempts_ >= count; });
    }

    bool WaitForResetCount(int count, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return resetCount_ >= count; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    bool failNextSend_ = false;
    bool failNextReset_ = false;
    int sendAttempts_ = 0;
    int resetCount_ = 0;
    std::vector<std::pair<Clock::time_point, std::string>> sendLog_;
    std::vector<Clock::time_point> resetTimestamps_;
};

} // namespace

// --- 1-3: basic duration -> stop timing ---

SH_TEST(HapticSchedulerDuration_BeforeExpiry_OnlyStartReportExists) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 300ms;
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 100ms) == false); // well before 300ms
    SH_CHECK(backend.SendAttempts() == 1);
}

SH_TEST(HapticSchedulerDuration_AtExpiry_SendsExactlyOneStopReport) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 60ms;
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 1s));
    SH_CHECK(backend.ResetCount() == 1);

    SH_CHECK(backend.WaitForResetCount(2, 200ms) == false); // no second/duplicate stop afterward
    SH_CHECK(backend.ResetCount() == 1);
}

SH_TEST(HapticSchedulerDuration_StopTimingTracksEffectDuration) {
    // Proves the stop's timing is bound to *this effect's* duration
    // specifically -- not immediate, and not some unrelated longer window
    // (e.g. an entire replay's length, per the ticket's framing). See also
    // test_replay_hardware_backend.cpp's positive fixture test, where the
    // overall replay+wait window is much longer than the dispatched
    // preset's 28ms duration, yet the stop still lands promptly at 28ms.
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 150ms;
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 1s));

    auto sendLog = backend.SendLog();
    auto resetTimestamps = backend.ResetTimestamps();
    SH_CHECK(sendLog.size() == 1);
    SH_CHECK(resetTimestamps.size() == 1);

    auto elapsed = resetTimestamps[0] - sendLog[0].first;
    SH_CHECK(elapsed >= 120ms); // not near-immediate
    SH_CHECK(elapsed <= 500ms); // not drastically later than the 150ms duration (generous scheduling slack)
}

// --- 4: two sequential (non-overlapping) effects, each on its own schedule ---

SH_TEST(HapticSchedulerDuration_TwoSequentialEffects_EachStopsAtItsOwnDuration) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect first;
    first.duration = 60ms;
    first.debugLabel = "first";
    scheduler.Schedule(first);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 1s)); // first fully stopped before second starts

    HapticEffect second;
    second.duration = 120ms;
    second.debugLabel = "second";
    scheduler.Schedule(second, 50ms);

    SH_CHECK(backend.WaitForSendCount(2, 1s));
    SH_CHECK(backend.WaitForResetCount(2, 1s));
    SH_CHECK(backend.ResetCount() == 2);
    SH_CHECK(backend.SendAttempts() == 2);
}

// --- 5-7: latest-effect-wins ---

SH_TEST(HapticSchedulerDuration_NewEffectBeforeOldExpiry_StartsImmediately) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect first;
    first.duration = 500ms; // won't expire during this test
    first.debugLabel = "first";
    scheduler.Schedule(first);
    SH_CHECK(backend.WaitForSendCount(1, 1s));

    HapticEffect second;
    second.duration = 60ms;
    second.debugLabel = "second";
    scheduler.Schedule(second, 30ms); // dispatches well before first's 500ms expiry

    SH_CHECK(backend.WaitForSendCount(2, 1s)); // second dispatched promptly, not delayed behind first
    auto log = backend.SendLog();
    SH_CHECK(log[1].second == "second");
}

SH_TEST(HapticSchedulerDuration_NewEffectSupersedesOld_OldExpiryNeverFires_NewStopsAtOwnExpiry) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect first;
    first.duration = 80ms;
    first.debugLabel = "first";
    scheduler.Schedule(first);
    SH_CHECK(backend.WaitForSendCount(1, 1s));

    HapticEffect second;
    second.duration = 200ms;
    second.debugLabel = "second";
    scheduler.Schedule(second, 20ms); // dispatches ~20ms in, well before first's 80ms expiry

    SH_CHECK(backend.WaitForSendCount(2, 1s));

    // At first's original expiry (~80ms after ITS dispatch), second is
    // still well inside its own 200ms window -- must not have been cut
    // off by first's now-stale expiry.
    SH_CHECK(backend.WaitForResetCount(1, 150ms) == false);
    SH_CHECK(backend.ResetCount() == 0);

    // second's own duration eventually elapses and stops it -- exactly
    // one stop total.
    SH_CHECK(backend.WaitForResetCount(1, 1s));
    SH_CHECK(backend.ResetCount() == 1);
    SH_CHECK(backend.WaitForResetCount(2, 200ms) == false);
    SH_CHECK(backend.ResetCount() == 1);
}

// --- 8: rapid successive inputs, no stale/duplicate stops ---

SH_TEST(HapticSchedulerDuration_RapidSuccessiveEffects_NoStaleOrDuplicateStops) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    // Each new effect dispatches (at 0/15/30ms) well before the previous
    // one's 40ms expiry, so effect0's and effect1's expiries are both
    // superseded before they can fire. Only effect2 (the last one,
    // dispatched ~30ms in) should ever produce a stop, at ~30+40=70ms.
    for (int i = 0; i < 3; ++i) {
        HapticEffect effect;
        effect.duration = 40ms;
        effect.debugLabel = "effect" + std::to_string(i);
        scheduler.Schedule(effect, std::chrono::milliseconds(i * 15));
    }

    SH_CHECK(backend.WaitForSendCount(3, 1s));

    SH_CHECK(backend.WaitForResetCount(1, 1s));
    SH_CHECK(backend.ResetCount() == 1); // exactly one -- no stale stops from effect0/effect1

    SH_CHECK(backend.WaitForResetCount(2, 300ms) == false); // and no duplicate/extra stop afterward
    SH_CHECK(backend.ResetCount() == 1);
}

// --- 9: explicit Reset() cancels pending expiry ---

SH_TEST(HapticSchedulerDuration_ExplicitReset_CancelsPendingExpiry) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 300ms;
    scheduler.Schedule(effect);
    SH_CHECK(backend.WaitForSendCount(1, 1s));

    scheduler.Reset(); // well before the 300ms duration would elapse
    SH_CHECK(backend.WaitForResetCount(1, 1s));
    SH_CHECK(backend.ResetCount() == 1);

    // The original 300ms auto-stop must not ALSO fire later -- it was
    // cancelled, not merely raced.
    SH_CHECK(backend.WaitForResetCount(2, 400ms) == false);
    SH_CHECK(backend.ResetCount() == 1);
}

// --- 10: scheduler shutdown with an active effect ---

SH_TEST(HapticSchedulerDuration_Shutdown_ActiveEffect_PerformsBestEffortStop) {
    DurationTestBackend backend;
    {
        HapticScheduler scheduler(backend);
        HapticEffect effect;
        effect.duration = 5s; // still "active" well past this test's lifetime
        scheduler.Schedule(effect);
        SH_CHECK(backend.WaitForSendCount(1, 1s));
        // scheduler destructs here, mid-effect
    }
    SH_CHECK(backend.ResetCount() == 1);
}

SH_TEST(HapticSchedulerDuration_Shutdown_NoActiveEffect_NoStopSent) {
    DurationTestBackend backend;
    { HapticScheduler scheduler(backend); }
    SH_CHECK(backend.ResetCount() == 0);
}

// --- 11: failed start does not become "active" ---

SH_TEST(HapticSchedulerDuration_FailedSendEffect_DoesNotScheduleAutoStop) {
    DurationTestBackend backend;
    backend.FailNextSend();
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 50ms;
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendAttempts(1, 1s));
    SH_CHECK(backend.SendCount() == 0); // the attempt failed -- nothing "landed"

    // If a stop had incorrectly been scheduled for the failed send, it
    // would land around now (its 50ms duration elapsed); confirm it never does.
    SH_CHECK(backend.WaitForResetCount(1, 200ms) == false);
    SH_CHECK(backend.ResetCount() == 0);
}

// --- 12: auto-stop write failure is observable, no crash, no retry ---

SH_TEST(HapticSchedulerDuration_AutoStopWriteFailure_ObservableNoCrashNoRetry) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 40ms;
    scheduler.Schedule(effect);
    SH_CHECK(backend.WaitForSendCount(1, 1s));

    backend.FailNextReset(); // the upcoming auto-stop attempt will fail

    SH_CHECK(backend.WaitForResetCount(1, 1s)); // the attempt happened (observable via ResetCount)
    SH_CHECK(backend.ResetCount() == 1);

    SH_CHECK(backend.WaitForResetCount(2, 300ms) == false); // no retry
    SH_CHECK(backend.ResetCount() == 1);

    // No crash: the process is still running here. The scheduler is also
    // still usable -- an explicit Reset() afterward works normally.
    scheduler.Reset();
    SH_CHECK(backend.WaitForResetCount(2, 1s));
}

// --- 13: zero/negative duration ---

SH_TEST(HapticSchedulerDuration_ZeroDuration_StopsPromptly) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = 0ms;
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 1s));
}

SH_TEST(HapticSchedulerDuration_NegativeDuration_ClampedToImmediateStopNoCrash) {
    DurationTestBackend backend;
    HapticScheduler scheduler(backend);

    HapticEffect effect;
    effect.duration = -50ms; // std::chrono::milliseconds can represent negative values
    scheduler.Schedule(effect);

    SH_CHECK(backend.WaitForSendCount(1, 1s));
    SH_CHECK(backend.WaitForResetCount(1, 1s)); // clamped to "stop immediately," not UB/hang
}
