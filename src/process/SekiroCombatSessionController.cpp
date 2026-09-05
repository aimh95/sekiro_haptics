#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"

namespace sekiro_haptics::process {

SekiroCombatSessionController::SekiroCombatSessionController(SekiroRawCombatReader& reader, IProcessInspector& inspector)
    : reader_(reader), inspector_(inspector) {}

CombatPlanReport SekiroCombatSessionController::Plan() const {
    CombatPlanReport report;
    ModuleInfo module;
    report.moduleFound = inspector_.GetMainModule(module) == ProcessInspectionResult::Success;
    if (report.moduleFound) {
        report.aobScanRangeBytes = module.imageSize;
    }
    return report;
}

CombatResolveResult SekiroCombatSessionController::Resolve() {
    lastResolve_ = reader_.Resolve();
    return lastResolve_;
}

CombatSnapshot SekiroCombatSessionController::Snapshot() {
    lastSnapshot_ = reader_.ReadSnapshot();
    return lastSnapshot_;
}

} // namespace sekiro_haptics::process
