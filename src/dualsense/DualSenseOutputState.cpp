#include "sekiro_haptics/dualsense/DualSenseOutputState.hpp"

#include <algorithm>

namespace sekiro_haptics::dualsense {

const char* ToString(HapticPath path) {
    switch (path) {
        case HapticPath::Unclaimed: return "unclaimed";
        case HapticPath::Pcm: return "pcm";
        case HapticPath::LegacyRumble: return "legacy-rumble";
    }
    return "unknown";
}

DualSenseOutputState::DualSenseOutputState()
    : leftTrigger_(OffTriggerBlock()), rightTrigger_(OffTriggerBlock()) {}

void DualSenseOutputState::SetTriggerBlock(TriggerSide side, const TriggerBlock& block) {
    auto& target = side == TriggerSide::Left ? leftTrigger_ : rightTrigger_;
    auto& claimed = side == TriggerSide::Left ? leftTriggerClaimed_ : rightTriggerClaimed_;
    if (claimed && target == block) return;   // nothing changed; do not dirty
    target = block;
    claimed = true;
    dirty_ = true;
}

TriggerEncodeResult DualSenseOutputState::SetTrigger(TriggerSide side, const TriggerEffectSpec& spec) {
    TriggerBlock block{};
    const auto result = EncodeTriggerEffect(spec, block);
    if (!result.ok) {
        lastError_ = result.error;
        return result;   // state untouched: a rejected spec never half-applies
    }
    SetTriggerBlock(side, block);
    return result;
}

void DualSenseOutputState::ReleaseTrigger(TriggerSide side) {
    SetTriggerBlock(side, OffTriggerBlock());
}

const TriggerBlock& DualSenseOutputState::Trigger(TriggerSide side) const {
    return side == TriggerSide::Left ? leftTrigger_ : rightTrigger_;
}

bool DualSenseOutputState::TriggerClaimed(TriggerSide side) const {
    return side == TriggerSide::Left ? leftTriggerClaimed_ : rightTriggerClaimed_;
}

void DualSenseOutputState::SetLegacyRumble(std::uint8_t leftMotor, std::uint8_t rightMotor) {
    if (motorsClaimed_ && leftMotor_ == leftMotor && rightMotor_ == rightMotor) return;
    leftMotor_ = leftMotor;
    rightMotor_ = rightMotor;
    motorsClaimed_ = true;
    dirty_ = true;
}

void DualSenseOutputState::ReleaseLegacyRumble() {
    if (!motorsClaimed_ && leftMotor_ == 0 && rightMotor_ == 0) return;
    // Send the zeroes once with the flags still set, then stop claiming, so
    // the motors actually stop instead of holding their last speed.
    leftMotor_ = 0;
    rightMotor_ = 0;
    motorsClaimed_ = false;
    dirty_ = true;
}

void DualSenseOutputState::SetAudio(const AudioOutputSettings& settings) {
    const bool same = audioClaimed_ && !audioReleasePending_ &&
                      audio_.speakerVolume == settings.speakerVolume &&
                      audio_.headphoneVolume == settings.headphoneVolume &&
                      audio_.outputPath == settings.outputPath &&
                      audio_.speakerPreGain == settings.speakerPreGain &&
                      audio_.routingOnly == settings.routingOnly;
    if (same) return;
    audio_ = settings;
    audioClaimed_ = true;
    audioReleasePending_ = false;
    dirty_ = true;
}

void DualSenseOutputState::ReleaseAudio() {
    if (!audioClaimed_ && !audioReleasePending_) return;
    audioClaimed_ = false;
    audioReleasePending_ = true;
    dirty_ = true;
}

void DualSenseOutputState::MarkPcmHapticsActive(bool active) {
    pcmHapticsActive_ = active;
}

HapticPath DualSenseOutputState::HapticPathInUse() const {
    // The motor claim wins when both are set, because HAPTICS_SELECT is what
    // the controller actually acts on -- saying "PCM" while that flag is
    // asserted would be reporting an intention rather than the device state.
    if (motorsClaimed_) return HapticPath::LegacyRumble;
    if (pcmHapticsActive_) return HapticPath::Pcm;
    return HapticPath::Unclaimed;
}

std::uint8_t DualSenseOutputState::ComposeValidFlag0() const {
    std::uint8_t flags = 0;
    if (motorsClaimed_) flags |= valid_flag0::kCompatibleVibration | valid_flag0::kHapticsSelect;
    if (rightTriggerClaimed_) flags |= valid_flag0::kRightTriggerEffect;
    if (leftTriggerClaimed_) flags |= valid_flag0::kLeftTriggerEffect;
    if (audioClaimed_ || audioReleasePending_) {
        if (audio_.routingOnly) {
            flags |= valid_flag0::kSpeakerVolume | valid_flag0::kAudioControl;
        } else {
            flags |= valid_flag0::kHeadphoneVolume | valid_flag0::kSpeakerVolume |
                     valid_flag0::kMicVolume | valid_flag0::kAudioControl;
        }
    }
    return flags;
}

std::uint8_t DualSenseOutputState::ComposeValidFlag1() const {
    std::uint8_t flags = 0;
    if ((audioClaimed_ || audioReleasePending_) && !audio_.routingOnly)
        flags |= valid_flag1::kAudioControl2;
    return flags;
}

OutputReport DualSenseOutputState::BuildReport() const {
    OutputReport report{};
    report[offsets::kReportId] = kOutputReportId;
    report[offsets::kValidFlag0] = ComposeValidFlag0();
    report[offsets::kValidFlag1] = ComposeValidFlag1();

    // Motor bytes are written whenever they are non-zero OR the section is
    // claimed, so a release transmits the zeroes rather than omitting them.
    report[offsets::kMotorRight] = rightMotor_;
    report[offsets::kMotorLeft] = leftMotor_;

    if (audioClaimed_) {
        report[offsets::kSpeakerVolume] = audio_.speakerVolume;
        report[offsets::kAudioControl] =
            static_cast<std::uint8_t>((audio_.outputPath & 0x03) << 4);
        if (!audio_.routingOnly) {
            report[offsets::kHeadphoneVolume] = audio_.headphoneVolume;
            report[offsets::kAudioControl2] = static_cast<std::uint8_t>(audio_.speakerPreGain & 0x07);
        }
    }
    // When audioReleasePending_ is set the flags above are present and every
    // audio value stays zero -- that is the release.

    if (rightTriggerClaimed_)
        std::copy(rightTrigger_.begin(), rightTrigger_.end(),
                  report.begin() + offsets::kRightTriggerBlock);
    if (leftTriggerClaimed_)
        std::copy(leftTrigger_.begin(), leftTrigger_.end(),
                  report.begin() + offsets::kLeftTriggerBlock);

    return report;
}

void DualSenseOutputState::MarkDirty() {
    dirty_ = true;
}

TransportResult DualSenseOutputState::Submit(IDualSenseTransport& transport, bool force) {
    if (!dirty_ && !force) {
        ++stats_.skippedUnchanged;
        return TransportResult::Success;
    }
    if (!transport.IsOpen()) {
        ++stats_.notOpen;
        lastError_ = "transport is not open";
        return TransportResult::NotOpen;
    }

    const auto report = BuildReport();
    const auto result = transport.WriteOutputReport(report.data(), report.size());
    if (result != TransportResult::Success) {
        ++stats_.writeFailures;
        lastError_ = std::string("write failed: ") + ToString(result);
        // dirty_ stays set: the device is not known to have taken this state.
        return result;
    }

    ++stats_.submits;
    dirty_ = false;
    lastError_.clear();
    // The audio release has now been transmitted once; stop re-sending it.
    audioReleasePending_ = false;
    return TransportResult::Success;
}

void DualSenseOutputState::ResetToNeutral() {
    // Triggers stay CLAIMED while being set to Off, because an unclaimed
    // section's flag is clear and the firmware would ignore the release.
    SetTriggerBlock(TriggerSide::Left, OffTriggerBlock());
    SetTriggerBlock(TriggerSide::Right, OffTriggerBlock());
    ReleaseLegacyRumble();
    ReleaseAudio();
    pcmHapticsActive_ = false;
    dirty_ = true;
}

void DualSenseOutputState::ForgetClaims() {
    leftTrigger_ = OffTriggerBlock();
    rightTrigger_ = OffTriggerBlock();
    leftTriggerClaimed_ = false;
    rightTriggerClaimed_ = false;
    leftMotor_ = 0;
    rightMotor_ = 0;
    motorsClaimed_ = false;
    audio_ = AudioOutputSettings{};
    audioClaimed_ = false;
    audioReleasePending_ = false;
    pcmHapticsActive_ = false;
    dirty_ = false;
    lastError_.clear();
}

} // namespace sekiro_haptics::dualsense
