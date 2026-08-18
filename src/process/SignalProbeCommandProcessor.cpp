#include "sekiro_haptics/process/SignalProbeCommandProcessor.hpp"

#include <iomanip>
#include <optional>

namespace sekiro_haptics::process {

namespace {

std::string FormatBytesHuman(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < (sizeof(kUnits) / sizeof(kUnits[0]))) {
        value /= 1024.0;
        ++unitIndex;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(unitIndex == 0 ? 0 : 2) << value << " " << kUnits[unitIndex];
    return oss.str();
}

std::string FormatBytesField(const char* label, std::uint64_t bytes) {
    std::ostringstream oss;
    oss << label << "=" << bytes << " (" << FormatBytesHuman(bytes) << ")";
    return oss.str();
}

std::optional<CandidateValue> ParseValueForType(const std::string& text, CandidateValueType type) {
    try {
        std::size_t consumed = 0;
        if (type == CandidateValueType::F32) {
            float f = std::stof(text, &consumed);
            if (consumed != text.size()) return std::nullopt;
            return CandidateValue{f};
        }
        if (type == CandidateValueType::I32) {
            long long v = std::stoll(text, &consumed, 0);
            if (consumed != text.size()) return std::nullopt;
            return CandidateValue{static_cast<std::int32_t>(v)};
        }
        unsigned long long v = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) return std::nullopt;
        switch (type) {
            case CandidateValueType::U8:
                return CandidateValue{static_cast<std::uint8_t>(v)};
            case CandidateValueType::U16:
                return CandidateValue{static_cast<std::uint16_t>(v)};
            case CandidateValueType::U32:
                return CandidateValue{static_cast<std::uint32_t>(v)};
            default:
                return std::nullopt;
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::string FormatCandidateValue(CandidateValueType type, const CandidateValue& value) {
    std::ostringstream oss;
    switch (type) {
        case CandidateValueType::U8:
            oss << static_cast<unsigned int>(std::get<std::uint8_t>(value));
            break;
        case CandidateValueType::U16:
            oss << std::get<std::uint16_t>(value);
            break;
        case CandidateValueType::U32:
            oss << std::get<std::uint32_t>(value);
            break;
        case CandidateValueType::I32:
            oss << std::get<std::int32_t>(value);
            break;
        case CandidateValueType::F32:
            oss << std::get<float>(value);
            break;
    }
    return oss.str();
}

} // namespace

SignalProbeCommandProcessor::SignalProbeCommandProcessor(SignalProbeScanController& controller)
    : controller_(controller) {}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::Process(const std::string& commandLine) {
    std::istringstream iss(commandLine);
    std::string verb;
    iss >> verb;

    if (verb == "plan") {
        return HandlePlan(iss);
    }
    if (verb == "begin") {
        return HandleBegin(iss);
    }
    if (verb == "begin-disk") {
        return HandleBeginDisk(iss);
    }
    if (verb == "filter") {
        return HandleFilter(iss);
    }
    if (verb == "resume") {
        return HandleResume();
    }
    if (verb == "status") {
        return HandleStatus();
    }
    if (verb == "list") {
        return HandleList(iss);
    }

    return ProcessResult{false, {}};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandlePlan(std::istringstream& args) {
    std::string typeText, scopeText;
    args >> typeText >> scopeText;
    CandidateValueType type;
    CandidateScanScope scope;
    if (!ParseCandidateValueType(typeText, type) || !ParseCandidateScanScope(scopeText, scope)) {
        return {true, {"usage: plan <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>"}};
    }

    PlanCommandResult result = controller_.Plan(type, scope);
    if (result.outcome != PlanCandidateScanOutcome::Success) {
        return {true, {std::string("plan failed: ") + ToString(result.outcome)}};
    }

    const ScanPlanReport& report = result.report;
    bool inMemoryFeasible = report.estimatedInMemoryRamBytes <= report.memoryBudgetBytes;
    std::vector<std::string> lines;
    lines.push_back("scope=" + std::string(ToString(scope)) + " type=" + std::string(ToString(type)));
    lines.push_back("regions=" + std::to_string(report.regionCount));
    lines.push_back(FormatBytesField("scopeBytes", report.totalScopeBytes));
    lines.push_back("alignedValueCount=" + std::to_string(report.comparableValueCount));
    lines.push_back(FormatBytesField("estimatedInMemoryBytes", report.estimatedInMemoryRamBytes));
    lines.push_back(FormatBytesField("memoryBudgetBytes", report.memoryBudgetBytes));
    lines.push_back(std::string("inMemoryFeasible=") + (inMemoryFeasible ? "yes" : "no"));
    lines.push_back(FormatBytesField("estimatedBaselineFileBytes", report.estimatedBaselineFileBytes));
    lines.push_back(FormatBytesField("estimatedPeakDiskBytes", report.requiredDiskSpaceBytes));
    lines.push_back(FormatBytesField("outputDriveFreeBytes", report.availableDiskSpaceBytes));
    lines.push_back("diskBackedFeasible=yes (preflight passed)");
    lines.push_back(std::string("recommendedStorageMode=") + ToString(report.recommendedStorageMode));
    std::ostringstream coverageLine;
    coverageLine << "coverageTargetPercent=" << report.expectedCoveragePercent;
    lines.push_back(coverageLine.str());
    lines.push_back("(plan only -- no file or snapshot was created)");
    return {true, lines};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleBegin(std::istringstream& args) {
    std::string typeText, scopeText;
    args >> typeText >> scopeText;
    CandidateValueType type;
    CandidateScanScope scope;
    if (!ParseCandidateValueType(typeText, type) || !ParseCandidateScanScope(scopeText, scope)) {
        return {true, {"usage: begin <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>"}};
    }

    BeginCommandResult result = controller_.Begin(type, scope);
    if (result.outcome == BeginCommandOutcome::Success) {
        return {true, {"scan complete: " + std::to_string(result.candidateCount) + " candidate(s)"}};
    }
    if (result.outcome == BeginCommandOutcome::InMemoryBudgetExceeded) {
        std::vector<std::string> lines;
        lines.push_back("in-memory budget exceeded -- refused before allocating any candidate memory");
        lines.push_back("  " + std::to_string(result.totals.comparableValueCount) + " candidate(s) would need " +
                         FormatBytesHuman(result.totals.comparableValueCount * sizeof(Candidate)));
        lines.push_back("use \"begin-disk " + typeText + " " + scopeText + "\" instead for a RAM-bounded scan.");
        return {true, lines};
    }

    std::vector<std::string> lines;
    lines.push_back(std::string("scan failed: ") + ToString(result.scanResult));
    if (result.scanResult == CandidateScanResult::CandidateLimitExceeded) {
        lines.push_back("  this scope produced more candidates than the configured caps allow.");
        lines.push_back("  use \"begin-disk " + typeText + " " + scopeText +
                         "\" instead of raising --max-candidates/--max-scan-bytes.");
    }
    return {true, lines};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleBeginDisk(std::istringstream& args) {
    std::string typeText, scopeText;
    args >> typeText >> scopeText;
    CandidateValueType type;
    CandidateScanScope scope;
    if (!ParseCandidateValueType(typeText, type) || !ParseCandidateScanScope(scopeText, scope)) {
        return {true, {"usage: begin-disk <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>"}};
    }

    BeginDiskCommandResult result = controller_.BeginDisk(type, scope);
    if (!result.ranScan) {
        return {true, {std::string("preflight failed: ") + ToString(result.planOutcome) +
                        " -- nothing was written to disk"}};
    }
    if (result.scanOutcome != DiskScanOutcome::CompleteCoverage) {
        std::vector<std::string> lines;
        lines.push_back(std::string("disk scan failed: ") + ToString(result.scanOutcome));
        lines.push_back("  regionsProcessed=" + std::to_string(result.stats.regionsProcessed) + "/" +
                         std::to_string(result.stats.regionsTotal));
        lines.push_back("  " + FormatBytesField("processedBytes", result.stats.processedBytes));
        return {true, lines};
    }

    std::vector<std::string> lines;
    lines.push_back("disk scan complete: coverage=100% candidates=" +
                     std::to_string(result.stats.survivingCandidateCount));
    lines.push_back("  " + FormatBytesField("processedBytes", result.stats.processedBytes));
    lines.push_back("  " + FormatBytesField("peakBufferedBytes", result.stats.peakBufferedBytes) +
                     " (bounded by configured budget " + FormatBytesHuman(result.stats.configuredMemoryBudgetBytes) +
                     ")");
    return {true, lines};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleFilter(std::istringstream& args) {
    if (controller_.Mode() == ScanMode::None) {
        return {true, {"no active scan -- run \"begin\" or \"begin-disk\" first"}};
    }

    std::string kindText;
    args >> kindText;

    CandidateFilterKind kind;
    CandidateValue exactValue{std::uint32_t{0}};
    const CandidateValue* exactTarget = nullptr;

    if (kindText == "changed") {
        kind = CandidateFilterKind::Changed;
    } else if (kindText == "unchanged") {
        kind = CandidateFilterKind::Unchanged;
    } else if (kindText == "increased") {
        kind = CandidateFilterKind::Increased;
    } else if (kindText == "decreased") {
        kind = CandidateFilterKind::Decreased;
    } else if (kindText == "exact") {
        kind = CandidateFilterKind::ExactValue;
        std::optional<CandidateValueType> currentType = controller_.CurrentValueType();
        std::string valueText;
        args >> valueText;
        if (!currentType.has_value()) {
            return {true, {"no active scan -- run \"begin\" or \"begin-disk\" first"}};
        }
        auto parsed = ParseValueForType(valueText, *currentType);
        if (!parsed.has_value()) {
            return {true, {"invalid value for current type"}};
        }
        exactValue = *parsed;
        exactTarget = &exactValue;
    } else {
        return {true, {"usage: filter <changed|unchanged|increased|decreased>", "       filter exact <value>"}};
    }

    FilterCommandResult result = controller_.Filter(kind, exactTarget);

    if (result.outcome == FilterCommandOutcome::NoActiveScan) {
        return {true, {"no active scan -- run \"begin\" or \"begin-disk\" first"}};
    }
    if (result.outcome == FilterCommandOutcome::InvalidCommand) {
        return {true, {"usage: filter <changed|unchanged|increased|decreased>", "       filter exact <value>"}};
    }
    if (result.outcome == FilterCommandOutcome::InMemoryFilterFailed) {
        return {true, {std::string("filter failed: ") + ToString(result.inMemoryResult)}};
    }
    if (result.outcome == FilterCommandOutcome::DiskFilterFailed) {
        std::vector<std::string> lines;
        lines.push_back(std::string("filter failed: ") + ToString(result.diskResult));
        if (result.diskResult == DiskScanOutcome::InitialFilterTooBroad) {
            lines.push_back("  \"unchanged\" cannot be the first filter on a fresh baseline -- try decreased,");
            lines.push_back("  increased, changed, or exact <value> instead.");
        }
        return {true, lines};
    }

    // Success.
    if (controller_.Mode() == ScanMode::InMemory) {
        return {true, {"filter complete: " + std::to_string(result.inMemoryRemainingCount) + " candidate(s) remain"}};
    }
    std::vector<std::string> lines;
    lines.push_back("filter complete: coverage=100% candidates=" +
                     std::to_string(result.diskStats.survivingCandidateCount));
    if (result.diskStats.droppedCandidateCount > 0) {
        lines.push_back("  droppedUnreadable=" + std::to_string(result.diskStats.droppedCandidateCount));
    }
    return {true, lines};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleResume() {
    ResumeCommandResult result = controller_.Resume();
    if (result.outcome != DiskScanOutcome::CompleteCoverage) {
        return {true, {std::string("resume failed: ") + ToString(result.outcome)}};
    }
    std::ostringstream oss;
    oss << "resume complete: generation=" << result.manifest.generation
        << " candidates=" << result.manifest.candidateCount << " coverage=" << result.manifest.coveragePercent << "%";
    return {true, {oss.str()}};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleStatus() {
    StatusSnapshot snapshot = controller_.Status();
    std::vector<std::string> lines;
    lines.push_back(std::string("mode=") + ToString(snapshot.mode));

    if (snapshot.mode == ScanMode::InMemory) {
        if (snapshot.inMemoryType.has_value()) {
            lines.push_back("type=" + std::string(ToString(*snapshot.inMemoryType)));
        }
        if (snapshot.inMemoryScope.has_value()) {
            lines.push_back("scope=" + std::string(ToString(*snapshot.inMemoryScope)));
        }
        lines.push_back("candidates=" + std::to_string(snapshot.inMemoryCandidateCount));
    } else if (snapshot.mode == ScanMode::Disk && snapshot.diskManifest.has_value()) {
        const ScanManifest& manifest = *snapshot.diskManifest;
        lines.push_back(std::string("manifestState=") + ToString(manifest.state));
        lines.push_back("type=" + manifest.identity.valueType);
        lines.push_back("scope=" + manifest.identity.scope);
        lines.push_back("generation=" + std::to_string(manifest.generation));
        lines.push_back("candidates=" + std::to_string(manifest.candidateCount));
        lines.push_back(FormatBytesField("processedBytes", manifest.processedBytes));
        lines.push_back(FormatBytesField("totalBytes", manifest.totalScopeBytes));
        std::ostringstream coverageLine;
        coverageLine << "coveragePercent=" << manifest.coveragePercent;
        lines.push_back(coverageLine.str());
        lines.push_back(FormatBytesField("memoryBudgetBytes", manifest.memoryBudgetBytes));
        lines.push_back(FormatBytesField("peakBufferedBytes", snapshot.lastDiskStats.peakBufferedBytes));
        lines.push_back("droppedUnreadable=" + std::to_string(snapshot.lastDiskStats.droppedCandidateCount));
        lines.push_back("currentFile=" + CurrentDataFileName(manifest.generation));
    }

    return {true, lines};
}

SignalProbeCommandProcessor::ProcessResult SignalProbeCommandProcessor::HandleList(std::istringstream& args) {
    std::size_t count = 10;
    std::string countText;
    if (args >> countText) {
        try {
            count = static_cast<std::size_t>(std::stoull(countText));
        } catch (...) {
            return {true, {"invalid count -- expected a non-negative integer"}};
        }
    }

    std::vector<ListEntry> entries = controller_.List(count);
    std::vector<std::string> lines;
    lines.reserve(entries.size());
    for (const ListEntry& entry : entries) {
        std::ostringstream oss;
        oss << "0x" << std::hex << entry.address << std::dec << " = " << FormatCandidateValue(entry.type, entry.value);
        lines.push_back(oss.str());
    }
    return {true, lines};
}

} // namespace sekiro_haptics::process
