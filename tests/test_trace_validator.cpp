#include "sekiro_haptics/replay/TraceJsonl.hpp"
#include "sekiro_haptics/replay/TraceValidator.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;
using namespace sekiro_haptics::trace;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

// --- LegacyTracePolicy::AllowLegacy: the explicit "I accept unversioned
// traces" opt-in. Fixtures below with no sidecar use this policy so their
// assertions stay focused on what they're actually testing (body-content
// validation), independent of the metadata policy question -- see the
// RejectLegacy tests further down for that question specifically.

SH_TEST(ValidateTraceFile_AllowLegacy_NoSidecar_IsValid) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("perfect_deflect.jsonl"), LegacyTracePolicy::AllowLegacy);

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.schemaVersion == kSchemaVersion);
    SH_CHECK(outcome.errors.empty());
}

SH_TEST(ValidateTraceFile_AllowLegacy_ValidSignalValidityValue_IsValid) {
    TraceValidationOutcome outcome =
        ValidateTraceFile(Fixture("signal_validity_unavailable.jsonl"), LegacyTracePolicy::AllowLegacy);

    SH_CHECK(outcome.ok);
}

// --- LegacyTracePolicy::RejectLegacy: the strict policy RunReplayLoopStrict
// always uses. Fixtures here that previously had no sidecar now carry a
// valid one (see tests/fixtures/*.meta.json) specifically so these tests
// keep exercising their named body-content failure, not the (now separate)
// missing-metadata failure covered by the dedicated tests below.

SH_TEST(ValidateTraceFile_RejectLegacy_ValidMetadataAndBody_IsValid) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("valid_metadata.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.schemaVersion == 1);
}

SH_TEST(ValidateTraceFile_RejectLegacy_MalformedJsonLine_FailsClosed) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("malformed_json.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
    SH_CHECK(!outcome.errors.empty());
    SH_CHECK(outcome.errors[0].find("MissingMetadata") == std::string::npos);
}

SH_TEST(ValidateTraceFile_RejectLegacy_OutOfOrderTimestamp_FailsClosed) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("out_of_order.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
    SH_CHECK(outcome.errors[0].find("MissingMetadata") == std::string::npos);
}

SH_TEST(ValidateTraceFile_RejectLegacy_UnsupportedSchemaVersion_FailsClosedBeforeReadingBody) {
    TraceValidationOutcome outcome =
        ValidateTraceFile(Fixture("unsupported_schema.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
    SH_CHECK(outcome.schemaVersion == 99);
    SH_CHECK(outcome.errors.size() == 1);
}

SH_TEST(ValidateTraceFile_RejectLegacy_MissingRequiredMetadataField_FailsClosed) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("missing_metadata.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
}

SH_TEST(ValidateTraceFile_RejectLegacy_InvalidSignalValidity_FailsClosed) {
    TraceValidationOutcome outcome =
        ValidateTraceFile(Fixture("invalid_signal_validity.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
    SH_CHECK(outcome.errors[0].find("MissingMetadata") == std::string::npos);
}

// --- The specific policy fix this ticket makes: a trace with NO sidecar at
// all must be a hard, explicitly-labeled failure under RejectLegacy, and
// must remain valid under the explicit AllowLegacy opt-in.

SH_TEST(ValidateTraceFile_RejectLegacy_NoSidecarAtAll_FailsWithMissingMetadataError) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("no_sidecar_trace.jsonl"), LegacyTracePolicy::RejectLegacy);

    SH_CHECK(outcome.ok == false);
    SH_CHECK(outcome.schemaVersion == 0);
    SH_CHECK(outcome.errors.size() == 1);
    SH_CHECK(outcome.errors[0].find("MissingMetadata") != std::string::npos);
}

SH_TEST(ValidateTraceFile_AllowLegacy_NoSidecarAtAll_IsValid) {
    TraceValidationOutcome outcome = ValidateTraceFile(Fixture("no_sidecar_trace.jsonl"), LegacyTracePolicy::AllowLegacy);

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.schemaVersion == kSchemaVersion);
}
