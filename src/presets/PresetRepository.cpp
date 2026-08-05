#include "sekiro_haptics/presets/PresetRepository.hpp"

#include "sekiro_haptics/Json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace sekiro_haptics {

namespace {

bool ReadFileToString(const std::string& path, std::string& outContent) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    outContent = buffer.str();
    return true;
}

} // namespace

PresetLoadOutcome PresetRepository::LoadFromFile(const std::string& path) {
    PresetLoadOutcome outcome;

    std::string content;
    if (!ReadFileToString(path, content)) {
        outcome.fatalError = "could not open file: " + path;
        return outcome;
    }

    json::JsonParseResult parsed = json::ParseJson(content);
    if (!parsed.ok) {
        outcome.fatalError = "JSON parse error: " + parsed.error;
        return outcome;
    }
    if (!parsed.value.IsObject()) {
        outcome.fatalError = "top-level JSON value must be an object";
        return outcome;
    }

    const json::JsonValue* presetsArray = parsed.value.Find("presets");
    if (presetsArray == nullptr || !presetsArray->IsArray()) {
        outcome.fatalError = "missing \"presets\" array";
        return outcome;
    }

    outcome.ok = true;

    const std::vector<json::JsonValue>& entries = presetsArray->AsArray();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const json::JsonValue& entry = entries[i];
        if (!entry.IsObject()) {
            outcome.errors.push_back({i, "entry is not a JSON object"});
            continue;
        }

        const json::JsonValue* presetIdValue = entry.Find("presetId");
        if (presetIdValue == nullptr || !presetIdValue->IsString() || presetIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"presetId\""});
            continue;
        }
        const std::string presetId = presetIdValue->AsString();

        const json::JsonValue* displayNameValue = entry.Find("displayName");
        if (displayNameValue == nullptr || !displayNameValue->IsString()) {
            outcome.errors.push_back({i, "missing \"displayName\""});
            continue;
        }

        const json::JsonValue* rumbleValue = entry.Find("legacyRumble");
        if (rumbleValue == nullptr || !rumbleValue->IsObject()) {
            outcome.errors.push_back({i, "missing \"legacyRumble\" object"});
            continue;
        }

        const json::JsonValue* durationValue = rumbleValue->Find("durationMs");
        if (durationValue == nullptr || !durationValue->IsNumber()) {
            outcome.errors.push_back({i, "missing or non-numeric \"legacyRumble.durationMs\""});
            continue;
        }
        double durationMs = durationValue->AsNumber();
        if (durationMs <= 0.0) {
            outcome.errors.push_back({i, "\"legacyRumble.durationMs\" must be > 0"});
            continue;
        }

        double left = rumbleValue->GetNumber("left", 0.0);
        double right = rumbleValue->GetNumber("right", 0.0);

        double clampedLeft = std::clamp(left, 0.0, 1.0);
        double clampedRight = std::clamp(right, 0.0, 1.0);
        if (clampedLeft != left || clampedRight != right) {
            outcome.errors.push_back({i, "left/right intensity outside [0,1], clamped"});
        }

        HapticPreset preset;
        preset.presetId = presetId;
        preset.displayName = displayNameValue->AsString();
        preset.effect.type = HapticEffectType::Generic;
        preset.effect.intensity.left = static_cast<float>(clampedLeft);
        preset.effect.intensity.right = static_cast<float>(clampedRight);
        preset.effect.duration = std::chrono::milliseconds(static_cast<long long>(durationMs));
        preset.effect.debugLabel = presetId;

        AddPreset(std::move(preset));
        ++outcome.loadedCount;
    }

    return outcome;
}

void PresetRepository::AddPreset(HapticPreset preset) {
    presets_[preset.presetId] = std::move(preset);
}

const HapticPreset* PresetRepository::Find(const std::string& presetId) const {
    auto it = presets_.find(presetId);
    return it != presets_.end() ? &it->second : nullptr;
}

std::size_t PresetRepository::Size() const {
    return presets_.size();
}

} // namespace sekiro_haptics
