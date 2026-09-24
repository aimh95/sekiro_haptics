#pragma once

// DualSense adaptive-trigger effect encoding.
//
// SCOPE: this header turns a validated, mode-specific parameter set into the
// 11 bytes the trigger section of a USB output report expects. It performs no
// I/O, owns no device, and knows nothing about effect lifetime -- that is
// AdaptiveTriggerRuntime's job, and report assembly is DualSenseOutputState's.
//
// WHY EACH MODE CARRIES ITS OWN PARAMETERS
// ----------------------------------------
// The trigger modes do NOT share a parameter vocabulary. Feedback has a
// position and a strength; Weapon has a start, an end and a strength;
// Vibration has a position, an amplitude and a frequency in hertz. There is no
// "frequency" in Feedback and no "end position" in Vibration. Modelling them
// with one flat struct of every field would invite sending a number in a byte
// the firmware reads as something else, so each mode has its own struct and
// its own validation, and the encoder reads only the struct its mode names.
//
// PROTOCOL SOURCES (recorded because none of this is guessed)
// -----------------------------------------------------------
//  [1] Nielk1, "DualSense Trigger Effect Generator", Revision 6.
//      https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db
//      Retrieved 2026-09-20. Source of: the official mode bytes (Off 0x05,
//      Feedback 0x21, Weapon 0x25, Vibration 0x26), the zone bit-packing
//      below, the documented parameter ranges, and the rule that a parameter
//      set producing no effect is sent as Off rather than as a zero-strength
//      effect.
//  [2] Linux kernel drivers/hid/hid-playstation.c -- struct
//      dualsense_output_report_common (static_assert size 47) and the
//      DS_OUTPUT_VALID_FLAG* macros. Retrieved 2026-09-20 from
//      git.kernel.org/.../plain/drivers/hid/hid-playstation.c. Source of: the
//      field order that puts reserved2[27] at report bytes 11..37, which is
//      where [3] and this project write the two trigger blocks, and of
//      audio_control2 at byte 38 / valid_flag2 at byte 39.
//  [3] Ohjurot/DualSense-Windows, DS5_Output.cpp. Retrieved 2026-09-20.
//      Source of: the right trigger block at report byte 11 and the left at
//      22, and of mode 0x01 "continuous resistance" taking
//      [start position, force] as raw bytes.
//  [4] flok/pydualsense, pydualsense/enums.py. Retrieved 2026-09-20.
//      Cross-check on the mode byte values.
//
// KNOWN DISAGREEMENT BETWEEN SOURCES, NOT RESOLVED HERE: [3] documents mode
// 0x02 as "section resistance" taking [start, force], while [1] documents the
// same byte as Simple_Weapon taking [start, end, strength]. Because the two
// cannot both be right and neither was verified on this controller, mode 0x02
// is NOT implemented. Use Weapon (0x25), which both sources agree on.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sekiro_haptics::dualsense {

/// Which physical trigger. L2 is the left trigger, R2 the right. They are
/// always addressed separately: nothing in this project ever sets one as a
/// side effect of setting the other.
enum class TriggerSide { Left, Right };

const char* ToString(TriggerSide side);
std::optional<TriggerSide> ParseTriggerSide(std::string_view text);

/// Mode byte written to the first byte of a trigger block.
///
/// Only the four modes [1] calls "official" are listed, plus the one legacy
/// mode this repository already shipped. The unofficial firmware leftovers
/// (Bow 0x22, Galloping 0x23, Machine 0x27) and the debug/calibration modes
/// (0xFC..0xFE) are deliberately absent: [1] states the debug modes corrupt
/// the trigger state until the controller is reset with the physical button,
/// which is not a state this project is willing to be able to reach.
enum class TriggerEffectMode : std::uint8_t {
    /// Effect off, stop returned to neutral. [1]
    Off = 0x05,
    /// Resists movement past `position`, to the end of travel. [1]
    Feedback = 0x21,
    /// Resists between `startPosition` and `endPosition`, then releases --
    /// the "weapon trigger" shape. [1]
    Weapon = 0x25,
    /// Vibrates past `position` at an amplitude and a frequency. [1]
    Vibration = 0x26,
    /// The mode this repository already used (see
    /// dualsense_protocol::BuildFeedbackReport). [1] calls it Simple_Feedback
    /// and recommends Feedback instead; it is kept so nothing that already
    /// works changes, and because its parameters are raw 0..255 bytes rather
    /// than zone indices, which is a different (coarser) control.
    SimpleFeedback = 0x01,
};

const char* ToString(TriggerEffectMode mode);
std::optional<TriggerEffectMode> ParseTriggerEffectMode(std::string_view text);

/// Trigger travel is divided into 10 zones, indexed 0 (untouched) to 9 (fully
/// pressed). These are ZONE INDICES -- not millimetres, not a percentage.
inline constexpr std::uint8_t kMaxZoneIndex = 9;
/// Strength/amplitude is a 1..8 scale packed into 3 bits, with 0 meaning "no
/// effect". It is not a percentage and not a force in newtons. [1]
inline constexpr std::uint8_t kMaxStrength = 8;

/// Feedback (0x21): constant resistance from `position` to the end of travel.
struct FeedbackParams {
    /// Zone the resistance begins in, 0..9.
    std::uint8_t position = 0;
    /// 0..8. 0 means no effect and is encoded as Off.
    std::uint8_t strength = 0;
};

/// Weapon (0x25): resistance between two zones, releasing past the end.
struct WeaponParams {
    /// 2..7. [1] rejects a start below 2.
    std::uint8_t startPosition = 2;
    /// startPosition+1 .. 8.
    std::uint8_t endPosition = 5;
    /// 0..8. 0 means no effect and is encoded as Off.
    std::uint8_t strength = 0;
};

/// Vibration (0x26): the trigger cycles past `position`.
struct VibrationParams {
    /// Zone the vibration begins in, 0..9.
    std::uint8_t position = 0;
    /// 0..8. 0 means no effect and is encoded as Off.
    std::uint8_t amplitude = 0;
    /// Cycling rate in HERTZ, sent as one byte, so 1..255. [1] documents no
    /// upper limit; 0 means no effect and is encoded as Off. What the
    /// actuator actually does at a given value was not measured here.
    std::uint8_t frequencyHz = 0;
};

/// SimpleFeedback (0x01): the legacy mode, raw bytes rather than zones.
struct SimpleFeedbackParams {
    /// Raw 0..255 start position. NOT a zone index.
    std::uint8_t startPosition = 0;
    /// Raw 0..255 force. NOT the 0..8 strength scale.
    std::uint8_t force = 0;
};

/// A complete effect request. Only the member matching `mode` is read; the
/// others keep their defaults and are never encoded.
struct TriggerEffectSpec {
    TriggerEffectMode mode = TriggerEffectMode::Off;
    FeedbackParams feedback{};
    WeaponParams weapon{};
    VibrationParams vibration{};
    SimpleFeedbackParams simple{};

    static TriggerEffectSpec MakeOff();
    static TriggerEffectSpec MakeFeedback(std::uint8_t position, std::uint8_t strength);
    static TriggerEffectSpec MakeWeapon(std::uint8_t startPosition, std::uint8_t endPosition,
                                        std::uint8_t strength);
    static TriggerEffectSpec MakeVibration(std::uint8_t position, std::uint8_t amplitude,
                                           std::uint8_t frequencyHz);
    static TriggerEffectSpec MakeSimpleFeedback(std::uint8_t startPosition, std::uint8_t force);
};

/// One trigger's bytes inside an output report: the mode byte plus 10
/// parameter bytes, matching reserved2's layout in [2] and the block size in
/// [1] and [3].
inline constexpr std::size_t kTriggerBlockBytes = 11;
using TriggerBlock = std::array<std::uint8_t, kTriggerBlockBytes>;

/// Outcome of encoding, kept richer than a bool so "your parameters meant
/// nothing, so the trigger was released" is visible rather than silent.
struct TriggerEncodeResult {
    bool ok = false;
    /// True when valid parameters described no effect (strength 0, amplitude
    /// 0, frequency 0) and Off was encoded instead, following [1].
    bool reducedToOff = false;
    /// Populated only when ok is false. Names the parameter and its range.
    std::string error;
};

/// Validates `spec` against its mode's documented ranges. Out-of-range values
/// are rejected, never clamped: a clamped resistance is a different effect
/// from the one asked for, and silently changing it would make a comparison
/// between two settings meaningless.
TriggerEncodeResult ValidateTriggerEffect(const TriggerEffectSpec& spec);

/// Validates and encodes. `out` is left untouched when validation fails.
TriggerEncodeResult EncodeTriggerEffect(const TriggerEffectSpec& spec, TriggerBlock& out);

/// The neutral block: mode Off, all parameters zero.
TriggerBlock OffTriggerBlock();

/// Human-readable one-line description, for CLI output and logs.
std::string DescribeTriggerEffect(const TriggerEffectSpec& spec);

} // namespace sekiro_haptics::dualsense
