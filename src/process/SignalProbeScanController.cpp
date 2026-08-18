#include "sekiro_haptics/process/SignalProbeScanController.hpp"

#include <utility>

namespace sekiro_haptics::process {

const char* ToString(ScanMode mode) {
    switch (mode) {
        case ScanMode::None:
            return "none";
        case ScanMode::InMemory:
            return "in-memory";
        case ScanMode::Disk:
            return "disk";
    }
    return "unknown";
}

const char* ToString(BeginCommandOutcome outcome) {
    switch (outcome) {
        case BeginCommandOutcome::Success:
            return "Success";
        case BeginCommandOutcome::InMemoryBudgetExceeded:
            return "InMemoryBudgetExceeded";
        case BeginCommandOutcome::ScanFailed:
            return "ScanFailed";
    }
    return "Unknown";
}

const char* ToString(FilterCommandOutcome outcome) {
    switch (outcome) {
        case FilterCommandOutcome::Success:
            return "Success";
        case FilterCommandOutcome::NoActiveScan:
            return "NoActiveScan";
        case FilterCommandOutcome::InvalidCommand:
            return "InvalidCommand";
        case FilterCommandOutcome::InMemoryFilterFailed:
            return "InMemoryFilterFailed";
        case FilterCommandOutcome::DiskFilterFailed:
            return "DiskFilterFailed";
    }
    return "Unknown";
}

SignalProbeScanController::SignalProbeScanController(IProcessReader& reader, IProcessInspector& inspector,
                                                       const IProcessMemoryMap& memoryMap,
                                                       SignalProbeControllerConfig config, ScanControllerIdentity identity)
    : reader_(reader),
      inspector_(inspector),
      memoryMap_(memoryMap),
      config_(std::move(config)),
      identity_(std::move(identity)) {}

void SignalProbeScanController::ClearInMemorySession() {
    candidates_.clear();
    inMemoryType_.reset();
    inMemoryScope_.reset();
}

void SignalProbeScanController::ClearDiskSession() {
    diskManifest_.reset();
}

ScanSessionIdentity SignalProbeScanController::BuildScanSessionIdentity(CandidateValueType type,
                                                                         CandidateScanScope scope) const {
    ScanSessionIdentity identity;
    identity.executableFileSizeBytes = identity_.executableFileSizeBytes;
    identity.sha256Hex = identity_.sha256Hex;
    identity.pid = identity_.pid;
    identity.mainModuleBaseAddress = identity_.mainModuleBaseAddress;
    identity.mainModuleImageSize = identity_.mainModuleImageSize;
    identity.valueType = ToString(type);
    identity.scope = ToString(scope);
    return identity;
}

PlanCommandResult SignalProbeScanController::Plan(CandidateValueType type, CandidateScanScope scope) {
    PlanCommandResult result;
    result.outcome = PlanCandidateScan(reader_, inspector_, memoryMap_, scope, type, config_.outputDir,
                                        config_.memoryBudgetBytes, result.report);
    return result;
}

BeginCommandResult SignalProbeScanController::Begin(CandidateValueType type, CandidateScanScope scope) {
    BeginCommandResult result;

    // Resolved independently of BeginCandidateScan()'s own internal
    // enumeration below -- a second, cheap metadata-only
    // EnumerateReadableRegions() call, not a second memory read. This
    // mirrors PlanCandidateScan()'s own independent resolution; neither
    // function's signature exposes a way to share a prior resolution, and
    // changing that is out of scope here.
    std::vector<ProcessMemoryRegion> allRegions;
    MemoryMapResult mapResult = memoryMap_.EnumerateReadableRegions(allRegions);
    if (mapResult != MemoryMapResult::Success) {
        result.outcome = BeginCommandOutcome::ScanFailed;
        if (mapResult == MemoryMapResult::NotAttached) {
            result.scanResult = CandidateScanResult::NotAttached;
        } else if (mapResult == MemoryMapResult::ProcessExited) {
            result.scanResult = CandidateScanResult::ProcessExited;
        } else {
            result.scanResult = CandidateScanResult::MemoryMapUnavailable;
        }
        return result;
    }

    std::vector<ProcessMemoryRegion> targetRegions;
    CandidateScanResult scopeResult = ResolveScanScopeRegions(inspector_, allRegions, scope, targetRegions);
    if (scopeResult != CandidateScanResult::Success) {
        result.outcome = BeginCommandOutcome::ScanFailed;
        result.scanResult = scopeResult;
        return result;
    }

    CandidateScanPlanTotals totals = ComputeCandidateScanPlanTotals(targetRegions, type);
    if (CheckInMemoryScanBudget(totals, config_.memoryBudgetBytes) == InMemoryBudgetCheckResult::InMemoryBudgetExceeded) {
        result.outcome = BeginCommandOutcome::InMemoryBudgetExceeded;
        result.totals = totals;
        return result;
    }

    std::vector<Candidate> newCandidates;
    CandidateScanResult scanResult = BeginCandidateScan(reader_, inspector_, memoryMap_, scope, type, newCandidates,
                                                         config_.maxCandidates, config_.maxScanBytes);
    if (scanResult != CandidateScanResult::Success) {
        // Matches the original CLI exactly: a failed begin leaves whatever
        // was active before (in-memory or disk) completely untouched.
        result.outcome = BeginCommandOutcome::ScanFailed;
        result.scanResult = scanResult;
        return result;
    }

    ClearDiskSession();
    candidates_ = std::move(newCandidates);
    inMemoryType_ = type;
    inMemoryScope_ = scope;
    mode_ = ScanMode::InMemory;

    result.outcome = BeginCommandOutcome::Success;
    result.scanResult = CandidateScanResult::Success;
    result.candidateCount = candidates_.size();
    return result;
}

BeginDiskCommandResult SignalProbeScanController::BeginDisk(CandidateValueType type, CandidateScanScope scope) {
    BeginDiskCommandResult result;

    ScanPlanReport report;
    result.planOutcome =
        PlanCandidateScan(reader_, inspector_, memoryMap_, scope, type, config_.outputDir, config_.memoryBudgetBytes, report);
    if (result.planOutcome != PlanCandidateScanOutcome::Success) {
        result.ranScan = false;
        return result;
    }

    result.ranScan = true;
    ScanSessionIdentity identity = BuildScanSessionIdentity(type, scope);

    DiskScanStats stats;
    DiskScanOutcome outcome = BeginDiskCandidateScan(reader_, inspector_, memoryMap_, scope, type, config_.outputDir,
                                                      identity, config_.memoryBudgetBytes, stats);
    result.scanOutcome = outcome;
    result.stats = stats;
    lastDiskStats_ = stats;

    if (outcome != DiskScanOutcome::CompleteCoverage) {
        return result;
    }

    ScanManifest manifest;
    std::string readError;
    if (ReadScanManifest(config_.outputDir / "scan-manifest.json", manifest, readError)) {
        ClearInMemorySession();
        diskManifest_ = manifest;
        mode_ = ScanMode::Disk;
    }
    // If the manifest somehow can't be read back despite CompleteCoverage
    // (BeginDiskCandidateScan() itself just wrote and self-verified it),
    // mode_ is left unchanged rather than claiming an active session this
    // controller can't actually describe.

    return result;
}

FilterCommandResult SignalProbeScanController::Filter(CandidateFilterKind kind, const CandidateValue* exactTarget) {
    FilterCommandResult result;

    if (kind == CandidateFilterKind::ExactValue && exactTarget == nullptr) {
        result.outcome = FilterCommandOutcome::InvalidCommand;
        return result;
    }

    if (mode_ == ScanMode::None) {
        result.outcome = FilterCommandOutcome::NoActiveScan;
        return result;
    }

    if (mode_ == ScanMode::InMemory) {
        CandidateFilterResult filterResult = FilterCandidates(reader_, candidates_, kind, exactTarget);
        result.inMemoryResult = filterResult;
        if (filterResult != CandidateFilterResult::Success) {
            result.outcome = FilterCommandOutcome::InMemoryFilterFailed;
            return result;
        }
        result.outcome = FilterCommandOutcome::Success;
        result.inMemoryRemainingCount = candidates_.size();
        return result;
    }

    // Disk mode.
    DiskScanStats stats;
    DiskScanOutcome diskOutcome =
        FilterDiskCandidates(reader_, config_.outputDir, kind, exactTarget, config_.memoryBudgetBytes, stats);
    result.diskResult = diskOutcome;
    result.diskStats = stats;
    lastDiskStats_ = stats;

    // Re-read the manifest regardless of outcome, so diskManifest_ always
    // reflects the truth on disk rather than stale in-memory bookkeeping
    // (FilterDiskCandidates() itself already wrote whatever the real
    // outcome was, success or failure).
    ScanManifest refreshedManifest;
    std::string readError;
    if (ReadScanManifest(config_.outputDir / "scan-manifest.json", refreshedManifest, readError)) {
        diskManifest_ = refreshedManifest;
    }

    if (diskOutcome != DiskScanOutcome::CompleteCoverage) {
        result.outcome = FilterCommandOutcome::DiskFilterFailed;
        return result;
    }
    result.outcome = FilterCommandOutcome::Success;
    return result;
}

ResumeCommandResult SignalProbeScanController::Resume() {
    ResumeCommandResult result;

    ScanManifest peeked;
    std::string peekError;
    if (!ReadScanManifest(config_.outputDir / "scan-manifest.json", peeked, peekError)) {
        result.outcome = DiskScanOutcome::CorruptFile;
        return result;
    }

    // `resume` takes no type/scope arguments -- there is nothing external
    // to compare those two identity fields against, so this deliberately
    // compares the manifest's own recorded values against themselves (a
    // structural no-op for exactly those two fields). The real protection
    // this call provides is the 5 stable fields below, plus process
    // liveness, plus the data file's own validation -- all still fully
    // enforced by ResumeDiskCandidateSession(), unmodified.
    ScanSessionIdentity currentIdentity;
    currentIdentity.executableFileSizeBytes = identity_.executableFileSizeBytes;
    currentIdentity.sha256Hex = identity_.sha256Hex;
    currentIdentity.pid = identity_.pid;
    currentIdentity.mainModuleBaseAddress = identity_.mainModuleBaseAddress;
    currentIdentity.mainModuleImageSize = identity_.mainModuleImageSize;
    currentIdentity.valueType = peeked.identity.valueType;
    currentIdentity.scope = peeked.identity.scope;

    ScanManifest manifest;
    DiskScanOutcome outcome = ResumeDiskCandidateSession(config_.outputDir, currentIdentity, reader_, manifest);
    result.outcome = outcome;

    if (outcome == DiskScanOutcome::CompleteCoverage) {
        result.manifest = manifest;
        ClearInMemorySession();
        diskManifest_ = manifest;
        mode_ = ScanMode::Disk;
    }

    return result;
}

StatusSnapshot SignalProbeScanController::Status() const {
    StatusSnapshot snapshot;
    snapshot.mode = mode_;
    snapshot.inMemoryType = inMemoryType_;
    snapshot.inMemoryScope = inMemoryScope_;
    snapshot.inMemoryCandidateCount = candidates_.size();
    snapshot.diskManifest = diskManifest_;
    snapshot.lastDiskStats = lastDiskStats_;
    return snapshot;
}

std::vector<ListEntry> SignalProbeScanController::List(std::size_t count) const {
    std::vector<ListEntry> entries;
    if (count == 0) {
        return entries;
    }

    if (mode_ == ScanMode::InMemory) {
        for (const Candidate& c : candidates_) {
            if (entries.size() >= count) {
                break;
            }
            entries.push_back(ListEntry{c.address, c.type, c.value});
        }
        return entries;
    }

    if (mode_ != ScanMode::Disk || !diskManifest_.has_value()) {
        return entries;
    }

    CandidateValueType type;
    if (!ParseCandidateValueType(diskManifest_->identity.valueType, type)) {
        return entries;
    }
    std::size_t valueSize = CandidateValueTypeSize(type);

    if (diskManifest_->generation == 0) {
        // Streams from the file: never loads more than `count` values
        // regardless of the baseline's real size.
        BaselineReader reader(config_.outputDir / "baseline.bin");
        if (reader.Open(diskManifest_->totalValueCount, type) != CandidateStorageResult::Success) {
            return entries;
        }
        ProcessMemoryRegion region;
        std::uint64_t regionValueCount = 0;
        std::vector<std::uint8_t> buffer(valueSize);
        while (entries.size() < count && reader.NextRegion(region, regionValueCount)) {
            std::size_t addressRemainder = region.baseAddress % valueSize;
            std::size_t alignedStart = (addressRemainder == 0) ? 0 : (valueSize - addressRemainder);
            for (std::uint64_t i = 0; i < regionValueCount && entries.size() < count; ++i) {
                std::size_t got = reader.ReadRegionValueChunk(i, 1, buffer.data());
                if (got != 1) {
                    break;
                }
                ListEntry entry;
                entry.address = region.baseAddress + alignedStart + i * valueSize;
                entry.type = type;
                entry.value = DecodeCandidateValue(type, buffer.data());
                entries.push_back(entry);
            }
        }
    } else {
        GenerationReader reader(config_.outputDir / CurrentDataFileName(diskManifest_->generation));
        if (reader.Open(diskManifest_->candidateCount, type, diskManifest_->generation) != CandidateStorageResult::Success) {
            return entries;
        }
        std::uint64_t address = 0;
        std::vector<std::uint8_t> buffer(valueSize);
        while (entries.size() < count && reader.NextRecord(address, buffer.data())) {
            ListEntry entry;
            entry.address = address;
            entry.type = type;
            entry.value = DecodeCandidateValue(type, buffer.data());
            entries.push_back(entry);
        }
    }

    return entries;
}

ScanMode SignalProbeScanController::Mode() const {
    return mode_;
}

std::optional<CandidateValueType> SignalProbeScanController::CurrentValueType() const {
    if (mode_ == ScanMode::InMemory) {
        return inMemoryType_;
    }
    if (mode_ == ScanMode::Disk && diskManifest_.has_value()) {
        CandidateValueType type;
        if (ParseCandidateValueType(diskManifest_->identity.valueType, type)) {
            return type;
        }
    }
    return std::nullopt;
}

} // namespace sekiro_haptics::process
