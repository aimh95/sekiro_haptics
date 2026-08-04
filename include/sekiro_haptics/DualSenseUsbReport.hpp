#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sekiro_haptics::dualsense_protocol {

/// Length in bytes of a DualSense USB output report.
inline constexpr std::size_t kUsbOutputReportLength = 64;

/// Report ID for a DualSense USB output report.
inline constexpr std::uint8_t kUsbOutputReportId = 0x02;

/// Builds a minimal DualSense USB output report that sets the two rumble
/// motor speeds and nothing else -- no adaptive-trigger, LED, or audio
/// fields are touched (they are left zeroed / disabled). This is
/// deliberately narrow: packet *construction* only, no HID/USB access.
/// Byte layout (report id, the "set main motors" flag bits, and the motor
/// offsets) follows the DualSense USB output report as documented in
/// flok/pydualsense (MIT licensed) -- see docs/ARCHITECTURE.md.
///
/// `leftMotor`/`rightMotor` are raw 0-255 motor strengths, not normalized
/// [0.0, 1.0] intensities; converting a HapticEffect's MotorIntensity to
/// this scale is a higher layer's job (e.g. a future DualSenseBackend), not
/// this function's.
std::array<std::uint8_t, kUsbOutputReportLength> BuildRumbleReport(std::uint8_t leftMotor, std::uint8_t rightMotor);

} // namespace sekiro_haptics::dualsense_protocol
