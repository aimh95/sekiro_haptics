#pragma once

#include <string>
#include <vector>

namespace sekiro_haptics::trace {

/// How ValidateTraceFile() treats a trace with no metadata sidecar at all.
/// Deliberately a required argument on every call (see ValidateTraceFile)
/// rather than a defaulted one -- whether a legacy, unversioned trace is
/// acceptable must be a caller's explicit choice, never an implicit
/// auto-detected fallback.
enum class LegacyTracePolicy {
    /// No sidecar is a hard validation failure
    /// (TraceValidationOutcome::errors gets a "MissingMetadata: ..."
    /// entry). This is what RunReplayLoopStrict uses, unconditionally --
    /// see ReplayPipeline.hpp.
    RejectLegacy,
    /// No sidecar is treated as schema version 1 (legacy), matching this
    /// project's pre-metadata behavior. Exists so a caller that genuinely
    /// needs to keep accepting old, unversioned traces can say so
    /// explicitly, instead of that being every caller's default.
    AllowLegacy,
};

/// Result of ValidateTraceFile(): a full, upfront verdict on whether a
/// trace is safe to replay, computed *before* anything downstream (an
/// event detector, mapping/preset resolution, a scheduler, a backend) ever
/// sees a single signal from it.
struct TraceValidationOutcome {
    bool ok = false;

    /// The trace's effective schema version: the sidecar's declared
    /// version if one was loaded, or kSchemaVersion (legacy) if there was
    /// no sidecar and `legacyPolicy` was AllowLegacy. 0 if validation
    /// failed before a version could be determined (no sidecar under
    /// RejectLegacy, a malformed/incomplete sidecar, or the trace file
    /// itself couldn't be opened).
    int schemaVersion = 0;

    /// Every problem found. Metadata problems (missing/unsupported
    /// sidecar) short-circuit before the signal body is read at all, so in
    /// that case this holds exactly one entry. A bad signal body may
    /// report more than one line's problem here -- every line is checked,
    /// not just the first bad one.
    std::vector<std::string> errors;
};

/// Fully validates `tracePath`: its metadata sidecar (see TraceMetadata.hpp)
/// per `legacyPolicy`, and every line of the JSONL signal body (see
/// TraceJsonl.hpp), without constructing any detector/mapping/preset/
/// scheduler/backend object and without emitting a single GameSignal to a
/// caller. This is the "validate before execute" gate --
/// RunReplayLoopStrict (ReplayPipeline.hpp) uses this, with
/// `LegacyTracePolicy::RejectLegacy`, to guarantee zero pipeline/backend
/// calls for a trace that fails here -- including a trace with no
/// metadata sidecar at all.
TraceValidationOutcome ValidateTraceFile(const std::string& tracePath, LegacyTracePolicy legacyPolicy);

} // namespace sekiro_haptics::trace
