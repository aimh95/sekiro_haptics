// Unit tests for SEK-PROBE-001D Stage C's AnalyzeCombatCaptureFile(). Pure
// file-based -- no live process, no SekiroCombatCaptureSession dependency
// (the fixture files below are hand-written JSONL, exercising the analyzer
// in isolation per the "sampler와 분석기를 분리" requirement).

#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"
#include "testing.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path WriteFixture(const std::string& name, const std::string& content) {
    auto path = std::filesystem::temp_directory_path() / ("sh_combat_analyzer_" + name + ".jsonl");
    std::ofstream out(path, std::ios::trunc);
    out << content;
    out.close();
    return path;
}

bool AnyStatMatches(const CombatCaptureAnalysisReport& report, std::size_t offset, const std::string& label,
                    std::uint64_t changeCount) {
    for (const CombatCaptureOffsetMarkerStat& stat : report.offsetMarkerStats) {
        if (stat.offset == offset && stat.label == label && stat.changeCount == changeCount) {
            return true;
        }
    }
    return false;
}

} // namespace

SH_TEST(AnalyzeCombatCaptureFile_MissingFile_ReturnsError) {
    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile("C:\\does\\not\\exist.jsonl", 100000);
    SH_CHECK(!report.ok);
    SH_CHECK(!report.error.empty());
}

SH_TEST(AnalyzeCombatCaptureFile_MalformedLine_ReturnsError) {
    auto path = WriteFixture("malformed", "not json at all\n");
    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100000);
    SH_CHECK(!report.ok);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_CountsRecordKinds) {
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":24,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1050,\"recordKind\":\"marker\",\"label\":\"take_damage\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":2000,\"recordKind\":\"discontinuity\",\"oldGeneration\":1,"
        "\"newGeneration\":2}\n"
        "{\"schemaVersion\":1,\"timestampUs\":3000,\"recordKind\":\"dropped\",\"reason\":\"read failed\"}\n";
    auto path = WriteFixture("counts", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100000);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalDeltaRecords == 1);
    SH_CHECK(report.totalMarkers == 1);
    SH_CHECK(report.totalDiscontinuities == 1);
    SH_CHECK(report.totalDropped == 1);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_DeltaWithinWindowOfMarker_IsCorrelated) {
    // Delta at t=1000, marker "perfect_deflect" at t=1050 -- 50us apart,
    // well within a 100us window.
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":52,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1050,\"recordKind\":\"marker\",\"label\":\"perfect_deflect\"}\n";
    auto path = WriteFixture("correlated", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(AnyStatMatches(report, 52, "perfect_deflect", 1));
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_DeltaOutsideWindow_IsNotCorrelated) {
    // Delta at t=1000, marker at t=1000000 (1 second later) -- far outside
    // any reasonable window.
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":52,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1000000,\"recordKind\":\"marker\",\"label\":\"perfect_deflect\"}\n";
    auto path = WriteFixture("uncorrelated", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(report.offsetMarkerStats.empty());
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_OffsetChangesNearMultipleMarkerLabels_TracksBothSeparately) {
    // Offset 52 changes near both a "normal_block" marker and a
    // "take_damage" marker in two separate trials -- a real-world "noise"
    // offset that isn't specific to either label. The analyzer must report
    // both correlations, not collapse them.
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":52,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1010,\"recordKind\":\"marker\",\"label\":\"normal_block\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":5000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":52,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"01000000\",\"currentBytesHex\":\"02000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":5010,\"recordKind\":\"marker\",\"label\":\"take_damage\"}\n";
    auto path = WriteFixture("multilabel", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(AnyStatMatches(report, 52, "normal_block", 1));
    SH_CHECK(AnyStatMatches(report, 52, "take_damage", 1));
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_MarkerOccurrencesReflectsAllOccurrencesOfLabel) {
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":8,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1010,\"recordKind\":\"marker\",\"label\":\"guard_only\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":9000,\"recordKind\":\"marker\",\"label\":\"guard_only\"}\n";
    auto path = WriteFixture("occurrences", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalMarkers == 2);
    bool found = false;
    for (const CombatCaptureOffsetMarkerStat& stat : report.offsetMarkerStats) {
        if (stat.offset == 8 && stat.label == "guard_only") {
            SH_CHECK(stat.changeCount == 1);   // only correlated with the first occurrence
            SH_CHECK(stat.markerOccurrences == 2); // but "guard_only" appeared twice total
            found = true;
        }
    }
    SH_CHECK(found);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_SortedByDescendingChangeCount) {
    std::string content =
        "{\"schemaVersion\":1,\"timestampUs\":1000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":8,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":1010,\"recordKind\":\"marker\",\"label\":\"rest\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":2000,\"recordKind\":\"delta\",\"generation\":1,\"offset\":12,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"00000000\",\"currentBytesHex\":\"01000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":2005,\"recordKind\":\"delta\",\"generation\":1,\"offset\":12,"
        "\"cellSizeBytes\":4,\"previousBytesHex\":\"01000000\",\"currentBytesHex\":\"02000000\"}\n"
        "{\"schemaVersion\":1,\"timestampUs\":2010,\"recordKind\":\"marker\",\"label\":\"idle\"}\n";
    auto path = WriteFixture("sorted", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(report.offsetMarkerStats.size() >= 2);
    for (std::size_t i = 1; i < report.offsetMarkerStats.size(); ++i) {
        SH_CHECK(report.offsetMarkerStats[i - 1].changeCount >= report.offsetMarkerStats[i].changeCount);
    }
    std::filesystem::remove(path);
}
