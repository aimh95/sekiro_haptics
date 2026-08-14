#pragma once

#include "sekiro_haptics/replay/TraceJsonl.hpp"

#include <optional>
#include <string>

namespace sekiro_haptics::trace {

/// Trace-level provenance metadata: where a trace came from and what
/// schema version it was written against. Loaded from a trace's companion
/// sidecar file ("<tracePath>.meta.json"), separate from the JSONL signal
/// data itself so existing/legacy traces need no changes to their line
/// format -- see docs/03-trace-format.md.
struct TraceSourceMetadata {
    int schemaVersion = kSchemaVersion;
    std::string sourceType;       // "capture" or "replay"
    std::string generatorVersion; // free-form version string of whatever tool wrote this trace

    /// Reserved slot for a future executable identity (hash, file version,
    /// ...). Never populated with real data in this repository -- see
    /// docs/03-trace-format.md.
    std::optional<std::string> executableIdentity;
};

/// Outcome of LoadTraceSourceMetadata().
enum class TraceMetadataLoadResult {
    /// No "<tracePath>.meta.json" sidecar exists. Not an error: the trace
    /// is treated as legacy/unversioned, equivalent to schemaVersion == 1,
    /// for backward compatibility with every fixture that predates this
    /// metadata format.
    NoSidecarFile,
    Success,
    MalformedJson,
    MissingSourceType,
    MissingGeneratorVersion,
    /// The sidecar explicitly declares a schemaVersion this reader does not
    /// recognize. This is the case this type exists to catch: an
    /// unsupported version must never be silently read as if it were
    /// current.
    UnsupportedSchemaVersion,
};

/// Returns a human-readable name for a TraceMetadataLoadResult, e.g. for logging.
const char* ToString(TraceMetadataLoadResult result);

/// Looks for "<tracePath>.meta.json" and, if present, loads and validates
/// it. See docs/03-trace-format.md for the schema and the exact set of
/// schemaVersion values this reader accepts.
TraceMetadataLoadResult LoadTraceSourceMetadata(const std::string& tracePath, TraceSourceMetadata& outMetadata);

} // namespace sekiro_haptics::trace
