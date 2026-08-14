#include "sekiro_haptics/replay/TraceMetadata.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;
using namespace sekiro_haptics::trace;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

SH_TEST(LoadTraceSourceMetadata_NoSidecarFile_ReturnsNoSidecarFile) {
    // perfect_deflect.jsonl predates this metadata format and has no
    // "perfect_deflect.jsonl.meta.json" sidecar -- this must keep working,
    // treated as legacy/unversioned, not an error.
    TraceSourceMetadata metadata;
    TraceMetadataLoadResult result = LoadTraceSourceMetadata(Fixture("perfect_deflect.jsonl"), metadata);

    SH_CHECK(result == TraceMetadataLoadResult::NoSidecarFile);
}

SH_TEST(LoadTraceSourceMetadata_ValidSidecar_LoadsAllFields) {
    TraceSourceMetadata metadata;
    TraceMetadataLoadResult result = LoadTraceSourceMetadata(Fixture("valid_metadata.jsonl"), metadata);

    SH_CHECK(result == TraceMetadataLoadResult::Success);
    SH_CHECK(metadata.schemaVersion == 1);
    SH_CHECK(metadata.sourceType == "replay");
    SH_CHECK(metadata.generatorVersion == "1.0.0");
}

SH_TEST(LoadTraceSourceMetadata_UnsupportedSchemaVersion_IsRejectedNotGuessed) {
    TraceSourceMetadata metadata;
    TraceMetadataLoadResult result = LoadTraceSourceMetadata(Fixture("unsupported_schema.jsonl"), metadata);

    SH_CHECK(result == TraceMetadataLoadResult::UnsupportedSchemaVersion);
    // The declared-but-rejected version is still reported, for diagnostics.
    SH_CHECK(metadata.schemaVersion == 99);
}

SH_TEST(LoadTraceSourceMetadata_MissingSourceTypeAndGeneratorVersion_ReturnsMissingSourceType) {
    TraceSourceMetadata metadata;
    TraceMetadataLoadResult result = LoadTraceSourceMetadata(Fixture("missing_metadata.jsonl"), metadata);

    SH_CHECK(result == TraceMetadataLoadResult::MissingSourceType);
}

SH_TEST(LoadTraceSourceMetadata_MissingGeneratorVersionOnly_ReturnsMissingGeneratorVersion) {
    TraceSourceMetadata metadata;
    TraceMetadataLoadResult result = LoadTraceSourceMetadata(Fixture("missing_generator_version.jsonl"), metadata);

    SH_CHECK(result == TraceMetadataLoadResult::MissingGeneratorVersion);
}
