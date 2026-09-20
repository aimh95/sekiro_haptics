#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"

#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"

#include <iomanip>
#include <filesystem>
#include <utility>

namespace sekiro_haptics::process {

namespace {

std::size_t ParseSizeOr(const std::string& text, std::size_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::size_t>(std::stoull(text, nullptr, 0));
    } catch (...) {
        return fallback;
    }
}

std::int64_t ParseInt64Or(const std::string& text, std::int64_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::int64_t>(std::stoll(text, nullptr, 0));
    } catch (...) {
        return fallback;
    }
}

/// Parses a runtime address (accepts an optional "0x" prefix via base-0
/// stoull). Returns 0 (an already-invalid sentinel address) on any parse
/// failure or empty input -- never guessed.
std::uintptr_t ParseAddressOrZero(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    try {
        std::size_t consumed = 0;
        unsigned long long value = std::stoull(text, &consumed, 0);
        if (consumed != text.size()) {
            return 0;
        }
        return static_cast<std::uintptr_t>(value);
    } catch (...) {
        return 0;
    }
}

} // namespace

SekiroCombatCommandProcessor::SekiroCombatCommandProcessor(SekiroCombatSessionController& controller,
                                                             std::string captureOutputPath)
    : controller_(controller), captureOutputPath_(captureOutputPath),
      preferredCaptureOutputPath_(std::move(captureOutputPath)) {}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::Process(const std::string& commandLine,
                                                                                   std::int64_t inputTimestampUs,
                                                                                   std::int64_t processedTimestampUs) {
    std::istringstream iss(commandLine);
    std::string verb;
    iss >> verb;

    if (verb == "combat-plan") {
        return HandleCombatPlan();
    }
    if (verb == "combat-resolve") {
        return HandleCombatResolve();
    }
    if (verb == "combat-status") {
        return HandleCombatStatus();
    }
    if (verb == "combat-capture") {
        return HandleCombatCapture(iss, processedTimestampUs);
    }
    if (verb == "combat-mark") {
        return HandleCombatMark(iss, inputTimestampUs, processedTimestampUs);
    }
    if (verb == "combat-stop") {
        return HandleCombatStop();
    }
    if (verb == "combat-analyze") {
        return HandleCombatAnalyze(iss);
    }
    if (verb == "combat-export") {
        return HandleCombatExport();
    }

    return ProcessResult{false, {}};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatPlan() {
    CombatPlanReport report = controller_.Plan();
    std::vector<std::string> lines;
    if (!report.moduleFound) {
        lines.push_back("combat-plan failed: could not identify main module");
        return {true, lines};
    }
    lines.push_back("aobScanRangeBytes=" + std::to_string(report.aobScanRangeBytes));
    lines.push_back("expectedBytesPerSample=" + std::to_string(report.expectedBytesPerSampleBytes));
    lines.push_back(std::string("fullScanUsed=") + (report.fullScanUsed ? "true" : "false"));
    lines.push_back("(plan only -- no AOB scan or process read was performed)");
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatResolve() {
    CombatResolveResult result = controller_.Resolve();
    std::vector<std::string> lines;
    lines.push_back(std::string("status=") + ToString(result.status));
    if (result.status == CombatSnapshotStatus::ResolvedUnvalidated) {
        std::ostringstream oss;
        oss << "gameDataManAddress=0x" << std::hex << result.gameDataManAddress << " playerGameDataAddress=0x"
            << result.playerGameDataAddress << std::dec;
        lines.push_back(oss.str());
        lines.push_back("generation=" + std::to_string(result.generation));
        lines.push_back("(pointer chain resolved -- run combat-status to read/validate HP/Posture)");
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatStatus() {
    CombatSnapshot snapshot = controller_.Snapshot();
    std::vector<std::string> lines;
    lines.push_back(std::string("status=") + ToString(snapshot.status));
    lines.push_back("generation=" + std::to_string(snapshot.generation));
    if (snapshot.status == CombatSnapshotStatus::Valid || snapshot.status == CombatSnapshotStatus::InvariantViolation) {
        lines.push_back("hp=" + std::to_string(snapshot.hp) + "/" + std::to_string(snapshot.maxHp));
        lines.push_back("posture=" + std::to_string(snapshot.posture) + "/" + std::to_string(snapshot.maxPosture));
    }
    if (snapshot.status == CombatSnapshotStatus::TemporarilyUnavailable) {
        lines.push_back("(not resolved yet -- run combat-resolve first)");
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatCapture(
    std::istringstream& args, std::int64_t nowMonotonicUs) {
    std::string scopeText;
    args >> scopeText;

    static const char* kUsage[] = {
        "usage: combat-capture player-game-data [window-size-bytes] [interval-ms]",
        "       combat-capture custom-address <hex-address> [window-size-bytes] [interval-ms]"};

    CombatCaptureConfig config;
    bool noActiveResolveHint = false;

    if (scopeText == "player-game-data") {
        std::string windowText, intervalText;
        args >> windowText >> intervalText;
        config.scope = CombatCaptureScope::PlayerGameData;
        config.requestedWindowSizeBytes = ParseSizeOr(windowText, kPlayerGameDataMaxCaptureBytes);
        config.samplingInterval =
            std::chrono::milliseconds(ParseInt64Or(intervalText, kDefaultCombatCaptureIntervalMs.count()));
        noActiveResolveHint = true;
    } else if (scopeText == "custom-address") {
        std::string addrText, windowText, intervalText;
        args >> addrText >> windowText >> intervalText;
        std::uintptr_t address = ParseAddressOrZero(addrText);
        if (address == 0) {
            return {true, {kUsage[0], kUsage[1], "  invalid or missing <hex-address> (e.g. 0x143d9b458)"}};
        }
        config.scope = CombatCaptureScope::CustomAddress;
        config.customBaseAddress = address;
        config.requestedWindowSizeBytes = ParseSizeOr(windowText, kCustomAddressMaxCaptureBytes);
        config.samplingInterval =
            std::chrono::milliseconds(ParseInt64Or(intervalText, kDefaultCombatCaptureIntervalMs.count()));
    } else {
        return {true, {kUsage[0], kUsage[1]}};
    }

    CombatCaptureStartResult result;
    std::filesystem::path path = preferredCaptureOutputPath_;
    for (unsigned attempt = 0; ; ++attempt) {
        result = controller_.StartCapture(config, path.string(), nowMonotonicUs);
        if (result != CombatCaptureStartResult::OutputExists || attempt == 999) break;
        const std::filesystem::path preferred(preferredCaptureOutputPath_);
        path = preferred.parent_path() / (preferred.stem().string() + "_" +
               std::to_string(++capturePathSuffix_) + preferred.extension().string());
    }
    if (result == CombatCaptureStartResult::Started) captureOutputPath_ = path.string();
    std::vector<std::string> lines;
    lines.push_back(std::string("result=") + ToString(result));
    if (result == CombatCaptureStartResult::InvalidConfig && noActiveResolveHint &&
        controller_.LastResolve().playerGameDataAddress == 0) {
        lines.push_back("  no active resolve -- run combat-resolve first");
    }
    if (result == CombatCaptureStartResult::Started) {
        lines.push_back("scope=" + std::string(ToString(config.scope)));
        lines.push_back("output=" + captureOutputPath_);
        lines.push_back("(schema v3: baseline + deltas + every sample + gaps; raw values are unvalidated. "
                         "use combat-mark <label> while playing, combat-stop when done)");
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatMark(
    std::istringstream& args, std::int64_t inputTimestampUs, std::int64_t processedTimestampUs) {
    std::string label;
    std::getline(args, label);
    while (!label.empty() && label.front() == ' ') {
        label.erase(label.begin());
    }
    if (label.empty()) {
        return {true, {"usage: combat-mark <label>"}};
    }
    bool ok = controller_.CaptureMark(label, inputTimestampUs, processedTimestampUs);
    return {true, {ok ? ("marked: " + label) : "combat-mark failed -- no capture is running or output failed"}};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatStop() {
    if (!controller_.IsCapturing()) {
        if (controller_.CaptureStats().outputFailed) {
            controller_.StopCapture();
            return {true, {"capture output failed; inspect the incomplete file and choose a new capture"}};
        }
        return {true, {"no capture is running"}};
    }
    controller_.StopCapture();
    CombatCaptureStats stats = controller_.CaptureStats();
    std::vector<std::string> lines;
    lines.push_back(stats.outputFailed ? "capture stopped with OUTPUT FAILURE" : "capture stopped");
    lines.push_back("outputFailed=" + std::string(stats.outputFailed ? "true" : "false"));
    lines.push_back("baselineRecordsWritten=" + std::to_string(stats.baselineRecordsWritten));
    lines.push_back("sampleRecordsWritten=" + std::to_string(stats.sampleRecordsWritten));
    lines.push_back("inconsistentSamples=" + std::to_string(stats.inconsistentSamples));
    lines.push_back("samplesTaken=" + std::to_string(stats.samplesTaken));
    lines.push_back("deltaRecordsWritten=" + std::to_string(stats.deltaRecordsWritten));
    lines.push_back("markersWritten=" + std::to_string(stats.markersWritten));
    lines.push_back("markersRejectedNotRunning=" + std::to_string(stats.markersRejectedNotRunning));
    lines.push_back("lateSamples=" + std::to_string(stats.lateSamples) +
                     " (lateToleranceUs=" + std::to_string(stats.lateToleranceUs) + ")");
    lines.push_back("droppedSamplesTotal=" + std::to_string(stats.droppedSamplesTotal));
    lines.push_back("  missedScheduleSamples=" + std::to_string(stats.missedScheduleSamples) +
                     " (Tick() calls fell behind -- slot skipped, never attempted)");
    lines.push_back("  readFailedSamples=" + std::to_string(stats.readFailedSamples) +
                     " (read attempted, reader reported failure)");
    lines.push_back("  unresolvedSamples=" + std::to_string(stats.unresolvedSamples) +
                     " (regionBaseAddress was 0 -- not currently resolved)");
    lines.push_back("discontinuities=" + std::to_string(stats.discontinuities));
    lines.push_back("configuredIntervalUs=" + std::to_string(stats.configuredIntervalUs));
    lines.push_back("actualIntervalUs: min=" + std::to_string(stats.minIntervalUs) +
                     " p50=" + std::to_string(stats.p50IntervalUs) + " p95=" + std::to_string(stats.p95IntervalUs) +
                     " p99=" + std::to_string(stats.p99IntervalUs) + " max=" + std::to_string(stats.maxIntervalUs));
    lines.push_back("latenessUs (actual - scheduled slot): p50=" + std::to_string(stats.p50LatenessUs) +
                     " p95=" + std::to_string(stats.p95LatenessUs) + " max=" + std::to_string(stats.maxLatenessUs));
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatAnalyze(std::istringstream& args) {
    std::string windowText;
    std::string lookbackText;
    args >> windowText >> lookbackText;
    std::int64_t windowUs = ParseInt64Or(windowText, 200) * 1000; // arg is milliseconds, default 200ms
    std::int64_t lookbackUs = ParseInt64Or(lookbackText, kDefaultGuardInputLookbackUs / 1000) *
                               1000; // arg is milliseconds, default 1000ms (SEK-PROBE-001E Section 7)

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(captureOutputPath_, windowUs, lookbackUs);
    std::vector<std::string> lines;
    if (!report.ok) {
        lines.push_back("combat-analyze failed: " + report.error);
        return {true, lines};
    }
    lines.push_back("totalDeltaRecords=" + std::to_string(report.totalDeltaRecords));
    lines.push_back("totalMarkers=" + std::to_string(report.totalMarkers));
    lines.push_back("totalDiscontinuities=" + std::to_string(report.totalDiscontinuities));
    lines.push_back("totalDropped=" + std::to_string(report.totalDropped));
    lines.push_back("totalTrials=" + std::to_string(report.totalTrials));
    lines.push_back("orphanMarkersExcluded=" + std::to_string(report.orphanMarkersExcluded) +
                     " (no candidate guard_input within lookback)");
    lines.push_back("ambiguousMarkersExcluded=" + std::to_string(report.ambiguousMarkersExcluded) +
                     " (more than one candidate guard_input within lookback)");
    lines.push_back("windowMs=" + std::to_string(windowUs / 1000));
    lines.push_back("guardInputLookbackMs=" + std::to_string(lookbackUs / 1000));
    lines.push_back("--- top offset/label candidates by support desc, false-positive asc "
                     "(not a validated signal -- see header comment) ---");
    std::size_t shown = 0;
    for (const CombatCaptureOffsetTrialStat& stat : report.offsetStats) {
        if (shown >= 30) {
            lines.push_back("... (truncated, " + std::to_string(report.offsetStats.size() - shown) + " more)");
            break;
        }
        std::ostringstream oss;
        oss << "offset=0x" << std::hex << stat.offset << std::dec << " label=" << stat.label << " support="
            << stat.trialsChangedForLabel << "/" << stat.trialsForLabel << " (" << std::fixed
            << std::setprecision(1) << (stat.SupportRate() * 100.0) << "%)"
            << " falsePositive=" << stat.trialsChangedForOtherLabels << "/" << stat.trialsForOtherLabels << " ("
            << (stat.FalsePositiveRate() * 100.0) << "%)";
        lines.push_back(oss.str());

        std::ostringstream example;
        example << "  example: before=0x" << std::hex << stat.exampleBeforeU32 << " after=0x"
                << stat.exampleAfterU32 << std::dec << " firstChangeUs=" << stat.exampleFirstChangeUs
                << " durationHeldUs="
                << (stat.exampleDurationHeldUs < 0 ? std::string("unbounded")
                                                    : std::to_string(stat.exampleDurationHeldUs));
        if (!stat.exampleBitFlips.empty()) {
            example << " bitFlips=[";
            for (std::size_t i = 0; i < stat.exampleBitFlips.size(); ++i) {
                if (i > 0) example << ",";
                example << "bit" << stat.exampleBitFlips[i].bitIndex << (stat.exampleBitFlips[i].becameOne ? "->1" : "->0");
            }
            example << "]";
        }
        lines.push_back(example.str());
        ++shown;
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatExport() {
    CombatCaptureStats stats = controller_.CaptureStats();
    std::vector<std::string> lines;
    lines.push_back("path=" + captureOutputPath_);
    lines.push_back("schemaVersion=3 (tools/deflect_capture.py reconstructs raw observations)");
    lines.push_back("outputFailed=" + std::string(stats.outputFailed ? "true" : "false"));
    lines.push_back("running=" + std::string(controller_.IsCapturing() ? "true" : "false"));
    lines.push_back("samplesTaken=" + std::to_string(stats.samplesTaken));
    lines.push_back("deltaRecordsWritten=" + std::to_string(stats.deltaRecordsWritten));
    lines.push_back("markersWritten=" + std::to_string(stats.markersWritten));
    return {true, lines};
}

} // namespace sekiro_haptics::process
