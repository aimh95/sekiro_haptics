#pragma once

// What one game event should produce on the controller, across every output
// the hardware has: PCM haptics, the built-in speaker, the two adaptive
// triggers, and the legacy rumble motors.
//
// RELATION TO HapticPreset
// ------------------------
// HapticPreset (presets/HapticPreset.hpp) can express exactly one thing: a
// pair of motor intensities for a duration. Its own header already said that a
// future PCM/adaptive-trigger preset should arrive by extending the preset
// representation rather than by growing HapticEffectType, and this is that
// type. OutputPreset is a superset: a v1 preset file still loads, and its
// single legacyRumble block becomes this type's `legacyRumble` layer with
// every other layer list empty. Nothing that reads HapticPreset changes.
//
// INDEPENDENT TIMING IS THE POINT
// -------------------------------
// Each layer carries its OWN start offset and its own lifetime, because the
// outputs do not share one. A guard cue's speaker clip runs 306 ms while its
// haptic runs 378 ms and a trigger effect might hold until the next event;
// making them share a duration is what produced the "the vibration cut off my
// sound" class of bug this runtime exists to remove. A layer's lifetime ends
// that layer and nothing else.
//
// UNITS AND DEFAULTS, STATED ONCE
// -------------------------------
//   startOffsetMs   milliseconds after the event's timestamp. 0 = immediately.
//                   Negative is rejected: an output cannot precede its cause.
//   durationMs      trigger layers only. 0 = hold until replaced or reset.
//                   PCM/speaker layers have no duration field at all -- their
//                   length is the clip's length, and cutting a clip short is
//                   exactly what the overlap policy forbids.
//   gain            linear multiplier on the clip's samples, 0..4. Not dB and
//                   not a percentage of anything physical.
//   balance         -1 fully left .. +1 fully right, PCM haptic layers only.
//   cueId           names a clip in the runtime's clip library. Resolution
//                   happens at bind time, before any output, so a preset
//                   naming a clip that does not exist is rejected rather than
//                   silently producing nothing.
//
// SCHEMA VERSIONING
// -----------------
//   version 1  the original file: presets[].legacyRumble only. Still loads.
//   version 2  adds presets[].speaker[], presets[].pcmHaptic[],
//              presets[].trigger[]. A v2 file may still carry legacyRumble.
// A file with no "schemaVersion" key is treated as version 1, which is what
// every file written before this change is.

#include "sekiro_haptics/HapticEffect.hpp"
#include "sekiro_haptics/presets/HapticPreset.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sekiro_haptics {

inline constexpr int kOutputPresetSchemaVersionLegacy = 1;
inline constexpr int kOutputPresetSchemaVersionCurrent = 2;

/// A clip sent to the controller's built-in speaker path.
struct SpeakerOutputLayer {
    std::string cueId;
    float gain = 1.0f;
    float startOffsetMs = 0.0f;
};

/// A clip sent to the voice-coil actuators over the same USB-audio endpoint.
/// Separate from the speaker layer on purpose: the two waveforms are designed
/// independently (see GuardCue.hpp) and neither is a filtered copy of the
/// other.
struct PcmHapticOutputLayer {
    std::string cueId;
    float gain = 1.0f;
    float balance = 0.0f;
    float startOffsetMs = 0.0f;
};

/// An adaptive-trigger effect on one side.
struct TriggerOutputLayer {
    dualsense::TriggerSide side = dualsense::TriggerSide::Right;
    dualsense::TriggerEffectSpec spec;
    float startOffsetMs = 0.0f;
    /// 0 holds until something replaces it or the runtime is reset.
    float durationMs = 0.0f;
};

/// The legacy motor pair. Kept optional rather than defaulted, because
/// claiming the motors switches the controller off the PCM haptic path
/// (see DualSenseOutputState.hpp) -- a preset must ask for that explicitly.
struct LegacyRumbleOutputLayer {
    HapticEffect effect;
    float startOffsetMs = 0.0f;
};

struct OutputPreset {
    std::string presetId;
    std::string displayName;
    int schemaVersion = kOutputPresetSchemaVersionCurrent;
    std::vector<SpeakerOutputLayer> speaker;
    std::vector<PcmHapticOutputLayer> pcmHaptic;
    std::vector<TriggerOutputLayer> trigger;
    std::optional<LegacyRumbleOutputLayer> legacyRumble;

    /// Total number of layers. The runtime compares this against the voices
    /// and trigger effects an event actually produced, which is why "one
    /// event = one voice" is never assumed anywhere.
    std::size_t LayerCount() const {
        return speaker.size() + pcmHaptic.size() + trigger.size() + (legacyRumble ? 1u : 0u);
    }
};

/// Every problem found in one preset. A preset with any problem is REJECTED,
/// not clamped: an output built from a silently corrected preset is not the
/// output the config asked for, and a comparison between two settings would
/// be meaningless.
struct OutputPresetValidation {
    bool ok = false;
    std::vector<std::string> errors;
};

/// Validates ranges, offsets and (for trigger layers) the mode's own
/// parameter rules. Does NOT check that cueIds resolve -- that needs the clip
/// library and happens at bind time.
OutputPresetValidation ValidateOutputPreset(const OutputPreset& preset);

/// Converts a v1 HapticPreset. Used so callers that already hold the old type
/// can move over without a file change.
OutputPreset FromLegacyPreset(const HapticPreset& legacy);

} // namespace sekiro_haptics
