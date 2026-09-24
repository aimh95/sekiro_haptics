#pragma once

// The one owner of what this process has told the controller to do.
//
// WHY THIS EXISTS
// ---------------
// Before this type, three independent builders each produced a freshly zeroed
// 64-byte report: BuildRumbleReport (motors), BuildFeedbackReport (motors +
// triggers) and BuildAudioRoutingReport (speaker volume + output path). Each
// one set its own valid flags and left every other section's flags clear.
// That is survivable as long as exactly one of them is ever used, but the
// moment adaptive triggers and speaker routing are live at the same time it
// is not: BuildFeedbackReport sets the two trigger-enable flags on EVERY
// send, so a rumble update issued for an unrelated reason rewrites both
// triggers -- with whatever happened to be in that caller's FeedbackState.
//
// So the state lives here instead, in one place, and every submission carries
// the complete picture of the sections this process has claimed. Changing a
// trigger cannot drop the audio routing, and configuring audio cannot release
// a trigger, because both are re-encoded from the same state on every write.
//
// CLAIMED SECTIONS, NOT "SET EVERYTHING"
// --------------------------------------
// A valid flag is what tells the firmware to APPLY a field; a field whose
// flag is clear is ignored. This type therefore tracks, per section, whether
// this process has taken it over, and sets flags only for claimed sections.
// Nothing here ever claims the lightbar, the player indicator, the mic mute
// LED or power-save control, so those keep whatever the system set.
//
// THE RUMBLE / PCM-HAPTIC MODE CONFLICT IS EXPLICIT
// -------------------------------------------------
// valid_flag0 bit 1 is HAPTICS_SELECT, which the Linux driver comments as
// "Select classic rumble style haptics and enable it" [2]. Claiming the
// motors therefore switches the actuators away from the PCM audio path this
// project uses for its guard cues. That is not a detail to discover by
// feel, so the motor section is claimed ONLY when a caller explicitly asks
// for legacy rumble, and HapticPathInUse() reports which path is live.
//
// Protocol sources are listed in AdaptiveTrigger.hpp; the numbered references
// below match that list.

#include "sekiro_haptics/IDualSenseTransport.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace sekiro_haptics::dualsense {

/// Length of the buffer the builders produce. The USB report itself is 63
/// bytes [2]; the extra byte is padding and HidApiDualSenseTransport resizes
/// to whatever the device's HID descriptor declares (48 on the controller
/// used here, which is the report id plus the 47-byte common struct, so every
/// field this type writes is inside it).
inline constexpr std::size_t kOutputReportBytes = 64;
inline constexpr std::uint8_t kOutputReportId = 0x02;

using OutputReport = std::array<std::uint8_t, kOutputReportBytes>;

/// Byte offsets inside the report, from the field order of
/// struct dualsense_output_report_common [2], with the two trigger blocks
/// placed inside reserved2[27] where [1] and [3] put them.
namespace offsets {
inline constexpr std::size_t kReportId = 0;
inline constexpr std::size_t kValidFlag0 = 1;
inline constexpr std::size_t kValidFlag1 = 2;
inline constexpr std::size_t kMotorRight = 3;
inline constexpr std::size_t kMotorLeft = 4;
inline constexpr std::size_t kHeadphoneVolume = 5;
inline constexpr std::size_t kSpeakerVolume = 6;
inline constexpr std::size_t kMicVolume = 7;
inline constexpr std::size_t kAudioControl = 8;
inline constexpr std::size_t kRightTriggerBlock = 11;
inline constexpr std::size_t kLeftTriggerBlock = 22;
inline constexpr std::size_t kAudioControl2 = 38;
inline constexpr std::size_t kValidFlag2 = 39;
} // namespace offsets

/// valid_flag0 bits. 0..1 and 5..7 are from [2]; bits 2 and 3 (the two
/// trigger-effect enables) are from [3] and [4] -- the Linux driver does not
/// define them because it does not implement adaptive triggers. Bit 4
/// (headphone volume) is likewise from [3]/[4]: this kernel revision defines
/// bits 5, 6 and 7 but not 4.
namespace valid_flag0 {
inline constexpr std::uint8_t kCompatibleVibration = 0x01;
inline constexpr std::uint8_t kHapticsSelect = 0x02;
inline constexpr std::uint8_t kRightTriggerEffect = 0x04;
inline constexpr std::uint8_t kLeftTriggerEffect = 0x08;
inline constexpr std::uint8_t kHeadphoneVolume = 0x10;
inline constexpr std::uint8_t kSpeakerVolume = 0x20;
inline constexpr std::uint8_t kMicVolume = 0x40;
inline constexpr std::uint8_t kAudioControl = 0x80;
} // namespace valid_flag0

/// valid_flag1 bits, from [2].
namespace valid_flag1 {
inline constexpr std::uint8_t kAudioControl2 = 0x80;
} // namespace valid_flag1

/// The audio section. Defaults match what this project already sent, so
/// adopting this type does not change any byte that currently reaches the
/// device.
struct AudioOutputSettings {
    /// hid-playstation uses 0x64 for 100% and notes the accepted range looks
    /// like 0x3D..0x64 [2]. NOT a 0..255 scale.
    std::uint8_t speakerVolume = 0x64;
    std::uint8_t headphoneVolume = 0x64;
    /// audio_control bits 4..5, the output path selector [2].
    std::uint8_t outputPath = 3;
    /// audio_control2 bits 0..2, the speaker pre-amp gain [2].
    std::uint8_t speakerPreGain = 0x07;
    /// Send ONLY the two bytes the existing audio-routing report sends: the
    /// speaker volume and the output-path selector, with just their two apply
    /// flags. This reproduces BuildAudioRoutingReport byte for byte.
    ///
    /// It exists because that narrower report is the one this project has
    /// actually verified on the hardware (docs/11-guard-feedback.md section 1),
    /// and adopting the single-owner state must not quietly start sending the
    /// headphone-volume, mic-volume and pre-gain fields on a path where they
    /// were never sent before.
    bool routingOnly = false;
};

/// Which haptic path this process has put the controller on.
enum class HapticPath {
    /// Nothing claimed. The controller keeps whatever it had.
    Unclaimed,
    /// PCM over the USB-audio endpoint. The motor section is deliberately
    /// NOT claimed, so HAPTICS_SELECT stays clear.
    Pcm,
    /// Legacy rumble motors. Claiming this sets HAPTICS_SELECT, which takes
    /// the actuators off the PCM path [2].
    LegacyRumble,
};

const char* ToString(HapticPath path);

/// Counters, so "the trigger did not change" can be told apart from "the
/// write never happened".
struct OutputSubmitStats {
    std::uint64_t submits = 0;          // Submit() calls that actually wrote
    std::uint64_t skippedUnchanged = 0; // Submit() calls with nothing to send
    std::uint64_t writeFailures = 0;
    std::uint64_t notOpen = 0;
};

/// Single-owner HID output state. Not thread-safe by itself: the owning
/// runtime serialises access (see AdaptiveTriggerRuntime).
class DualSenseOutputState {
public:
    DualSenseOutputState();

    // --- triggers ---------------------------------------------------------

    /// Stores an already-encoded block for one side. Claims that side's
    /// section; the other side and every other section are untouched.
    void SetTriggerBlock(TriggerSide side, const TriggerBlock& block);

    /// Encodes and stores. Returns the encode result; on failure the stored
    /// state is unchanged, so a rejected parameter set cannot half-apply.
    TriggerEncodeResult SetTrigger(TriggerSide side, const TriggerEffectSpec& spec);

    /// Sets the side to the neutral Off block, keeping the section claimed so
    /// the release is actually transmitted.
    void ReleaseTrigger(TriggerSide side);

    const TriggerBlock& Trigger(TriggerSide side) const;
    bool TriggerClaimed(TriggerSide side) const;

    // --- motors -----------------------------------------------------------

    /// Claims the motor section and sets both raw 0..255 speeds. This puts the
    /// controller on the legacy rumble path -- see the header comment.
    void SetLegacyRumble(std::uint8_t leftMotor, std::uint8_t rightMotor);

    /// Zeroes the motors and unclaims the section, so subsequent reports stop
    /// asserting HAPTICS_SELECT and the actuators can return to PCM.
    void ReleaseLegacyRumble();

    std::uint8_t LeftMotor() const { return leftMotor_; }
    std::uint8_t RightMotor() const { return rightMotor_; }

    // --- audio ------------------------------------------------------------

    /// Claims the audio section (volumes, output path, pre-gain).
    void SetAudio(const AudioOutputSettings& settings);

    /// Hands the audio section back: the apply flags are still sent, with the
    /// values zeroed, so the device returns to its default routing instead of
    /// being left on whatever this process configured. Matches what
    /// BuildAudioRoutingReport(false, ...) did.
    void ReleaseAudio();

    bool AudioClaimed() const { return audioClaimed_; }
    const AudioOutputSettings& Audio() const { return audio_; }

    /// Declares that PCM haptics are in use. Purely bookkeeping for
    /// HapticPathInUse(); it writes no byte, because the PCM path is the USB
    /// audio endpoint and not part of this report.
    void MarkPcmHapticsActive(bool active);

    HapticPath HapticPathInUse() const;

    // --- report / transmission -------------------------------------------

    /// The complete report for the current state. Deterministic and free of
    /// side effects, which is what makes it testable without a device.
    OutputReport BuildReport() const;

    /// True when the state has changed since the last successful Submit().
    bool Dirty() const { return dirty_; }

    /// Forces the next Submit() to write even if nothing changed. Used after
    /// a reconnect, where the device's state is unknown.
    void MarkDirty();

    /// Writes the current report if the state changed (or `force`). A
    /// successful write clears the dirty flag; a failed one leaves it set so
    /// the next attempt retries rather than assuming the device agreed.
    TransportResult Submit(IDualSenseTransport& transport, bool force = false);

    /// Returns the state to neutral IN MEMORY: both triggers Off (still
    /// claimed, so the release transmits), motors zeroed and unclaimed, audio
    /// released if it was claimed. Does not write -- call Submit() after.
    void ResetToNeutral();

    /// Drops every claim without transmitting anything, for use when the
    /// device has gone away and a write is meaningless. The next Submit()
    /// after a reconnect therefore starts from a known-neutral state instead
    /// of replaying what was set before the disconnect.
    void ForgetClaims();

    const OutputSubmitStats& Stats() const { return stats_; }
    const std::string& LastError() const { return lastError_; }

private:
    std::uint8_t ComposeValidFlag0() const;
    std::uint8_t ComposeValidFlag1() const;

    TriggerBlock leftTrigger_;
    TriggerBlock rightTrigger_;
    bool leftTriggerClaimed_ = false;
    bool rightTriggerClaimed_ = false;

    std::uint8_t leftMotor_ = 0;
    std::uint8_t rightMotor_ = 0;
    bool motorsClaimed_ = false;

    AudioOutputSettings audio_;
    bool audioClaimed_ = false;
    /// Set by ReleaseAudio(): send the apply flags with zeroed values once,
    /// then stop claiming the section.
    bool audioReleasePending_ = false;

    bool pcmHapticsActive_ = false;

    bool dirty_ = false;
    OutputSubmitStats stats_;
    std::string lastError_;
};

} // namespace sekiro_haptics::dualsense
