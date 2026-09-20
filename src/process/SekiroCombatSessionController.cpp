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
    std::lock_guard<std::mutex> lock(mutex_);
    lastResolve_ = combatReader_.Resolve();
    return lastResolve_;
}

CombatSnapshot SekiroCombatSessionController::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastSnapshot_ = combatReader_.ReadSnapshot();
    lastResolve_ = combatReader_.Current();
    return lastSnapshot_;
}

CombatResolveResult SekiroCombatSessionController::LastResolve() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastResolve_;
}

CombatSnapshot SekiroCombatSessionController::LastSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSnapshot_;
}

CombatCaptureStartResult SekiroCombatSessionController::StartCapture(const CombatCaptureConfig& config,
                                                                       const std::string& outputPath,
                                                                       std::int64_t nowMonotonicUs) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (captureSession_ && captureSession_->IsRunning()) {
        return CombatCaptureStartResult::AlreadyRunning;
    }

    std::uintptr_t baseAddress = 0;
    std::uint64_t generation = 0;

    if (config.scope == CombatCaptureScope::PlayerGameData) {
        lastResolve_ = combatReader_.Refresh();
        if (lastResolve_.status != CombatSnapshotStatus::ResolvedUnvalidated || lastResolve_.playerGameDataAddress == 0) {
            return CombatCaptureStartResult::InvalidConfig;
        }
        baseAddress = lastResolve_.playerGameDataAddress;
        generation = lastResolve_.generation;
    } else { // CustomAddress
        if (config.customBaseAddress == 0) {
            return CombatCaptureStartResult::InvalidConfig;
        }
        baseAddress = config.customBaseAddress;
        generation = 1; // fixed -- a custom address is never re-resolved, so no discontinuity concept applies
    }

    SekiroCombatCaptureSession::ValidateRegion validate;
    if (config.scope == CombatCaptureScope::PlayerGameData) {
        validate = [this](std::uintptr_t address, std::uint64_t expectedGeneration) {
            lastResolve_ = combatReader_.Refresh();
            return lastResolve_.status == CombatSnapshotStatus::ResolvedUnvalidated &&
                   lastResolve_.playerGameDataAddress == address && lastResolve_.generation == expectedGeneration;
        };
    }
    auto next = std::make_unique<SekiroCombatCaptureSession>(processReader_, std::move(validate));
    const auto result = next->Start(config, baseAddress, generation, outputPath, nowMonotonicUs);
    if (result == CombatCaptureStartResult::Started) {
        activeCaptureScope_ = config.scope;
        customCaptureBaseAddress_ = config.scope == CombatCaptureScope::CustomAddress ? config.customBaseAddress : 0;
        captureSession_ = std::move(next);
    }
    return result;
}

bool SekiroCombatSessionController::CaptureTick(std::int64_t nowMonotonicUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captureSession_ || !captureSession_->IsRunning()) {
        return false;
    }
    if (!captureSession_->IsDue(nowMonotonicUs)) return true;

    std::uintptr_t address = 0;
    std::uint64_t generation = 0;
    if (activeCaptureScope_ == CombatCaptureScope::PlayerGameData) {
        lastResolve_ = combatReader_.Refresh();
        bool resolved = lastResolve_.status == CombatSnapshotStatus::ResolvedUnvalidated;
        address = resolved ? lastResolve_.playerGameDataAddress : 0;
        generation = resolved ? lastResolve_.generation : 0;
    } else { // CustomAddress -- static for the whole session's lifetime
        address = customCaptureBaseAddress_;
        generation = 1;
    }
    return captureSession_->Tick(address, generation, nowMonotonicUs);
}

bool SekiroCombatSessionController::CaptureMark(const std::string& label, std::int64_t inputTimestampUs,
                                                 std::int64_t processedTimestampUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captureSession_) {
        return false;
    }
    return captureSession_->Mark(label, inputTimestampUs, processedTimestampUs);
}

void SekiroCombatSessionController::StopCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (captureSession_) {
        captureSession_->Stop();
    }
}

bool SekiroCombatSessionController::IsCapturing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captureSession_ && captureSession_->IsRunning();
}

CombatCaptureStats SekiroCombatSessionController::CaptureStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captureSession_ ? captureSession_->Stats() : CombatCaptureStats{};
}

std::chrono::milliseconds SekiroCombatSessionController::CaptureSamplingInterval() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return captureSession_ ? captureSession_->SamplingInterval() : kDefaultCombatCaptureIntervalMs;
}

} // namespace sekiro_haptics::process
