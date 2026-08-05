#pragma once

#include "sekiro_haptics/presets/HapticPreset.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace sekiro_haptics {

/// One problem found while loading a single entry from a presets JSON file.
/// `entryIndex` is the entry's position in the top-level "presets" array
/// (0-based). Not every PresetLoadError means the entry was rejected --
/// see PresetRepository::LoadFromFile's validation policy.
struct PresetLoadError {
    std::size_t entryIndex = 0;
    std::string message;
};

/// Result of PresetRepository::LoadFromFile().
struct PresetLoadOutcome {
    /// False only if the file could not be opened or its top-level content
    /// was not valid JSON / not the expected shape (a JSON object with a
    /// "presets" array). Individual malformed entries do not set this to
    /// false -- see `errors`.
    bool ok = false;
    std::size_t loadedCount = 0;
    std::vector<PresetLoadError> errors;
    std::string fatalError;
};

/// Loads and looks up HapticPreset instances by `presetId`.
///
/// Validation policy for LoadFromFile (see docs/03-trace-format.md for the
/// full schema): an entry missing "presetId", "displayName", or
/// "legacyRumble" (or with a non-positive "durationMs") is rejected --
/// recorded in `errors` and not added. An entry whose "left"/"right"
/// intensity is outside [0,1] is clamped into range and still loaded, with
/// the clamp recorded in `errors` as a warning. "left"/"right" default to
/// 0.0 if absent.
class PresetRepository {
public:
    PresetLoadOutcome LoadFromFile(const std::string& path);

    /// Inserts or replaces the preset with this presetId.
    void AddPreset(HapticPreset preset);

    /// Returns nullptr if no preset with this id has been loaded/added.
    const HapticPreset* Find(const std::string& presetId) const;

    std::size_t Size() const;

private:
    std::unordered_map<std::string, HapticPreset> presets_;
};

} // namespace sekiro_haptics
