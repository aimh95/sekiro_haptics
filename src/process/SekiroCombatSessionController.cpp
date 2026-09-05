#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"

namespace sekiro_haptics::process {

SekiroCombatSessionController::SekiroCombatSessionController(SekiroRawCombatReader& combatReader,
                                                               IProcessInspector& inspector,
                                                               IProcessReader& processReader)
    : combatReader_(combatReader), inspector_(inspector), processReader_(processReader) {}

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
    lastResolve_ = combatReader_.Resolve();
    return lastResolve_;
}

CombatSnapshot SekiroCombatSessionController::Snapshot() {
    lastSnapshot_ = combatReader_.ReadSnapshot();
    return lastSnapshot_;
}

CombatCaptureStartResult SekiroCombatSessionController::StartCapture(const CombatCaptureConfig& config,
                                                                       const std::string& outputPath,
                                                                       std::int64_t nowMonotonicUs) {
    if (lastResolve_.status != CombatSnapshotStatus::ResolvedUnvalidated || lastResolve_.playerGameDataAddress == 0) {
        return CombatCaptureStartResult::InvalidConfig;
    }
    captureSession_ = std::make_unique<SekiroCombatCaptureSession>(processReader_);
    return captureSession_->Start(config, lastResolve_.playerGameDataAddress, lastResolve_.generation, outputPath,
                                   nowMonotonicUs);
}

bool SekiroCombatSessionController::CaptureTick(std::int64_t nowMonotonicUs) {
    if (!captureSession_ || !captureSession_->IsRunning()) {
        return false;
    }
    bool resolved = lastResolve_.status == CombatSnapshotStatus::ResolvedUnvalidated;
    std::uintptr_t address = resolved ? lastResolve_.playerGameDataAddress : 0;
    std::uint64_t generation = resolved ? lastResolve_.generation : 0;
    return captureSession_->Tick(address, generation, nowMonotonicUs);
}

bool SekiroCombatSessionController::CaptureMark(const std::string& label, std::int64_t nowMonotonicUs) {
    if (!captureSession_) {
        return false;
    }
    return captureSession_->Mark(label, nowMonotonicUs);
}

void SekiroCombatSessionController::StopCapture() {
    if (captureSession_) {
        captureSession_->Stop();
    }
}

bool SekiroCombatSessionController::IsCapturing() const {
    return captureSession_ && captureSession_->IsRunning();
}

CombatCaptureStats SekiroCombatSessionController::CaptureStats() const {
    return captureSession_ ? captureSession_->Stats() : CombatCaptureStats{};
}

std::chrono::milliseconds SekiroCombatSessionController::CaptureSamplingInterval() const {
    return captureSession_ ? captureSession_->SamplingInterval() : kDefaultCombatCaptureIntervalMs;
}

} // namespace sekiro_haptics::process
