#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"

#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"

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

} // namespace

SekiroCombatCommandProcessor::SekiroCombatCommandProcessor(SekiroCombatSessionController& controller,
                                                             std::string captureOutputPath)
    : controller_(controller), captureOutputPath_(std::move(captureOutputPath)) {}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::Process(const std::string& commandLine,
                                                                                   std::int64_t nowMonotonicUs) {
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
        return HandleCombatCapture(iss, nowMonotonicUs);
    }
    if (verb == "combat-mark") {
        return HandleCombatMark(iss, nowMonotonicUs);
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
    std::string scopeText, windowText, intervalText;
    args >> scopeText >> windowText >> intervalText;

    if (scopeText != "player-game-data") {
        return {true, {"usage: combat-capture player-game-data [window-size-bytes] [interval-ms]",
                        "  (player-game-data is the only supported scope so far -- see "
                        "SekiroCombatCaptureSession.hpp)"}};
    }

    CombatCaptureConfig config;
    config.scope = CombatCaptureScope::PlayerGameData;
    config.requestedWindowSizeBytes = ParseSizeOr(windowText, kPlayerGameDataMaxCaptureBytes);
    config.samplingInterval =
        std::chrono::milliseconds(ParseInt64Or(intervalText, kDefaultCombatCaptureIntervalMs.count()));

    CombatCaptureStartResult result = controller_.StartCapture(config, captureOutputPath_, nowMonotonicUs);
    std::vector<std::string> lines;
    lines.push_back(std::string("result=") + ToString(result));
    if (result == CombatCaptureStartResult::InvalidConfig && controller_.LastResolve().playerGameDataAddress == 0) {
        lines.push_back("  no active resolve -- run combat-resolve first");
    }
    if (result == CombatCaptureStartResult::Started) {
        lines.push_back("output=" + captureOutputPath_);
        lines.push_back("(delta-only capture -- the full raw block is never written; "
                         "use combat-mark <label> while playing, combat-stop when done)");
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatMark(std::istringstream& args,
                                                                                             std::int64_t nowMonotonicUs) {
    std::string label;
    std::getline(args, label);
    while (!label.empty() && label.front() == ' ') {
        label.erase(label.begin());
    }
    if (label.empty()) {
        return {true, {"usage: combat-mark <label>"}};
    }
    bool ok = controller_.CaptureMark(label, nowMonotonicUs);
    return {true, {ok ? ("marked: " + label) : "combat-mark failed -- no capture is running"}};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatStop() {
    if (!controller_.IsCapturing()) {
        return {true, {"no capture is running"}};
    }
    CombatCaptureStats stats = controller_.CaptureStats();
    controller_.StopCapture();
    std::vector<std::string> lines;
    lines.push_back("capture stopped");
    lines.push_back("samplesTaken=" + std::to_string(stats.samplesTaken));
    lines.push_back("deltaRecordsWritten=" + std::to_string(stats.deltaRecordsWritten));
    lines.push_back("markersWritten=" + std::to_string(stats.markersWritten));
    lines.push_back("lateSamples=" + std::to_string(stats.lateSamples));
    lines.push_back("droppedSamples=" + std::to_string(stats.droppedSamples));
    lines.push_back("discontinuities=" + std::to_string(stats.discontinuities));
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatAnalyze(std::istringstream& args) {
    std::string windowText;
    args >> windowText;
    std::int64_t windowUs = ParseInt64Or(windowText, 200) * 1000; // arg is milliseconds, default 200ms

    CombatCaptureAnalysisReport report = AnalyzeCombatCaptureFile(captureOutputPath_, windowUs);
    std::vector<std::string> lines;
    if (!report.ok) {
        lines.push_back("combat-analyze failed: " + report.error);
        return {true, lines};
    }
    lines.push_back("totalDeltaRecords=" + std::to_string(report.totalDeltaRecords));
    lines.push_back("totalMarkers=" + std::to_string(report.totalMarkers));
    lines.push_back("totalDiscontinuities=" + std::to_string(report.totalDiscontinuities));
    lines.push_back("totalDropped=" + std::to_string(report.totalDropped));
    lines.push_back("windowMs=" + std::to_string(windowUs / 1000));
    lines.push_back("--- top offset/marker correlations (not a validated signal -- see header comment) ---");
    std::size_t shown = 0;
    for (const CombatCaptureOffsetMarkerStat& stat : report.offsetMarkerStats) {
        if (shown >= 30) {
            lines.push_back("... (truncated, " + std::to_string(report.offsetMarkerStats.size() - shown) +
                             " more)");
            break;
        }
        std::ostringstream oss;
        oss << "offset=0x" << std::hex << stat.offset << std::dec << " label=" << stat.label
            << " changeCount=" << stat.changeCount << "/" << stat.markerOccurrences << " occurrences";
        lines.push_back(oss.str());
        ++shown;
    }
    return {true, lines};
}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::HandleCombatExport() {
    CombatCaptureStats stats = controller_.CaptureStats();
    std::vector<std::string> lines;
    lines.push_back("path=" + captureOutputPath_);
    lines.push_back("running=" + std::string(controller_.IsCapturing() ? "true" : "false"));
    lines.push_back("samplesTaken=" + std::to_string(stats.samplesTaken));
    lines.push_back("deltaRecordsWritten=" + std::to_string(stats.deltaRecordsWritten));
    lines.push_back("markersWritten=" + std::to_string(stats.markersWritten));
    return {true, lines};
}

} // namespace sekiro_haptics::process
