#include "sekiro_haptics/DualSenseUsbReport.hpp"

#include <algorithm>
#include <cmath>

namespace sekiro_haptics::dualsense_protocol {

namespace {

// "Set the main motors" flag bits in byte[1] of the USB output report.
// Both bits are required together: 0x01 alone lets rumble gracefully hand
// back off to audio-haptics on timeout, 0x02 alone would let motors time
// out without doing so. Setting both is the documented way to explicitly
// drive rumble.
constexpr std::uint8_t kEnableMainMotorsFlags = 0x01 | 0x02;

constexpr std::size_t kFlagsOffset = 1;
constexpr std::size_t kRightMotorOffset = 3;
constexpr std::size_t kLeftMotorOffset = 4;

} // namespace

std::array<std::uint8_t, kUsbOutputReportLength> BuildRumbleReport(std::uint8_t leftMotor, std::uint8_t rightMotor) {
    std::array<std::uint8_t, kUsbOutputReportLength> report{};
    report[0] = kUsbOutputReportId;
    report[kFlagsOffset] = kEnableMainMotorsFlags;
    report[kRightMotorOffset] = rightMotor;
    report[kLeftMotorOffset] = leftMotor;
    return report;
}

std::array<std::uint8_t, kUsbOutputReportLength> BuildFeedbackReport(const FeedbackState& state) {
    auto byte = [](float value) -> std::uint8_t {
        if (!std::isfinite(value)) return 0;
        return static_cast<std::uint8_t>(std::lround(std::clamp(value,0.0f,1.0f)*255.0f));
    };
    auto report = BuildRumbleReport(byte(state.leftMotor),byte(state.rightMotor));
    report[1] |= 0x04 | 0x08;
    auto trigger = [&](std::size_t offset, const TriggerResistance& resistance) {
        if (!std::isfinite(resistance.start)) return;
        const auto force = byte(resistance.force);
        if (force == 0) return; // zero mode + zero parameters = explicit release
        report[offset] = 0x01;
        report[offset+1] = byte(resistance.start);
        report[offset+2] = force;
    };
    trigger(11,state.rightTrigger);
    trigger(22,state.leftTrigger);
    return report;
}

std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerOutputReport(const SpeakerOutputSettings& s) {
    std::array<std::uint8_t,kUsbOutputReportLength> report{};
    report[0] = kUsbOutputReportId;
    if (!s.enable) {
        // Hand the speaker back: same apply flags, all audio values zeroed, so
        // the device returns to its default routing instead of being left with
        // whatever this app configured.
        report[1] = s.validFlag0;
        report[2] = s.validFlag1;
        return report;
    }
    report[1] = s.validFlag0;
    report[2] = s.validFlag1;
    report[5] = s.headphoneVolume;
    report[6] = s.speakerVolume;
    report[8] = static_cast<std::uint8_t>((s.outputPath & 0x03) << 4);
    report[38] = static_cast<std::uint8_t>(s.speakerPreGain & 0x07);
    return report;
}

std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerRoutingReport(bool enable) {
    return BuildSpeakerRoutingReport(enable, 0x50);
}

std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerRoutingReport(bool enable,
                                                                           std::uint8_t speakerVolume) {
    return BuildAudioRoutingReport(enable, speakerVolume, 3);
}

std::array<std::uint8_t, kUsbOutputReportLength> BuildAudioRoutingReport(bool enable,
                                                                         std::uint8_t speakerVolume,
                                                                         std::uint8_t outputPath) {
    std::array<std::uint8_t,kUsbOutputReportLength> report{};
    report[0] = kUsbOutputReportId;
    report[1] = 0x20 | 0x80; // speaker volume + audio routing controls
    report[6] = enable ? speakerVolume : 0;
    report[8] = enable ? static_cast<std::uint8_t>((outputPath & 0x03) << 4) : 0; // bits 4..5
    // Protocol fields: duaLib/src/include/dataStructures.h SetStateData,
    // duaLib.h SCE_PAD_AUDIO_PATH_ONLY_SPEAKER. Authored implementation.
    return report;
}

} // namespace sekiro_haptics::dualsense_protocol
