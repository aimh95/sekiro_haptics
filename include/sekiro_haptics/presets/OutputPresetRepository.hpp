#pragma once

// Loads OutputPreset entries from JSON, accepting both the original v1 file
// and the v2 file that adds the PCM/speaker/trigger layers.
//
// The validation policy is stricter than PresetRepository's on purpose. That
// repository clamps an out-of-range rumble intensity and loads the entry
// anyway; here an invalid entry is rejected, because an adaptive-trigger
// parameter outside its mode's range is not a loudness that can be nudged
// into place -- the firmware would reject it or, worse, read the byte as
// something else. "Reject bad config before any output" is the rule.
//
// FILE SHAPE
// ----------
//   {
//     "schemaVersion": 2,                 // absent => 1
//     "presets": [
//       {
//         "presetId": "guard.deflect",
//         "displayName": "Deflect",
//         "speaker":   [ { "cueId": "deflect.speaker", "gain": 1.0,
//                          "startOffsetMs": 0 } ],
//         "pcmHaptic": [ { "cueId": "deflect.haptic", "gain": 1.0,
//                          "balance": 0.0, "startOffsetMs": 0 } ],
//         "trigger":   [ { "side": "r2", "mode": "weapon",
//                          "startPosition": 3, "endPosition": 7,
//                          "strength": 8, "startOffsetMs": 0,
//                          "durationMs": 120 } ],
//         "legacyRumble": { "left": 0.4, "right": 0.8, "durationMs": 28,
//                           "startOffsetMs": 0 }     // optional
//       }
//     ]
//   }
//
// Trigger entries name only the parameters their mode has. A key belonging to
// another mode is an error, not an ignored extra -- silently dropping
// "frequency" from a feedback effect would hide a real mistake in the config.

#include "sekiro_haptics/presets/OutputPreset.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace sekiro_haptics {

struct OutputPresetLoadError {
    std::size_t entryIndex = 0;
    std::string presetId;   // empty when the entry had no usable id
    std::string message;
};

struct OutputPresetLoadOutcome {
    /// False only when the file could not be opened or its top-level shape is
    /// wrong. A rejected entry does not make the load fatal -- it appears in
    /// `errors` and is simply absent from the repository.
    bool ok = false;
    int schemaVersion = kOutputPresetSchemaVersionLegacy;
    std::size_t loadedCount = 0;
    std::vector<OutputPresetLoadError> errors;
    std::string fatalError;
};

class OutputPresetRepository {
public:
    OutputPresetLoadOutcome LoadFromFile(const std::string& path);
    /// Same parser, for tests and for config embedded in a string.
    OutputPresetLoadOutcome LoadFromString(const std::string& json);

    void AddPreset(OutputPreset preset);
    const OutputPreset* Find(const std::string& presetId) const;
    std::size_t Size() const;
    std::vector<std::string> PresetIds() const;

private:
    std::unordered_map<std::string, OutputPreset> presets_;
};

} // namespace sekiro_haptics
