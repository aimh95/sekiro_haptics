#include "sekiro_haptics/replay/TraceValidator.hpp"

#include "sekiro_haptics/replay/TraceJsonl.hpp"
#include "sekiro_haptics/replay/TraceMetadata.hpp"

namespace sekiro_haptics::trace {

TraceValidationOutcome ValidateTraceFile(const std::string& tracePath, LegacyTracePolicy legacyPolicy) {
    TraceValidationOutcome outcome;

    TraceSourceMetadata metadata;
    TraceMetadataLoadResult metadataResult = LoadTraceSourceMetadata(tracePath, metadata);
    switch (metadataResult) {
        case TraceMetadataLoadResult::NoSidecarFile:
            if (legacyPolicy == LegacyTracePolicy::RejectLegacy) {
                outcome.errors.push_back("MissingMetadata: no metadata sidecar found (expected \"" + tracePath +
                                          ".meta.json\"); strict validation requires trace metadata");
                return outcome;
            }
            outcome.schemaVersion = kSchemaVersion;
            break;
        case TraceMetadataLoadResult::Success:
            outcome.schemaVersion = metadata.schemaVersion;
            break;
        case TraceMetadataLoadResult::MalformedJson:
            outcome.errors.push_back("metadata sidecar: malformed JSON");
            return outcome;
        case TraceMetadataLoadResult::MissingSourceType:
            outcome.errors.push_back("metadata sidecar: missing \"sourceType\"");
            return outcome;
        case TraceMetadataLoadResult::MissingGeneratorVersion:
            outcome.errors.push_back("metadata sidecar: missing \"generatorVersion\"");
            return outcome;
        case TraceMetadataLoadResult::UnsupportedSchemaVersion:
            outcome.schemaVersion = metadata.schemaVersion;
            outcome.errors.push_back("metadata sidecar: unsupported schemaVersion " +
                                      std::to_string(metadata.schemaVersion));
            return outcome;
    }

    // Metadata (if any) checked out -- now validate every line of the
    // signal body itself, via a fresh reader so this never shares state
    // with, or advances, any reader a caller might use afterward.
    TraceReader reader(tracePath);
    if (!reader.IsOpen()) {
        outcome.schemaVersion = 0;
        outcome.errors.push_back("could not open trace file: " + tracePath);
        return outcome;
    }

    GameSignal signal;
    while (true) {
        std::string error;
        TraceReadResult result = reader.ReadNext(signal, &error);
        if (result == TraceReadResult::EndOfTrace) {
            break;
        }
        if (result != TraceReadResult::Success) {
            outcome.errors.push_back(error);
        }
    }

    outcome.ok = outcome.errors.empty();
    return outcome;
}

} // namespace sekiro_haptics::trace
