// Effect lifetime: expiry, cancellation, replacement, reset, device loss and
// reconnect. All against the fake transport, with an explicit clock -- no
// sleeps, no hardware.

#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"
#include "FakeDualSenseTransport.hpp"
#include "testing.hpp"

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

namespace {

constexpr std::int64_t kMs = 1000;   // microseconds per millisecond

std::uint8_t ModeByteFor(const std::vector<std::uint8_t>& report, TriggerSide side) {
    return report[side == TriggerSide::Left ? offsets::kLeftTriggerBlock
                                            : offsets::kRightTriggerBlock];
}

} // namespace

SH_TEST(TriggerRuntime_LeftAndRightAreControlledIndependently) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    const auto left = runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(3, 5),
                                    kHoldUntilReplaced, 0);
    const auto right = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeWeapon(2, 6, 7),
                                     kHoldUntilReplaced, 0);
    SH_CHECK(left.accepted && right.accepted);
    SH_CHECK(left.effectId != right.effectId);

    const auto report = transport.WrittenReports().back();
    SH_CHECK(ModeByteFor(report, TriggerSide::Left) == 0x21);
    SH_CHECK(ModeByteFor(report, TriggerSide::Right) == 0x25);

    // Releasing one side leaves the other holding its effect.
    runtime.CancelSide(TriggerSide::Left, 1 * kMs);
    const auto after = transport.WrittenReports().back();
    SH_CHECK(ModeByteFor(after, TriggerSide::Left) == 0x05);
    SH_CHECK(ModeByteFor(after, TriggerSide::Right) == 0x25);
    SH_CHECK(runtime.Active(TriggerSide::Right).active);
}

SH_TEST(TriggerRuntime_AnEffectWithADurationIsReleasedWhenItExpires) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 6), 100 * kMs, 0);
    SH_CHECK(runtime.Active(TriggerSide::Right).active);

    runtime.Tick(99 * kMs);
    SH_CHECK(runtime.Active(TriggerSide::Right).active);
    SH_CHECK(runtime.Stats().expired == 0);

    runtime.Tick(100 * kMs);
    SH_CHECK(!runtime.Active(TriggerSide::Right).active);
    SH_CHECK(runtime.Stats().expired == 1);
    SH_CHECK(ModeByteFor(transport.WrittenReports().back(), TriggerSide::Right) == 0x05);
}

SH_TEST(TriggerRuntime_HoldUntilReplacedNeverExpiresOnItsOwn) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(1, 4), kHoldUntilReplaced, 0);
    runtime.Tick(60'000 * kMs);   // a minute later
    SH_CHECK(runtime.Active(TriggerSide::Left).active);
    SH_CHECK(runtime.Stats().expired == 0);
}

SH_TEST(TriggerRuntime_AnOldEffectsExpiryDoesNotTurnOffTheOneThatReplacedIt) {
    // The reason effect ids exist. Without the id check, the 100 ms effect's
    // expiry at t=100 would release a trigger that a new effect took over at
    // t=50 and which should hold until t=250.
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    const auto first = runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(2, 4),
                                     100 * kMs, 0);
    const auto second = runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeWeapon(3, 7, 8),
                                      200 * kMs, 50 * kMs);
    SH_CHECK(first.effectId != second.effectId);
    SH_CHECK(runtime.Stats().replaced == 1);

    runtime.Tick(101 * kMs);
    SH_CHECK(runtime.Active(TriggerSide::Left).active);
    SH_CHECK(runtime.Active(TriggerSide::Left).effectId == second.effectId);
    SH_CHECK(runtime.Stats().expired == 0);
    SH_CHECK(ModeByteFor(transport.WrittenReports().back(), TriggerSide::Left) == 0x25);

    runtime.Tick(250 * kMs);
    SH_CHECK(!runtime.Active(TriggerSide::Left).active);
    SH_CHECK(runtime.Stats().expired == 1);
}

SH_TEST(TriggerRuntime_CancellingAStaleIdIsANoOp) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    const auto first = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 4),
                                     kHoldUntilReplaced, 0);
    const auto second = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(5, 8),
                                      kHoldUntilReplaced, 10 * kMs);

    SH_CHECK(!runtime.Cancel(TriggerSide::Right, first.effectId, 20 * kMs));
    SH_CHECK(runtime.Active(TriggerSide::Right).effectId == second.effectId);
    SH_CHECK(runtime.Stats().staleCancelsIgnored == 1);

    SH_CHECK(runtime.Cancel(TriggerSide::Right, second.effectId, 30 * kMs));
    SH_CHECK(!runtime.Active(TriggerSide::Right).active);
    SH_CHECK(runtime.Stats().cancelled == 1);
}

SH_TEST(TriggerRuntime_ChangingATriggerDoesNotDisturbSpeakerRoutingOrRumble) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    AudioOutputSettings audio;
    audio.speakerVolume = 0x64;
    audio.outputPath = 3;
    state.SetAudio(audio);
    state.SetLegacyRumble(80, 40);
    state.Submit(transport);

    runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeVibration(2, 6, 30), 50 * kMs, 0);
    runtime.Tick(60 * kMs);   // and expire it again

    for (const auto& report : transport.WrittenReports()) {
        SH_CHECK(report[offsets::kSpeakerVolume] == 0x64);
        SH_CHECK(report[offsets::kAudioControl] == (3 << 4));
        SH_CHECK(report[offsets::kMotorLeft] == 80);
        SH_CHECK(report[offsets::kMotorRight] == 40);
    }
}

SH_TEST(TriggerRuntime_ResetReleasesBothTriggersButKeepsTheAudioSection) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    state.SetAudio(AudioOutputSettings{});
    runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(2, 5), kHoldUntilReplaced, 0);
    runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(4, 5), kHoldUntilReplaced, 0);

    runtime.ResetToNeutral(10 * kMs);

    const auto report = transport.WrittenReports().back();
    SH_CHECK(ModeByteFor(report, TriggerSide::Left) == 0x05);
    SH_CHECK(ModeByteFor(report, TriggerSide::Right) == 0x05);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kSpeakerVolume) != 0);
    SH_CHECK(!runtime.Active(TriggerSide::Left).active);
    SH_CHECK(!runtime.Active(TriggerSide::Right).active);
    SH_CHECK(runtime.Stats().resets == 1);
}

SH_TEST(TriggerRuntime_ReconnectStartsNeutralAndDoesNotReplayTheOldEffect) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeWeapon(2, 6, 8), kHoldUntilReplaced, 0);
    SH_CHECK(runtime.Active(TriggerSide::Left).active);

    // Cable out: claims are dropped and nothing is written, because a write
    // to a gone device proves nothing about its physical state.
    const auto writesBeforeLoss = transport.WrittenReports().size();
    transport.SetOpen(false);
    runtime.OnDeviceLost();
    SH_CHECK(transport.WrittenReports().size() == writesBeforeLoss);
    SH_CHECK(!runtime.Active(TriggerSide::Left).active);

    // Cable back in.
    transport.SetOpen(true);
    runtime.OnReconnect(500 * kMs);

    const auto report = transport.WrittenReports().back();
    SH_CHECK(ModeByteFor(report, TriggerSide::Left) == 0x05);
    SH_CHECK(ModeByteFor(report, TriggerSide::Right) == 0x05);
    SH_CHECK(!runtime.Active(TriggerSide::Left).active);
    SH_CHECK(runtime.Stats().reconnects == 1);

    // And an expiry timer from before the disconnect cannot fire now.
    runtime.Tick(10'000 * kMs);
    SH_CHECK(runtime.Stats().expired == 0);
}

SH_TEST(TriggerRuntime_ARejectedSpecLeavesTheExistingEffectInPlace) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    const auto good = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(3, 6),
                                    kHoldUntilReplaced, 0);
    const auto writes = transport.WrittenReports().size();

    const auto bad = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(3, 99),
                                   kHoldUntilReplaced, 10 * kMs);
    SH_CHECK(!bad.accepted);
    SH_CHECK(!bad.error.empty());
    SH_CHECK(runtime.Stats().rejectedSpecs == 1);
    SH_CHECK(transport.WrittenReports().size() == writes);   // nothing sent
    SH_CHECK(runtime.Active(TriggerSide::Right).effectId == good.effectId);
}

SH_TEST(TriggerRuntime_AFailedWriteReleasesTheTriggerInsteadOfClaimingSuccess) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    transport.FailNextWrite();
    const auto result = runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(2, 5),
                                      100 * kMs, 0);
    SH_CHECK(!result.accepted);
    SH_CHECK(result.transport == TransportResult::WriteFailed);
    SH_CHECK(!runtime.Active(TriggerSide::Left).active);
    SH_CHECK(runtime.Stats().writeFailures == 1);

    // Nothing is left scheduled, so no later tick fabricates a release.
    runtime.Tick(1'000 * kMs);
    SH_CHECK(runtime.Stats().expired == 0);
}

SH_TEST(TriggerRuntime_ApplyingToAClosedDeviceFailsWithoutRecordingAnEffect) {
    FakeDualSenseTransport transport;   // never opened
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    const auto result = runtime.Apply(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 5),
                                      kHoldUntilReplaced, 0);
    SH_CHECK(!result.accepted);
    SH_CHECK(result.transport == TransportResult::NotOpen);
    SH_CHECK(!runtime.Active(TriggerSide::Right).active);
}

SH_TEST(TriggerRuntime_TickWritesNothingWhenNothingExpired) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime runtime(transport, state);

    runtime.Apply(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(2, 5), 500 * kMs, 0);
    const auto writes = transport.WrittenReports().size();
    for (std::int64_t t = 0; t < 400 * kMs; t += 5 * kMs) runtime.Tick(t);
    SH_CHECK(transport.WrittenReports().size() == writes);
}
