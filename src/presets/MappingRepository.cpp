#include "sekiro_haptics/presets/MappingRepository.hpp"

#include "sekiro_haptics/Json.hpp"

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

MappingLoadOutcome MappingRepository::LoadFromFile(const std::string& path) {
    MappingLoadOutcome outcome;

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

    const json::JsonValue* mappingsArray = parsed.value.Find("mappings");
    if (mappingsArray == nullptr || !mappingsArray->IsArray()) {
        outcome.fatalError = "missing \"mappings\" array";
        return outcome;
    }

    outcome.ok = true;

    const std::vector<json::JsonValue>& entries = mappingsArray->AsArray();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const json::JsonValue& entry = entries[i];
        if (!entry.IsObject()) {
            outcome.errors.push_back({i, "entry is not a JSON object"});
            continue;
        }

        const json::JsonValue* gameIdValue = entry.Find("gameId");
        const json::JsonValue* eventIdValue = entry.Find("eventId");
        const json::JsonValue* presetIdValue = entry.Find("presetId");

        if (gameIdValue == nullptr || !gameIdValue->IsString() || gameIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"gameId\""});
            continue;
        }
        if (eventIdValue == nullptr || !eventIdValue->IsString() || eventIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"eventId\""});
            continue;
        }
        if (presetIdValue == nullptr || !presetIdValue->IsString() || presetIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"presetId\""});
            continue;
        }

        EventMapping mapping;
        mapping.gameId = gameIdValue->AsString();
        mapping.eventId = eventIdValue->AsString();
        mapping.presetId = presetIdValue->AsString();

        AddMapping(std::move(mapping));
        ++outcome.loadedCount;
    }

    return outcome;
}

void MappingRepository::AddMapping(EventMapping mapping) {
    auto key = std::make_pair(mapping.gameId, mapping.eventId);
    mappings_[key] = std::move(mapping);
}

const EventMapping* MappingRepository::Find(const std::string& gameId, const std::string& eventId) const {
    auto it = mappings_.find(std::make_pair(gameId, eventId));
    return it != mappings_.end() ? &it->second : nullptr;
}

std::size_t MappingRepository::Size() const {
    return mappings_.size();
}

} // namespace sekiro_haptics
