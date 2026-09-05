#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"

namespace sekiro_haptics::process {

SekiroCombatCommandProcessor::SekiroCombatCommandProcessor(SekiroCombatSessionController& controller)
    : controller_(controller) {}

SekiroCombatCommandProcessor::ProcessResult SekiroCombatCommandProcessor::Process(const std::string& commandLine) {
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

} // namespace sekiro_haptics::process
