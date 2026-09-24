#include "sekiro_haptics/presets/OutputPresetRepository.hpp"

#include "sekiro_haptics/Json.hpp"

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace sekiro_haptics {

namespace {

using dualsense::TriggerEffectMode;
using dualsense::TriggerEffectSpec;
using dualsense::TriggerSide;

bool ReadFileToString(const std::string& path, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

/// Reads an optional number. Returns false only when the key is present but
/// is not a number, so "absent" and "wrong type" never look the same.
bool OptionalNumber(const json::JsonValue& object, const std::string& key, float& out,
                    std::string& error) {
    const auto* value = object.Find(key);
    if (value == nullptr) return true;
    if (!value->IsNumber()) { error = "\"" + key + "\" must be a number"; return false; }
    out = static_cast<float>(value->AsNumber());
    return true;
}

bool RequiredInt(const json::JsonValue& object, const std::string& key, int& out,
                 std::string& error) {
    const auto* value = object.Find(key);
    if (value == nullptr) { error = "missing \"" + key + "\""; return false; }
    if (!value->IsNumber()) { error = "\"" + key + "\" must be a number"; return false; }
    const double raw = value->AsNumber();
    if (!std::isfinite(raw) || raw != std::floor(raw)) {
        error = "\"" + key + "\" must be a whole number";
        return false;
    }
    out = static_cast<int>(raw);
    return true;
}

/// Rejects any key that does not belong to the trigger mode being parsed. A
/// stray "frequency" on a feedback effect is a config mistake, and quietly
/// ignoring it would hide it.
bool RejectForeignKeys(const json::JsonValue& entry, const std::set<std::string>& allowed,
                       std::string& error) {
    for (const auto& [key, unused] : entry.AsObject()) {
        (void)unused;
        if (allowed.count(key) == 0) {
            error = "\"" + key + "\" is not a parameter of this trigger mode";
            return false;
        }
    }
    return true;
}

bool ParseTriggerLayer(const json::JsonValue& entry, TriggerOutputLayer& out, std::string& error) {
    if (!entry.IsObject()) { error = "trigger entry is not an object"; return false; }

    const auto* sideValue = entry.Find("side");
    if (sideValue == nullptr || !sideValue->IsString()) {
        error = "trigger entry needs a \"side\" of \"l2\" or \"r2\"";
        return false;
    }
    const auto side = dualsense::ParseTriggerSide(sideValue->AsString());
    if (!side) { error = "unknown trigger side \"" + sideValue->AsString() + "\""; return false; }
    out.side = *side;

    const auto* modeValue = entry.Find("mode");
    if (modeValue == nullptr || !modeValue->IsString()) {
        error = "trigger entry needs a \"mode\"";
        return false;
    }
    const auto mode = dualsense::ParseTriggerEffectMode(modeValue->AsString());
    if (!mode) { error = "unknown trigger mode \"" + modeValue->AsString() + "\""; return false; }

    // Keys every trigger entry may carry, regardless of mode.
    std::set<std::string> allowed{"side", "mode", "startOffsetMs", "durationMs"};
    int a = 0, b = 0, c = 0;
    switch (*mode) {
        case TriggerEffectMode::Off:
            if (!RejectForeignKeys(entry, allowed, error)) return false;
            out.spec = TriggerEffectSpec::MakeOff();
            break;
        case TriggerEffectMode::Feedback:
            allowed.insert({"position", "strength"});
            if (!RejectForeignKeys(entry, allowed, error)) return false;
            if (!RequiredInt(entry, "position", a, error)) return false;
            if (!RequiredInt(entry, "strength", b, error)) return false;
            if (a < 0 || a > 255 || b < 0 || b > 255) { error = "trigger parameter out of byte range"; return false; }
            out.spec = TriggerEffectSpec::MakeFeedback(static_cast<std::uint8_t>(a),
                                                       static_cast<std::uint8_t>(b));
            break;
        case TriggerEffectMode::Weapon:
            allowed.insert({"startPosition", "endPosition", "strength"});
            if (!RejectForeignKeys(entry, allowed, error)) return false;
            if (!RequiredInt(entry, "startPosition", a, error)) return false;
            if (!RequiredInt(entry, "endPosition", b, error)) return false;
            if (!RequiredInt(entry, "strength", c, error)) return false;
            if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255) { error = "trigger parameter out of byte range"; return false; }
            out.spec = TriggerEffectSpec::MakeWeapon(static_cast<std::uint8_t>(a),
                                                     static_cast<std::uint8_t>(b),
                                                     static_cast<std::uint8_t>(c));
            break;
        case TriggerEffectMode::Vibration:
            allowed.insert({"position", "amplitude", "frequencyHz"});
            if (!RejectForeignKeys(entry, allowed, error)) return false;
            if (!RequiredInt(entry, "position", a, error)) return false;
            if (!RequiredInt(entry, "amplitude", b, error)) return false;
            if (!RequiredInt(entry, "frequencyHz", c, error)) return false;
            if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255) { error = "trigger parameter out of byte range"; return false; }
            out.spec = TriggerEffectSpec::MakeVibration(static_cast<std::uint8_t>(a),
                                                        static_cast<std::uint8_t>(b),
                                                        static_cast<std::uint8_t>(c));
            break;
        case TriggerEffectMode::SimpleFeedback:
            allowed.insert({"startByte", "forceByte"});
            if (!RejectForeignKeys(entry, allowed, error)) return false;
            if (!RequiredInt(entry, "startByte", a, error)) return false;
            if (!RequiredInt(entry, "forceByte", b, error)) return false;
            if (a < 0 || a > 255 || b < 0 || b > 255) { error = "trigger parameter out of byte range"; return false; }
            out.spec = TriggerEffectSpec::MakeSimpleFeedback(static_cast<std::uint8_t>(a),
                                                             static_cast<std::uint8_t>(b));
            break;
    }

    if (!OptionalNumber(entry, "startOffsetMs", out.startOffsetMs, error)) return false;
    if (!OptionalNumber(entry, "durationMs", out.durationMs, error)) return false;
    return true;
}

bool ParseSpeakerLayer(const json::JsonValue& entry, SpeakerOutputLayer& out, std::string& error) {
    if (!entry.IsObject()) { error = "speaker entry is not an object"; return false; }
    const auto* cue = entry.Find("cueId");
    if (cue == nullptr || !cue->IsString()) { error = "speaker entry needs a \"cueId\""; return false; }
    out.cueId = cue->AsString();
    if (!OptionalNumber(entry, "gain", out.gain, error)) return false;
    if (!OptionalNumber(entry, "startOffsetMs", out.startOffsetMs, error)) return false;
    return true;
}

bool ParsePcmHapticLayer(const json::JsonValue& entry, PcmHapticOutputLayer& out, std::string& error) {
    if (!entry.IsObject()) { error = "pcmHaptic entry is not an object"; return false; }
    const auto* cue = entry.Find("cueId");
    if (cue == nullptr || !cue->IsString()) { error = "pcmHaptic entry needs a \"cueId\""; return false; }
    out.cueId = cue->AsString();
    if (!OptionalNumber(entry, "gain", out.gain, error)) return false;
    if (!OptionalNumber(entry, "balance", out.balance, error)) return false;
    if (!OptionalNumber(entry, "startOffsetMs", out.startOffsetMs, error)) return false;
    return true;
}

bool ParseLegacyRumble(const json::JsonValue& entry, LegacyRumbleOutputLayer& out,
                       std::string& error) {
    if (!entry.IsObject()) { error = "\"legacyRumble\" is not an object"; return false; }
    float left = 0.0f, right = 0.0f;
    if (!OptionalNumber(entry, "left", left, error)) return false;
    if (!OptionalNumber(entry, "right", right, error)) return false;
    const auto* duration = entry.Find("durationMs");
    if (duration == nullptr || !duration->IsNumber()) {
        error = "\"legacyRumble\" needs a numeric \"durationMs\"";
        return false;
    }
    out.effect.intensity.left = left;
    out.effect.intensity.right = right;
    out.effect.duration = std::chrono::milliseconds(
        static_cast<long long>(std::llround(duration->AsNumber())));
    if (!OptionalNumber(entry, "startOffsetMs", out.startOffsetMs, error)) return false;
    return true;
}

} // namespace

OutputPresetLoadOutcome OutputPresetRepository::LoadFromFile(const std::string& path) {
    std::string content;
    if (!ReadFileToString(path, content)) {
        OutputPresetLoadOutcome outcome;
        outcome.fatalError = "could not open file: " + path;
        return outcome;
    }
    return LoadFromString(content);
}

OutputPresetLoadOutcome OutputPresetRepository::LoadFromString(const std::string& text) {
    OutputPresetLoadOutcome outcome;

    const auto parsed = json::ParseJson(text);
    if (!parsed.ok) { outcome.fatalError = "JSON parse error: " + parsed.error; return outcome; }
    if (!parsed.value.IsObject()) {
        outcome.fatalError = "top-level JSON value must be an object";
        return outcome;
    }

    // No schemaVersion means version 1 -- which is what every file written
    // before this type existed is.
    outcome.schemaVersion = kOutputPresetSchemaVersionLegacy;
    if (const auto* version = parsed.value.Find("schemaVersion")) {
        if (!version->IsNumber()) {
            outcome.fatalError = "\"schemaVersion\" must be a number";
            return outcome;
        }
        outcome.schemaVersion = static_cast<int>(version->AsNumber());
        if (outcome.schemaVersion > kOutputPresetSchemaVersionCurrent) {
            std::ostringstream out;
            out << "file schemaVersion " << outcome.schemaVersion
                << " is newer than this build understands (" << kOutputPresetSchemaVersionCurrent
                << "); refusing rather than guessing at fields it does not know";
            outcome.fatalError = out.str();
            return outcome;
        }
    }

    const auto* presets = parsed.value.Find("presets");
    if (presets == nullptr || !presets->IsArray()) {
        outcome.fatalError = "missing \"presets\" array";
        return outcome;
    }

    outcome.ok = true;
    const auto& entries = presets->AsArray();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!entry.IsObject()) {
            outcome.errors.push_back({i, {}, "entry is not a JSON object"});
            continue;
        }

        OutputPreset preset;
        preset.schemaVersion = outcome.schemaVersion;

        const auto* idValue = entry.Find("presetId");
        if (idValue == nullptr || !idValue->IsString() || idValue->AsString().empty()) {
            outcome.errors.push_back({i, {}, "missing or empty \"presetId\""});
            continue;
        }
        preset.presetId = idValue->AsString();

        const auto* nameValue = entry.Find("displayName");
        if (nameValue == nullptr || !nameValue->IsString()) {
            outcome.errors.push_back({i, preset.presetId, "missing \"displayName\""});
            continue;
        }
        preset.displayName = nameValue->AsString();

        std::string error;
        bool rejected = false;

        auto readArray = [&](const char* key, auto parseOne, auto& destination) {
            if (rejected) return;
            const auto* array = entry.Find(key);
            if (array == nullptr) return;
            if (!array->IsArray()) {
                outcome.errors.push_back({i, preset.presetId,
                                          std::string("\"") + key + "\" must be an array"});
                rejected = true;
                return;
            }
            for (const auto& element : array->AsArray()) {
                typename std::decay_t<decltype(destination)>::value_type layer;
                if (!parseOne(element, layer, error)) {
                    outcome.errors.push_back({i, preset.presetId,
                                              std::string(key) + ": " + error});
                    rejected = true;
                    return;
                }
                destination.push_back(layer);
            }
        };

        readArray("speaker", ParseSpeakerLayer, preset.speaker);
        readArray("pcmHaptic", ParsePcmHapticLayer, preset.pcmHaptic);
        readArray("trigger", ParseTriggerLayer, preset.trigger);

        if (!rejected) {
            if (const auto* rumble = entry.Find("legacyRumble")) {
                LegacyRumbleOutputLayer layer;
                if (!ParseLegacyRumble(*rumble, layer, error)) {
                    outcome.errors.push_back({i, preset.presetId, error});
                    rejected = true;
                } else {
                    preset.legacyRumble = layer;
                }
            }
        }
        if (rejected) continue;

        // Final gate. Nothing that fails here reaches the repository, so no
        // caller can build output from a preset the ranges reject.
        const auto validation = ValidateOutputPreset(preset);
        if (!validation.ok) {
            for (const auto& message : validation.errors)
                outcome.errors.push_back({i, preset.presetId, message});
            continue;
        }

        presets_[preset.presetId] = std::move(preset);
        ++outcome.loadedCount;
    }

    return outcome;
}

void OutputPresetRepository::AddPreset(OutputPreset preset) {
    presets_[preset.presetId] = std::move(preset);
}

const OutputPreset* OutputPresetRepository::Find(const std::string& presetId) const {
    const auto found = presets_.find(presetId);
    return found == presets_.end() ? nullptr : &found->second;
}

std::size_t OutputPresetRepository::Size() const { return presets_.size(); }

std::vector<std::string> OutputPresetRepository::PresetIds() const {
    std::vector<std::string> ids;
    ids.reserve(presets_.size());
    for (const auto& [id, unused] : presets_) { (void)unused; ids.push_back(id); }
    return ids;
}

} // namespace sekiro_haptics
