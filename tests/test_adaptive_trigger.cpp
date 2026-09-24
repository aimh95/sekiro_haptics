// Adaptive-trigger encoding and output-state composition. Pure byte tests --
// no HID device is touched; the transport is the existing fake.

#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"
#include "sekiro_haptics/dualsense/DualSenseOutputState.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "FakeDualSenseTransport.hpp"
#include "testing.hpp"

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

// --- encoding ------------------------------------------------------------

SH_TEST(Trigger_OffUsesTheOfficialModeByteAndNoParameters) {
    TriggerBlock block{};
    const auto result = EncodeTriggerEffect(TriggerEffectSpec::MakeOff(), block);

    SH_CHECK(result.ok);
    SH_CHECK(block[0] == 0x05);
    for (std::size_t i = 1; i < block.size(); ++i) SH_CHECK(block[i] == 0);
}

SH_TEST(Trigger_FeedbackPacksOneActiveBitAndOneLevelPerZoneFromPositionUp) {
    // position 4, strength 3 -> zones 4..9 active, each carrying (3-1)=2.
    TriggerBlock block{};
    const auto result = EncodeTriggerEffect(TriggerEffectSpec::MakeFeedback(4, 3), block);
    SH_CHECK(result.ok);
    SH_CHECK(!result.reducedToOff);
    SH_CHECK(block[0] == 0x21);

    const auto activeZones = static_cast<std::uint16_t>(block[1] | (block[2] << 8));
    SH_CHECK(activeZones == 0b1111110000);   // bits 4..9

    const auto levelZones = static_cast<std::uint32_t>(block[3] | (block[4] << 8) |
                                                        (block[5] << 16) | (block[6] << 24));
    for (unsigned zone = 0; zone < 10; ++zone) {
        const auto level = (levelZones >> (3 * zone)) & 0x07u;
        SH_CHECK(level == (zone >= 4 ? 2u : 0u));
    }
    SH_CHECK(block[7] == 0 && block[8] == 0 && block[9] == 0 && block[10] == 0);
}

SH_TEST(Trigger_WeaponMarksExactlyItsStartAndEndZones) {
    TriggerBlock block{};
    const auto result = EncodeTriggerEffect(TriggerEffectSpec::MakeWeapon(3, 6, 8), block);
    SH_CHECK(result.ok);
    SH_CHECK(block[0] == 0x25);

    const auto zones = static_cast<std::uint16_t>(block[1] | (block[2] << 8));
    SH_CHECK(zones == ((1u << 3) | (1u << 6)));
    SH_CHECK(block[3] == 7);   // strength - 1
    for (std::size_t i = 4; i < block.size(); ++i) SH_CHECK(block[i] == 0);
}

SH_TEST(Trigger_VibrationCarriesItsFrequencyAndFeedbackDoesNotHaveOne) {
    TriggerBlock vibration{};
    SH_CHECK(EncodeTriggerEffect(TriggerEffectSpec::MakeVibration(2, 5, 40), vibration).ok);
    SH_CHECK(vibration[0] == 0x26);
    SH_CHECK(vibration[9] == 40);

    // Feedback has no frequency parameter at all, so byte 9 must stay zero --
    // this is the "do not apply an unsupported parameter to every mode" rule
    // expressed as a byte check.
    TriggerBlock feedback{};
    SH_CHECK(EncodeTriggerEffect(TriggerEffectSpec::MakeFeedback(2, 5), feedback).ok);
    SH_CHECK(feedback[9] == 0);
}

SH_TEST(Trigger_ZeroStrengthIsEncodedAsOffRatherThanAZeroStrengthEffect) {
    TriggerBlock block{};
    const auto result = EncodeTriggerEffect(TriggerEffectSpec::MakeFeedback(3, 0), block);
    SH_CHECK(result.ok);
    SH_CHECK(result.reducedToOff);
    SH_CHECK(block[0] == 0x05);

    TriggerBlock vib{};
    const auto noFrequency = EncodeTriggerEffect(TriggerEffectSpec::MakeVibration(3, 5, 0), vib);
    SH_CHECK(noFrequency.ok);
    SH_CHECK(noFrequency.reducedToOff);
    SH_CHECK(vib[0] == 0x05);
}

SH_TEST(Trigger_OutOfRangeParametersAreRejectedNotClamped) {
    TriggerBlock block{};
    block.fill(0xAB);

    const auto badPosition = EncodeTriggerEffect(TriggerEffectSpec::MakeFeedback(10, 3), block);
    SH_CHECK(!badPosition.ok);
    SH_CHECK(!badPosition.error.empty());

    const auto badStrength = EncodeTriggerEffect(TriggerEffectSpec::MakeFeedback(3, 9), block);
    SH_CHECK(!badStrength.ok);

    // Weapon's start has a floor of 2 and its end must be above the start.
    SH_CHECK(!EncodeTriggerEffect(TriggerEffectSpec::MakeWeapon(1, 5, 4), block).ok);
    SH_CHECK(!EncodeTriggerEffect(TriggerEffectSpec::MakeWeapon(5, 5, 4), block).ok);
    SH_CHECK(!EncodeTriggerEffect(TriggerEffectSpec::MakeWeapon(3, 9, 4), block).ok);
    SH_CHECK(!EncodeTriggerEffect(TriggerEffectSpec::MakeVibration(3, 9, 30), block).ok);

    // The destination was never written by any of the rejections.
    for (const auto byte : block) SH_CHECK(byte == 0xAB);
}

SH_TEST(Trigger_SimpleFeedbackKeepsTheLegacyRawByteMeaning) {
    // The mode this repository already shipped: raw 0..255 bytes, not zones.
    TriggerBlock block{};
    SH_CHECK(EncodeTriggerEffect(TriggerEffectSpec::MakeSimpleFeedback(200, 180), block).ok);
    SH_CHECK(block[0] == 0x01);
    SH_CHECK(block[1] == 200);
    SH_CHECK(block[2] == 180);
}

// --- output state --------------------------------------------------------

SH_TEST(OutputState_TriggerBlocksLandAtTheDocumentedOffsets) {
    DualSenseOutputState state;
    state.SetTrigger(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(3, 4));
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeWeapon(2, 6, 5));

    const auto report = state.BuildReport();
    SH_CHECK(report[0] == 0x02);
    SH_CHECK(report[offsets::kRightTriggerBlock] == 0x21);
    SH_CHECK(report[offsets::kLeftTriggerBlock] == 0x25);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kRightTriggerEffect) != 0);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kLeftTriggerEffect) != 0);
}

SH_TEST(OutputState_SettingOneTriggerLeavesTheOtherSideAlone) {
    DualSenseOutputState state;
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(5, 6));
    const auto leftBefore = state.Trigger(TriggerSide::Left);

    state.SetTrigger(TriggerSide::Right, TriggerEffectSpec::MakeVibration(1, 4, 25));

    SH_CHECK(state.Trigger(TriggerSide::Left) == leftBefore);
    const auto report = state.BuildReport();
    SH_CHECK(report[offsets::kLeftTriggerBlock] == 0x21);
    SH_CHECK(report[offsets::kRightTriggerBlock] == 0x26);
}

SH_TEST(OutputState_ChangingATriggerPreservesSpeakerRoutingAndRumble) {
    // This is the regression the whole type exists for: the old builders each
    // produced a fresh zeroed report, so a trigger write dropped the audio
    // section's apply flags and values.
    DualSenseOutputState state;
    AudioOutputSettings audio;
    audio.speakerVolume = 0x64;
    audio.outputPath = 3;
    audio.speakerPreGain = 7;
    state.SetAudio(audio);
    state.SetLegacyRumble(120, 30);

    state.SetTrigger(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(4, 6));

    const auto report = state.BuildReport();
    SH_CHECK(report[offsets::kSpeakerVolume] == 0x64);
    SH_CHECK(report[offsets::kAudioControl] == (3 << 4));
    SH_CHECK(report[offsets::kAudioControl2] == 7);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kSpeakerVolume) != 0);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kAudioControl) != 0);
    SH_CHECK((report[offsets::kValidFlag1] & valid_flag1::kAudioControl2) != 0);
    SH_CHECK(report[offsets::kMotorLeft] == 120);
    SH_CHECK(report[offsets::kMotorRight] == 30);
    SH_CHECK(report[offsets::kRightTriggerBlock] == 0x21);
}

SH_TEST(OutputState_UnclaimedSectionsNeverSetTheirApplyFlags) {
    DualSenseOutputState state;
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(4, 6));

    const auto report = state.BuildReport();
    // No audio claim, no motor claim -> those flags stay clear, so the device
    // keeps whatever it had rather than being reset to this process's idea of
    // neutral.
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kSpeakerVolume) == 0);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kHapticsSelect) == 0);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kCompatibleVibration) == 0);
    SH_CHECK(report[offsets::kValidFlag1] == 0);
    SH_CHECK(report[offsets::kValidFlag2] == 0);
}

SH_TEST(OutputState_PcmHapticsDoNotAssertHapticsSelect) {
    // valid_flag0 bit 1 switches the actuators to classic rumble, which would
    // take them off the PCM path the guard cues use.
    DualSenseOutputState state;
    state.MarkPcmHapticsActive(true);
    state.SetTrigger(TriggerSide::Right, TriggerEffectSpec::MakeFeedback(2, 5));

    const auto report = state.BuildReport();
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kHapticsSelect) == 0);
    SH_CHECK(state.HapticPathInUse() == HapticPath::Pcm);

    state.SetLegacyRumble(10, 10);
    SH_CHECK(state.HapticPathInUse() == HapticPath::LegacyRumble);
    SH_CHECK((state.BuildReport()[offsets::kValidFlag0] & valid_flag0::kHapticsSelect) != 0);
}

SH_TEST(OutputState_SubmitSkipsWritesWhenNothingChanged) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;

    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(3, 5));
    SH_CHECK(state.Submit(transport) == TransportResult::Success);
    SH_CHECK(transport.WrittenReports().size() == 1);

    // Same effect again: no byte differs, so no write.
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(3, 5));
    SH_CHECK(state.Submit(transport) == TransportResult::Success);
    SH_CHECK(transport.WrittenReports().size() == 1);
    SH_CHECK(state.Stats().skippedUnchanged == 1);

    // A different one does write.
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(3, 6));
    SH_CHECK(state.Submit(transport) == TransportResult::Success);
    SH_CHECK(transport.WrittenReports().size() == 2);
}

SH_TEST(OutputState_FailedWriteKeepsTheStateDirtyForARetry) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    state.SetTrigger(TriggerSide::Right, TriggerEffectSpec::MakeWeapon(2, 5, 4));

    transport.FailNextWrite();
    SH_CHECK(state.Submit(transport) == TransportResult::WriteFailed);
    SH_CHECK(state.Dirty());
    SH_CHECK(state.Stats().writeFailures == 1);

    SH_CHECK(state.Submit(transport) == TransportResult::Success);
    SH_CHECK(!state.Dirty());
}

SH_TEST(OutputState_ReleasingAudioTransmitsZeroedValuesWithTheApplyFlagsOnce) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    state.SetAudio(AudioOutputSettings{});
    state.Submit(transport);

    state.ReleaseAudio();
    SH_CHECK(state.Submit(transport) == TransportResult::Success);

    const auto reports = transport.WrittenReports();
    SH_CHECK(reports.size() == 2);
    const auto& release = reports.back();
    SH_CHECK((release[offsets::kValidFlag0] & valid_flag0::kSpeakerVolume) != 0);
    SH_CHECK(release[offsets::kSpeakerVolume] == 0);
    SH_CHECK(release[offsets::kAudioControl] == 0);

    // The release is a one-shot: the next report no longer claims audio.
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(2, 3));
    SH_CHECK(state.Submit(transport) == TransportResult::Success);
    const auto after = transport.WrittenReports().back();
    SH_CHECK((after[offsets::kValidFlag0] & valid_flag0::kSpeakerVolume) == 0);
}

SH_TEST(OutputState_ResetToNeutralStillClaimsTheTriggersSoTheReleaseIsSent) {
    DualSenseOutputState state;
    state.SetTrigger(TriggerSide::Left, TriggerEffectSpec::MakeFeedback(4, 7));
    state.SetLegacyRumble(200, 200);
    state.ResetToNeutral();

    const auto report = state.BuildReport();
    SH_CHECK(report[offsets::kLeftTriggerBlock] == 0x05);
    SH_CHECK((report[offsets::kValidFlag0] & valid_flag0::kLeftTriggerEffect) != 0);
    SH_CHECK(report[offsets::kMotorLeft] == 0);
    SH_CHECK(report[offsets::kMotorRight] == 0);
}

// --- byte-identity with the reports this project already verified ---------
//
// These pin the claim made in DualSenseOutputState.hpp: adopting the
// single-owner state does not change a single byte that previously reached
// the device on a verified path. If someone later "improves" the composition,
// these fail rather than the hardware behaviour silently changing.

SH_TEST(OutputState_FullAudioClaimMatchesTheVerifiedSpeakerOutputReport) {
    dualsense_protocol::SpeakerOutputSettings legacy;
    legacy.speakerVolume = 0x64;
    legacy.headphoneVolume = 0x64;
    legacy.outputPath = 3;
    legacy.speakerPreGain = 7;
    const auto expected = dualsense_protocol::BuildSpeakerOutputReport(legacy);

    DualSenseOutputState state;
    AudioOutputSettings audio;
    audio.speakerVolume = 0x64;
    audio.headphoneVolume = 0x64;
    audio.outputPath = 3;
    audio.speakerPreGain = 7;
    state.SetAudio(audio);
    const auto actual = state.BuildReport();

    for (std::size_t i = 0; i < expected.size(); ++i) SH_CHECK(actual[i] == expected[i]);
}

SH_TEST(OutputState_AudioReleaseMatchesTheVerifiedReleaseReport) {
    dualsense_protocol::SpeakerOutputSettings legacy;
    legacy.enable = false;
    const auto expected = dualsense_protocol::BuildSpeakerOutputReport(legacy);

    DualSenseOutputState state;
    state.SetAudio(AudioOutputSettings{});
    state.ReleaseAudio();
    const auto actual = state.BuildReport();

    for (std::size_t i = 0; i < expected.size(); ++i) SH_CHECK(actual[i] == expected[i]);
}

SH_TEST(OutputState_RoutingOnlyClaimMatchesTheNarrowRoutingReport) {
    const auto expected = dualsense_protocol::BuildAudioRoutingReport(true, 0x64, 3);

    DualSenseOutputState state;
    AudioOutputSettings audio;
    audio.routingOnly = true;
    audio.speakerVolume = 0x64;
    audio.outputPath = 3;
    state.SetAudio(audio);
    const auto actual = state.BuildReport();

    for (std::size_t i = 0; i < expected.size(); ++i) SH_CHECK(actual[i] == expected[i]);
}

SH_TEST(OutputState_LegacyRumbleClaimMatchesTheVerifiedRumbleReportFlags) {
    const auto expected = dualsense_protocol::BuildRumbleReport(120, 30);

    DualSenseOutputState state;
    state.SetLegacyRumble(120, 30);
    const auto actual = state.BuildReport();

    for (std::size_t i = 0; i < expected.size(); ++i) SH_CHECK(actual[i] == expected[i]);
}
