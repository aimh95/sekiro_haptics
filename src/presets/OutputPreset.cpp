#include "sekiro_haptics/presets/OutputPreset.hpp"

#include <cmath>
#include <sstream>

namespace sekiro_haptics {

namespace {

void CheckOffset(const char* layer, std::size_t index, float offsetMs,
                 std::vector<std::string>& errors) {
    std::ostringstream where;
    where << layer << "[" << index << "]";
    if (!std::isfinite(offsetMs)) {
        errors.push_back(where.str() + ".startOffsetMs is not a finite number");
        return;
    }
    if (offsetMs < 0.0f)
        errors.push_back(where.str() + ".startOffsetMs is negative -- an output cannot start "
                                       "before the event that caused it");
}

void CheckGain(const char* layer, std::size_t index, float gain,
               std::vector<std::string>& errors) {
    std::ostringstream where;
    where << layer << "[" << index << "]";
    if (!std::isfinite(gain) || gain < 0.0f || gain > 4.0f)
        errors.push_back(where.str() + ".gain must be a finite value in [0, 4]");
}

void CheckCueId(const char* layer, std::size_t index, const std::string& cueId,
                std::vector<std::string>& errors) {
    std::ostringstream where;
    where << layer << "[" << index << "]";
    if (cueId.empty()) errors.push_back(where.str() + ".cueId is empty");
}

} // namespace

OutputPresetValidation ValidateOutputPreset(const OutputPreset& preset) {
    OutputPresetValidation result;
    auto& errors = result.errors;

    if (preset.presetId.empty()) errors.push_back("presetId is empty");
    if (preset.schemaVersion < kOutputPresetSchemaVersionLegacy ||
        preset.schemaVersion > kOutputPresetSchemaVersionCurrent) {
        std::ostringstream out;
        out << "schemaVersion " << preset.schemaVersion << " is not one this build understands (1 or 2)";
        errors.push_back(out.str());
    }
    if (preset.LayerCount() == 0)
        errors.push_back("preset has no output layers -- it would produce nothing");

    for (std::size_t i = 0; i < preset.speaker.size(); ++i) {
        CheckCueId("speaker", i, preset.speaker[i].cueId, errors);
        CheckGain("speaker", i, preset.speaker[i].gain, errors);
        CheckOffset("speaker", i, preset.speaker[i].startOffsetMs, errors);
    }

    for (std::size_t i = 0; i < preset.pcmHaptic.size(); ++i) {
        const auto& layer = preset.pcmHaptic[i];
        CheckCueId("pcmHaptic", i, layer.cueId, errors);
        CheckGain("pcmHaptic", i, layer.gain, errors);
        CheckOffset("pcmHaptic", i, layer.startOffsetMs, errors);
        if (!std::isfinite(layer.balance) || layer.balance < -1.0f || layer.balance > 1.0f) {
            std::ostringstream out;
            out << "pcmHaptic[" << i << "].balance must be a finite value in [-1, 1]";
            errors.push_back(out.str());
        }
    }

    for (std::size_t i = 0; i < preset.trigger.size(); ++i) {
        const auto& layer = preset.trigger[i];
        CheckOffset("trigger", i, layer.startOffsetMs, errors);
        if (!std::isfinite(layer.durationMs) || layer.durationMs < 0.0f) {
            std::ostringstream out;
            out << "trigger[" << i << "].durationMs must be a finite value >= 0 (0 holds)";
            errors.push_back(out.str());
        }
        // The mode's own rules. This is what keeps a config from asking for a
        // resistance the firmware would reject.
        const auto encoded = dualsense::ValidateTriggerEffect(layer.spec);
        if (!encoded.ok) {
            std::ostringstream out;
            out << "trigger[" << i << "]: " << encoded.error;
            errors.push_back(out.str());
        }
    }

    if (preset.legacyRumble) {
        const auto& layer = *preset.legacyRumble;
        CheckOffset("legacyRumble", 0, layer.startOffsetMs, errors);
        const auto& intensity = layer.effect.intensity;
        if (!std::isfinite(intensity.left) || intensity.left < 0.0f || intensity.left > 1.0f)
            errors.push_back("legacyRumble.left must be a finite value in [0, 1]");
        if (!std::isfinite(intensity.right) || intensity.right < 0.0f || intensity.right > 1.0f)
            errors.push_back("legacyRumble.right must be a finite value in [0, 1]");
        if (layer.effect.duration.count() <= 0)
            errors.push_back("legacyRumble.durationMs must be greater than zero");
    }

    result.ok = errors.empty();
    return result;
}

OutputPreset FromLegacyPreset(const HapticPreset& legacy) {
    OutputPreset preset;
    preset.presetId = legacy.presetId;
    preset.displayName = legacy.displayName;
    preset.schemaVersion = kOutputPresetSchemaVersionLegacy;
    LegacyRumbleOutputLayer layer;
    layer.effect = legacy.effect;
    preset.legacyRumble = layer;
    return preset;
}

} // namespace sekiro_haptics
