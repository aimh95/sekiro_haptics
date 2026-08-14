#include "sekiro_haptics/replay/TraceMetadata.hpp"

#include "sekiro_haptics/Json.hpp"

#include <array>
#include <fstream>
#include <sstream>

namespace sekiro_haptics::trace {

namespace {

// Every schemaVersion this reader is willing to accept. Bump alongside
// kSchemaVersion (TraceJsonl.hpp) if the schema ever changes in a way that
// needs a new version -- do not widen this just to make an unrecognized
// trace "work."
constexpr std::array<int, 1> kSupportedSchemaVersions = {1};

bool IsSupportedSchemaVersion(int version) {
    for (int supported : kSupportedSchemaVersions) {
        if (supported == version) {
            return true;
        }
    }
    return false;
}

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

const char* ToString(TraceMetadataLoadResult result) {
    switch (result) {
        case TraceMetadataLoadResult::NoSidecarFile:
            return "NoSidecarFile";
        case TraceMetadataLoadResult::Success:
            return "Success";
        case TraceMetadataLoadResult::MalformedJson:
            return "MalformedJson";
        case TraceMetadataLoadResult::MissingSourceType:
            return "MissingSourceType";
        case TraceMetadataLoadResult::MissingGeneratorVersion:
            return "MissingGeneratorVersion";
        case TraceMetadataLoadResult::UnsupportedSchemaVersion:
            return "UnsupportedSchemaVersion";
    }
    return "Unknown";
}

TraceMetadataLoadResult LoadTraceSourceMetadata(const std::string& tracePath, TraceSourceMetadata& outMetadata) {
    const std::string sidecarPath = tracePath + ".meta.json";

    std::string content;
    if (!ReadFileToString(sidecarPath, content)) {
        return TraceMetadataLoadResult::NoSidecarFile;
    }

    json::JsonParseResult parsed = json::ParseJson(content);
    if (!parsed.ok || !parsed.value.IsObject()) {
        return TraceMetadataLoadResult::MalformedJson;
    }

    const json::JsonValue* schemaVersionValue = parsed.value.Find("schemaVersion");
    if (schemaVersionValue == nullptr || !schemaVersionValue->IsNumber()) {
        return TraceMetadataLoadResult::MalformedJson;
    }
    int schemaVersion = static_cast<int>(schemaVersionValue->AsNumber());
    if (!IsSupportedSchemaVersion(schemaVersion)) {
        // Report the declared-but-unsupported version back to the caller
        // (e.g. for an error message) even though loading is rejected.
        outMetadata.schemaVersion = schemaVersion;
        return TraceMetadataLoadResult::UnsupportedSchemaVersion;
    }

    const json::JsonValue* sourceTypeValue = parsed.value.Find("sourceType");
    if (sourceTypeValue == nullptr || !sourceTypeValue->IsString() || sourceTypeValue->AsString().empty()) {
        return TraceMetadataLoadResult::MissingSourceType;
    }

    const json::JsonValue* generatorVersionValue = parsed.value.Find("generatorVersion");
    if (generatorVersionValue == nullptr || !generatorVersionValue->IsString() ||
        generatorVersionValue->AsString().empty()) {
        return TraceMetadataLoadResult::MissingGeneratorVersion;
    }

    TraceSourceMetadata metadata;
    metadata.schemaVersion = schemaVersion;
    metadata.sourceType = sourceTypeValue->AsString();
    metadata.generatorVersion = generatorVersionValue->AsString();
    if (const json::JsonValue* identity = parsed.value.Find("executableIdentity")) {
        if (identity->IsString()) {
            metadata.executableIdentity = identity->AsString();
        }
    }

    outMetadata = std::move(metadata);
    return TraceMetadataLoadResult::Success;
}

} // namespace sekiro_haptics::trace
