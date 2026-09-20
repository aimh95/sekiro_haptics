// Unit tests for SEK-PROBE-001D Stage C's AnalyzeCombatCaptureFile(). Pure
// file-based -- no live process, no SekiroCombatCaptureSession dependency
// (the fixture files below are hand-written JSONL, exercising the analyzer
// in isolation per the "sampler와 분석기를 분리" requirement).

#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"
#include "testing.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
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

/// A "delta" JSONL line: previousU32/currentU32 are written little-endian
/// (byte0 first), matching SekiroCombatCaptureSession's own ToHexBytes().
std::string DeltaLine(std::int64_t timestampUs, std::size_t offset, std::uint32_t previousU32,
                       std::uint32_t currentU32) {
    auto toHex = [](std::uint32_t v) {
        std::uint8_t bytes[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                                  static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (std::uint8_t b : bytes) oss << std::setw(2) << static_cast<unsigned>(b);
        return oss.str();
    };
    std::ostringstream oss;
    oss << "{\"schemaVersion\":1,\"timestampUs\":" << timestampUs << ",\"recordKind\":\"delta\",\"generation\":1,"
        << "\"offset\":" << offset << ",\"cellSizeBytes\":4,\"previousBytesHex\":\"" << toHex(previousU32)
        << "\",\"currentBytesHex\":\"" << toHex(currentU32) << "\"}\n";
    return oss.str();
}

std::string MarkerLine(std::int64_t timestampUs, const std::string& label) {
    std::ostringstream oss;
    oss << "{\"schemaVersion\":1,\"timestampUs\":" << timestampUs << ",\"recordKind\":\"marker\",\"label\":\""
        << label << "\"}\n";
    return oss.str();
}

const CombatCaptureOffsetTrialStat* FindStat(const CombatCaptureAnalysisReport& report, std::size_t offset,
                                              const std::string& label) {
    for (const CombatCaptureOffsetTrialStat& stat : report.offsetStats) {
        if (stat.offset == offset && stat.label == label) {
            return &stat;
        }
    }
    return nullptr;
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

SH_TEST(AnalyzeCombatCaptureFile_OverlappingWindowsCannotCreditOneDeltaTwice) {
    auto path = WriteFixture("overlap-v3-fix", MarkerLine(1000, "guard_input") +
        MarkerLine(1001, "perfect_deflect") + MarkerLine(1100, "guard_input") +
        MarkerLine(1101, "normal_block") + DeltaLine(1150, 24, 0, 1));
    const auto report = AnalyzeCombatCaptureFile(path.string(), 200);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalDeltaRecords == 1);
    SH_CHECK(report.overlappingTrialsExcluded == 2);
    SH_CHECK(report.totalTrials == 0);
    SH_CHECK(report.offsetStats.empty());
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_GapWithinTrialExcludesNumeratorAndDenominator) {
    auto path = WriteFixture("gap-v3-fix", MarkerLine(1000, "guard_input") +
        MarkerLine(1001, "perfect_deflect") +
        "{\"timestampUs\":1050,\"recordKind\":\"dropped\"}\n" + DeltaLine(1100, 24, 0, 1));
    const auto report = AnalyzeCombatCaptureFile(path.string(), 200);
    SH_CHECK(report.ok);
    SH_CHECK(report.discontinuousTrialsExcluded == 1);
    SH_CHECK(report.totalTrials == 0);
    SH_CHECK(report.offsetStats.empty());
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_NonHexBytesAreRejected) {
    auto text = DeltaLine(1000, 24, 0, 1);
    const auto position = text.find("00000000");
    SH_CHECK(position != std::string::npos);
    text.replace(position, 8, "zzzzzzzz");
    auto path = WriteFixture("nonhex-v3-fix", text);
    SH_CHECK(!AnalyzeCombatCaptureFile(path.string(), 200).ok);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_V3IsNotSilentlyScoredAsDeltaOnlyV2) {
    auto path = WriteFixture("schema-v3-fix", "{\"schemaVersion\":3,\"recordKind\":\"capture_start\",\"timestampUs\":0}\n");
    const auto report = AnalyzeCombatCaptureFile(path.string(), 200);
    SH_CHECK(!report.ok);
    SH_CHECK(report.error.find("deflect_capture.py") != std::string::npos);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_CountsRecordKinds) {
    std::string content = DeltaLine(1000, 24, 0, 1) + MarkerLine(1050, "take_damage") +
                          "{\"schemaVersion\":1,\"timestampUs\":2000,\"recordKind\":\"discontinuity\","
                          "\"oldGeneration\":1,\"newGeneration\":2}\n" +
                          "{\"schemaVersion\":1,\"timestampUs\":3000,\"recordKind\":\"dropped\","
                          "\"reason\":\"read failed\"}\n";
    auto path = WriteFixture("counts", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100000);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalDeltaRecords == 1);
    SH_CHECK(report.totalMarkers == 1);
    SH_CHECK(report.totalDiscontinuities == 1);
    SH_CHECK(report.totalDropped == 1);
    SH_CHECK(report.totalTrials == 0); // the one take_damage marker has no guard_input candidate -- orphan, excluded
    SH_CHECK(report.orphanMarkersExcluded == 1);
    SH_CHECK(report.ambiguousMarkersExcluded == 0);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_OutcomeLinkedToPrecedingGuardInput_UsesGuardInputAnchor) {
    // guard_input at t=990 (precise); delta at t=1000 (10us after); the
    // human's perfect_deflect judgment lands 510us later at t=1500 -- far
    // outside a 50us window on its own, but the trial anchors on the
    // guard_input, so the delta correlates.
    std::string content =
        MarkerLine(990, "guard_input") + DeltaLine(1000, 52, 0, 1) + MarkerLine(1500, "perfect_deflect");
    auto path = WriteFixture("linked", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 50);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalTrials == 1); // guard_input consumed by perfect_deflect -- not a second, separate trial
    const CombatCaptureOffsetTrialStat* stat = FindStat(report, 52, "perfect_deflect");
    SH_CHECK(stat != nullptr);
    if (stat != nullptr) {
        SH_CHECK(stat->trialsForLabel == 1);
        SH_CHECK(stat->trialsChangedForLabel == 1);
    }
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_UnclaimedGuardInput_BecomesItsOwnTrial) {
    std::string content = MarkerLine(1000, "guard_input") + DeltaLine(1010, 8, 0, 1);
    auto path = WriteFixture("unclaimed", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalTrials == 1);
    const CombatCaptureOffsetTrialStat* stat = FindStat(report, 8, "guard_input");
    SH_CHECK(stat != nullptr);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_TwoOutcomesAfterOneGuardInput_OnlyFirstClaimsItSecondIsOrphan) {
    // A mis-press scenario: one guard_input, then two outcome labels shortly
    // after (e.g. the human pressed F4 then corrected to F5). Only the
    // first (normal_block) should claim the guard_input as its anchor; the
    // second (perfect_deflect) has no remaining candidate -- orphan, excluded
    // from scoring rather than anchored at its own (imprecise) timestamp.
    std::string content =
        MarkerLine(1000, "guard_input") + MarkerLine(1100, "normal_block") + MarkerLine(1200, "perfect_deflect");
    auto path = WriteFixture("two-outcomes", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 50);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalTrials == 1); // normal_block only (anchored at 1000)
    SH_CHECK(report.orphanMarkersExcluded == 1); // perfect_deflect
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_TwoUnclaimedGuardInputsBeforeOneOutcome_IsAmbiguousAndExcluded) {
    // Two guard_input presses close together, then one outcome label that
    // could belong to either -- genuinely ambiguous which press it labels,
    // so it must be excluded rather than guessed at (e.g. "nearest wins").
    // Both guard_input candidates stay unclaimed and become their own
    // guard_input-labeled trials.
    std::string content =
        MarkerLine(1000, "guard_input") + MarkerLine(1050, "guard_input") + MarkerLine(1100, "normal_block");
    auto path = WriteFixture("ambiguous", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 50);
    SH_CHECK(report.ok);
    SH_CHECK(report.ambiguousMarkersExcluded == 1);
    // Closed windows [1000,1050] and [1050,1100] overlap at the edge.
    SH_CHECK(report.totalTrials == 0);
    SH_CHECK(report.overlappingTrialsExcluded == 2);
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_SupportAndFalsePositiveRatesComputedAcrossTrials) {
    // offset 8 changes near every normal_block trial (2/2) but only one of
    // two perfect_deflect trials (1/2) -- support/false-positive should
    // reflect that exactly.
    std::string content = MarkerLine(1000, "guard_input") + DeltaLine(1010, 8, 0, 1) +
                          MarkerLine(1050, "normal_block") + MarkerLine(2000, "guard_input") +
                          DeltaLine(2010, 8, 1, 2) + MarkerLine(2050, "normal_block") +
                          MarkerLine(3000, "guard_input") + DeltaLine(3010, 8, 2, 3) +
                          MarkerLine(3050, "perfect_deflect") + MarkerLine(4000, "guard_input") +
                          MarkerLine(4050, "perfect_deflect"); // no delta near this one
    auto path = WriteFixture("support-fp", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 50);
    SH_CHECK(report.ok);
    SH_CHECK(report.totalTrials == 4);

    const CombatCaptureOffsetTrialStat* normalBlockStat = FindStat(report, 8, "normal_block");
    SH_CHECK(normalBlockStat != nullptr);
    if (normalBlockStat != nullptr) {
        SH_CHECK(normalBlockStat->trialsForLabel == 2);
        SH_CHECK(normalBlockStat->trialsChangedForLabel == 2);
        SH_CHECK(normalBlockStat->trialsForOtherLabels == 2); // the two perfect_deflect trials
        SH_CHECK(normalBlockStat->trialsChangedForOtherLabels == 1);
    }

    const CombatCaptureOffsetTrialStat* deflectStat = FindStat(report, 8, "perfect_deflect");
    SH_CHECK(deflectStat != nullptr);
    if (deflectStat != nullptr) {
        SH_CHECK(deflectStat->trialsForLabel == 2);
        SH_CHECK(deflectStat->trialsChangedForLabel == 1);
    }
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_ExampleFieldsAndBitFlipsAreCorrect) {
    // 0 (0b00) -> 3 (0b11): bits 0 and 1 both flip 0->1.
    std::string content = MarkerLine(1000, "guard_input") + DeltaLine(1030, 16, 0, 3) + DeltaLine(1200, 16, 3, 3) +
                          MarkerLine(1010, "normal_block");
    auto path = WriteFixture("example-fields", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 100);
    SH_CHECK(report.ok);
    const CombatCaptureOffsetTrialStat* stat = FindStat(report, 16, "normal_block");
    SH_CHECK(stat != nullptr);
    if (stat != nullptr) {
        SH_CHECK(stat->exampleBeforeU32 == 0);
        SH_CHECK(stat->exampleAfterU32 == 3);
        SH_CHECK(stat->exampleFirstChangeUs == 30); // 1030 - 1000 (trial anchored at the guard_input)
        SH_CHECK(stat->exampleBitFlips.size() == 2);
        for (const CombatCaptureBitFlip& flip : stat->exampleBitFlips) {
            SH_CHECK(flip.bitIndex == 0 || flip.bitIndex == 1);
            SH_CHECK(flip.becameOne);
        }
    }
    std::filesystem::remove(path);
}

SH_TEST(AnalyzeCombatCaptureFile_SortedBySupportDescendingThenFalsePositiveAscending) {
    // offset 8: 2/2 support, 0/1 false-positive. offset 12: 1/2 support.
    // offset 8 must sort first (higher support).
    std::string content = MarkerLine(1000, "guard_input") + DeltaLine(1010, 8, 0, 1) + DeltaLine(1010, 12, 0, 1) +
                          MarkerLine(1050, "normal_block") + MarkerLine(2000, "guard_input") +
                          DeltaLine(2010, 8, 1, 2) + MarkerLine(2050, "normal_block") +
                          MarkerLine(3000, "guard_input") + MarkerLine(3050, "take_damage");
    auto path = WriteFixture("sorted", content);

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(path.string(), 50);
    SH_CHECK(report.ok);
    SH_CHECK(report.offsetStats.size() >= 2);
    for (std::size_t i = 1; i < report.offsetStats.size(); ++i) {
        bool inOrder = report.offsetStats[i - 1].SupportRate() > report.offsetStats[i].SupportRate() ||
                       (report.offsetStats[i - 1].SupportRate() == report.offsetStats[i].SupportRate() &&
                        report.offsetStats[i - 1].FalsePositiveRate() <= report.offsetStats[i].FalsePositiveRate());
        SH_CHECK(inOrder);
    }
    std::filesystem::remove(path);
}
