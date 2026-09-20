#pragma once

#include "sekiro_haptics/ActionFeedback.hpp"

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
/// flok/pydualsense (MIT licensed) -- see docs/01-architecture.md.
///
/// `leftMotor`/`rightMotor` are raw 0-255 motor strengths, not normalized
/// [0.0, 1.0] intensities; converting a HapticEffect's MotorIntensity to
/// this scale is a higher layer's job (e.g. a future DualSenseBackend), not
/// this function's.
std::array<std::uint8_t, kUsbOutputReportLength> BuildRumbleReport(std::uint8_t leftMotor, std::uint8_t rightMotor);

// Complete motor + trigger state. Explicitly sends off modes for neutral
// triggers. Does not change lights, microphone, audio routing, or volume.
// Resistance mode 0x01 uses start-position and force bytes; protocol basis:
// Ohjurot/DualSense-Windows DS5_Output.cpp and flok/pydualsense USB offsets.
std::array<std::uint8_t, kUsbOutputReportLength> BuildFeedbackReport(const FeedbackState& state);

// ---------------------------------------------------------------------------
// Built-in speaker output.
//
// The earlier version of this code treated the speaker volume byte as a 0..255
// scale and sent 0xFF for "maximum". That is almost certainly wrong: Linux's
// hid-playstation driver uses 0x64 (=100) for its 100% setting and the usable
// values observed there sit in 0x3D..0x64. 0xFF is outside that range, so the
// firmware may clamp or ignore it -- a plausible cause of "the speaker is
// barely audible". This is a comparison point taken from one driver, NOT a
// documented contract for every firmware, so every field below stays settable
// and the CLI can sweep them against real hardware.
//
// That same driver branch sets more than a volume: it selects the output path
// that sends the right-hand PCM source to the speaker, sets the flag that makes
// the volume byte take effect, and sets a speaker pre-gain in a second audio
// control byte together with ITS enable flag. The old report set neither the
// pre-gain nor anything in valid_flag1, which is the second suspect.
//
// Byte layout (report id first, then the common output-report struct), matching
// hid-playstation's dualsense_output_report_common field order. The trigger
// offsets already used by BuildFeedbackReport (11 and 22) line up with it,
// which is the cross-check that this mapping is the right one.
//   [0]  report id                [8]  audio_flags (output path in bits 4..5)
//   [1]  valid_flag0              [9]  mute_button_led
//   [2]  valid_flag1              [10] power_save_control
//   [3]  motor_right              [11] right trigger mode (+10 params)
//   [4]  motor_left               [22] left trigger mode (+10 params)
//   [5]  headphone_audio_volume   [37] reduce_motor_power
//   [6]  speaker_audio_volume     [38] audio_flags2 (speaker pre-gain, low bits)
//   [7]  internal_microphone_vol  [39] valid_flag2
// ---------------------------------------------------------------------------
struct SpeakerOutputSettings {
    bool enable = true;
    /// 0x64 is hid-playstation's 100%. Values below ~0x3D were not observed to
    /// be used there. NOT a 0..255 scale.
    std::uint8_t speakerVolume = 0x64;
    std::uint8_t headphoneVolume = 0x64;
    /// audio_flags bits 4..5 -- which PCM source reaches the speaker.
    std::uint8_t outputPath = 3;
    /// audio_flags2 low 3 bits -- speaker pre-gain.
    std::uint8_t speakerPreGain = 0x07;
    /// Which fields the device should apply. Defaults enable the headphone,
    /// speaker and mic volume bytes plus the audio-control byte.
    std::uint8_t validFlag0 = 0x10 | 0x20 | 0x40 | 0x80;
    /// The enable for the second audio-control byte. Which bit this is has not
    /// been confirmed on this firmware -- sweep it rather than trusting it.
    std::uint8_t validFlag1 = 0x80;
};

/// Full speaker-output report. Touches only audio fields: motors, triggers,
/// LEDs and microphone mute are left zeroed/untouched.
std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerOutputReport(const SpeakerOutputSettings& settings);

// Backwards-compatible wrappers. The one-argument form keeps its original
// bytes so existing tests still describe what they described.
std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerRoutingReport(bool enable);

// Same routing report with an explicit built-in-speaker volume byte. The
// one-argument overload above keeps its original 0x50 value; this overload
// exists because 0x50 is audibly quiet on the built-in speaker and the level
// has to be adjustable rather than baked in. Ignored when `enable` is false,
// which always relinquishes routing and sets the volume byte to zero.
std::array<std::uint8_t, kUsbOutputReportLength> BuildSpeakerRoutingReport(bool enable,
                                                                           std::uint8_t speakerVolume);

// Same again with the audio output-path selector left to the caller. The
// two-argument overload uses path 3, which is what the previous code hardcoded.
// The mapping from this value to "headphone / speaker / both" is NOT verified
// here: pass each candidate and listen. Only bits 4..5 of the byte are used.
std::array<std::uint8_t, kUsbOutputReportLength> BuildAudioRoutingReport(bool enable,
                                                                         std::uint8_t speakerVolume,
                                                                         std::uint8_t outputPath);

} // namespace sekiro_haptics::dualsense_protocol
