#include "sekiro_haptics/DualSenseUsbReport.hpp"

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

} // namespace sekiro_haptics::dualsense_protocol
