#include "sekiro_haptics/replay/ExpectedEventRepository.hpp"

#include "sekiro_haptics/Json.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

namespace sekiro_haptics::replay {

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

ExpectedEventLoadOutcome ExpectedEventRepository::LoadFromFile(const std::string& path) {
    ExpectedEventLoadOutcome outcome;

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

    const json::JsonValue* eventsArray = parsed.value.Find("expectedEvents");
    if (eventsArray == nullptr || !eventsArray->IsArray()) {
        outcome.fatalError = "missing \"expectedEvents\" array";
        return outcome;
    }

    outcome.ok = true;

    const std::vector<json::JsonValue>& entries = eventsArray->AsArray();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const json::JsonValue& entry = entries[i];
        if (!entry.IsObject()) {
            outcome.errors.push_back({i, "entry is not a JSON object"});
            continue;
        }

        const json::JsonValue* gameIdValue = entry.Find("gameId");
        const json::JsonValue* eventIdValue = entry.Find("eventId");
        const json::JsonValue* timestampValue = entry.Find("timestampUs");

        if (gameIdValue == nullptr || !gameIdValue->IsString() || gameIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"gameId\""});
            continue;
        }
        if (eventIdValue == nullptr || !eventIdValue->IsString() || eventIdValue->AsString().empty()) {
            outcome.errors.push_back({i, "missing or empty \"eventId\""});
            continue;
        }
        if (timestampValue == nullptr || !timestampValue->IsNumber()) {
            outcome.errors.push_back({i, "missing or non-numeric \"timestampUs\""});
            continue;
        }

        ExpectedEvent event;
        event.gameId = gameIdValue->AsString();
        event.eventId = eventIdValue->AsString();
        event.timestamp = std::chrono::microseconds(static_cast<long long>(std::llround(timestampValue->AsNumber())));

        events_.push_back(std::move(event));
        ++outcome.loadedCount;
    }

    return outcome;
}

const std::vector<ExpectedEvent>& ExpectedEventRepository::Events() const {
    return events_;
}

} // namespace sekiro_haptics::replay
