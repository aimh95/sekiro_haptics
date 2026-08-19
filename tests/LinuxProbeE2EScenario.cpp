#include "LinuxProbeE2EScenario.hpp"

#include "LinuxChildProcessReader.hpp"
#include "LinuxHelperProcess.hpp"

#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "sekiro_haptics/process/SignalProbeScanController.hpp"

#include <cerrno>
#include <sstream>
#include <unistd.h>

namespace sekiro_haptics::process {

namespace {

ScanControllerIdentity MakeSyntheticTestIdentity(pid_t pid, std::uintptr_t arenaBase, std::size_t arenaSize) {
    ScanControllerIdentity id;
    // Deliberately obvious, unmistakable synthetic values -- never
    // confusable with a real executable's build identity. arenaBase/
    // arenaSize stand in for "main module base/size" only because
    // ScanControllerIdentity has no separate "arena" concept; this
    // scenario never uses CandidateScanScope::MainModule, so these two
    // fields are never actually matched against real module metadata.
    id.executableFileSizeBytes = 0;
    id.sha256Hex = "linux-e2e-test-only-not-a-real-build-identity";
    id.pid = static_cast<std::uint32_t>(pid);
    id.mainModuleBaseAddress = arenaBase;
    id.mainModuleImageSize = arenaSize;
    return id;
}

} // namespace

LinuxProbeE2EReport RunLinuxProbeE2EScenario(const LinuxProbeE2EConfig& config) {
    LinuxProbeE2EReport report;
    report.arenaSizeBytes = config.arenaSizeBytes;
    report.configuredMemoryBudgetBytes = config.memoryBudgetBytes;

    std::error_code createEc;
    std::filesystem::create_directories(config.outputDir, createEc);
    if (createEc) {
        report.failureReason = "could not create output directory: " + createEc.message();
        return report;
    }

    // --- Phase 1: helper launch, handshake, direct real cross-process read ---

    LinuxHelperProcess helper;
    if (!helper.Start(config.helperExePath, {std::to_string(config.arenaSizeBytes)})) {
        report.failureReason = "failed to fork/exec the Linux signal-probe helper process";
        return report;
    }

    std::string readyLine;
    if (!helper.ReadLine(readyLine) || readyLine.rfind("READY ", 0) != 0) {
        report.failureReason = "malformed or missing READY handshake line from helper: \"" + readyLine + "\"";
        return report;
    }
    report.handshakeOk = true;

    pid_t childPid = 0;
    std::uintptr_t arenaBase = 0;
    std::size_t arenaSize = 0;
    std::uintptr_t hpAddr = 0;

    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, arenaBaseTok, arenaSizeTok, hpTok, decoyTok, stableTok, noiseTok;
        iss >> tag >> pidTok >> arenaBaseTok >> arenaSizeTok >> hpTok >> decoyTok >> stableTok >> noiseTok;
        if (pidTok.empty() || arenaBaseTok.empty() || arenaSizeTok.empty() || hpTok.empty()) {
            report.failureReason = "READY line missing expected fields: \"" + readyLine + "\"";
            return report;
        }
        childPid = static_cast<pid_t>(ParseLinuxHelperKeyValue(pidTok));
        arenaBase = static_cast<std::uintptr_t>(ParseLinuxHelperKeyValue(arenaBaseTok));
        arenaSize = static_cast<std::size_t>(ParseLinuxHelperKeyValue(arenaSizeTok));
        hpAddr = static_cast<std::uintptr_t>(ParseLinuxHelperKeyValue(hpTok));
    }

    report.childPid = childPid;
    report.arenaBase = arenaBase;
    report.arenaSizeBytes = arenaSize;
    report.expectedHpAddress = hpAddr;

    if (childPid == getpid()) {
        report.failureReason = "helper PID equals parent PID -- this should never happen after a real fork()";
        return report;
    }
    if (helper.Pid() != childPid) {
        report.failureReason = "helper-reported PID does not match the PID fork() actually returned";
        return report;
    }

    LinuxChildProcessReader reader(childPid, arenaBase, arenaSize);
    if (reader.AttachByPid(static_cast<std::uint32_t>(childPid)) != ProcessReaderResult::Success) {
        report.failureReason = "AttachByPid() to the just-forked helper child failed unexpectedly";
        return report;
    }

    std::vector<ProcessMemoryRegion> arenaRegions;
    MemoryMapResult mapResult = reader.EnumerateReadableRegions(arenaRegions);
    if (mapResult != MemoryMapResult::Success || arenaRegions.size() != 1 || arenaRegions[0].baseAddress != arenaBase ||
        arenaRegions[0].sizeBytes != arenaSize) {
        report.failureReason = "/proc/<pid>/maps arena validation failed: " + std::string(ToString(mapResult));
        return report;
    }
    report.procMapsValidationOk = true;

    std::uint32_t directHpValue = 0;
    ProcessReaderResult directReadResult = reader.ReadBytes(hpAddr, &directHpValue, sizeof(directHpValue));
    if (directReadResult != ProcessReaderResult::Success) {
        if (reader.LastErrno() == EPERM) {
            report.permissionDenied = true;
            report.failureReason =
                "process_vm_readv failed with EPERM reading the helper's own known-valid arena address -- "
                "stopping without attempting sudo or Yama ptrace_scope changes, as required.";
            return report;
        }
        report.failureReason = "direct process_vm_readv read of playerHp failed: " + std::string(ToString(directReadResult));
        return report;
    }
    if (directHpValue != 1000) {
        report.failureReason = "direct read of playerHp returned " + std::to_string(directHpValue) + ", expected 1000";
        return report;
    }
    report.directCrossProcessReadOk = true;
    report.directReadHpValue = directHpValue;

    // --- Phase 2: plan + disk baseline ---

    SignalProbeControllerConfig controllerConfig;
    controllerConfig.outputDir = config.outputDir;
    controllerConfig.memoryBudgetBytes = config.memoryBudgetBytes;

    SignalProbeScanController controller(reader, reader, reader, controllerConfig,
                                          MakeSyntheticTestIdentity(childPid, arenaBase, arenaSize));

    PlanCommandResult planResult = controller.Plan(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    if (planResult.outcome != PlanCandidateScanOutcome::Success) {
        report.failureReason = "PlanCandidateScan failed: " + std::string(ToString(planResult.outcome));
        return report;
    }
    report.planOk = true;
    report.diskBackedRecommended = planResult.report.recommendedStorageMode == RecommendedStorageMode::DiskBacked;
    if (!report.diskBackedRecommended) {
        report.failureReason = "test misconfiguration: memory budget was not small enough to force the disk-backed "
                                "path for this arena size";
        return report;
    }

    BeginDiskCommandResult beginDiskResult =
        controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    if (!beginDiskResult.ranScan || beginDiskResult.scanOutcome != DiskScanOutcome::CompleteCoverage) {
        report.failureReason = "BeginDiskCandidateScan did not reach CompleteCoverage: ranScan=" +
                                std::to_string(beginDiskResult.ranScan) +
                                " scanOutcome=" + std::string(ToString(beginDiskResult.scanOutcome));
        return report;
    }
    report.baselineCompleteCoverage = true;
    report.baselineProcessedBytes = beginDiskResult.stats.processedBytes;
    report.baselineCoveragePercent = beginDiskResult.stats.coveragePercent;
    report.baselinePeakBufferedBytes = beginDiskResult.stats.peakBufferedBytes;

    std::error_code sizeEc;
    report.baselineFileSizeBytes = std::filesystem::file_size(config.outputDir / "baseline.bin", sizeEc);
    if (sizeEc) {
        report.failureReason = "baseline.bin missing or unreadable after CompleteCoverage: " + sizeEc.message();
        return report;
    }
    if (report.baselinePeakBufferedBytes > config.memoryBudgetBytes) {
        report.failureReason = "peakBufferedBytes exceeded the configured memory budget";
        return report;
    }

    // --- Phase 3: first "decreased" filter, real DAMAGE command ---

    if (!helper.SendCommandAndWaitForAck("DAMAGE")) {
        report.failureReason = "helper did not ACK DAMAGE";
        return report;
    }

    FilterCommandResult decreasedResult = controller.Filter(CandidateFilterKind::Decreased, nullptr);
    if (decreasedResult.outcome != FilterCommandOutcome::Success) {
        report.failureReason = "first \"decreased\" filter failed: outcome=" +
                                std::string(ToString(decreasedResult.outcome)) +
                                " diskResult=" + std::string(ToString(decreasedResult.diskResult));
        return report;
    }
    if (decreasedResult.diskStats.regionsProcessed != decreasedResult.diskStats.regionsTotal) {
        report.failureReason = "first filter did not process the whole baseline sequentially";
        return report;
    }
    report.firstDecreasedOk = true;
    report.firstDecreasedCandidateCount = decreasedResult.diskStats.survivingCandidateCount;
    if (report.firstDecreasedCandidateCount != 2) {
        report.failureReason = "expected exactly 2 survivors (hp + decoy) after the first decreased filter, got " +
                                std::to_string(report.firstDecreasedCandidateCount);
        return report;
    }

    // --- Phase 4: negative control -- NOISE_ONLY + "unchanged" ---

    if (!helper.SendCommandAndWaitForAck("NOISE_ONLY")) {
        report.failureReason = "helper did not ACK NOISE_ONLY";
        return report;
    }

    FilterCommandResult unchangedResult = controller.Filter(CandidateFilterKind::Unchanged, nullptr);
    if (unchangedResult.outcome != FilterCommandOutcome::Success) {
        report.failureReason = "negative-control \"unchanged\" filter failed: outcome=" +
                                std::string(ToString(unchangedResult.outcome)) +
                                " diskResult=" + std::string(ToString(unchangedResult.diskResult));
        return report;
    }
    report.unchangedOk = true;
    report.unchangedCandidateCount = unchangedResult.diskStats.survivingCandidateCount;
    if (report.unchangedCandidateCount != 1) {
        report.failureReason =
            "expected exactly 1 survivor (hp; decoy must be removed) after the negative-control unchanged filter, "
            "got " +
            std::to_string(report.unchangedCandidateCount);
        return report;
    }

    StatusSnapshot afterUnchangedStatus = controller.Status();
    if (!afterUnchangedStatus.diskManifest.has_value() ||
        !std::filesystem::exists(config.outputDir / CurrentDataFileName(afterUnchangedStatus.diskManifest->generation))) {
        report.failureReason = "generation file for the negative-control filter was not published";
        return report;
    }

    // --- Phase 5: heal + final "increased" ---

    if (!helper.SendCommandAndWaitForAck("HEAL")) {
        report.failureReason = "helper did not ACK HEAL";
        return report;
    }

    FilterCommandResult increasedResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    if (increasedResult.outcome != FilterCommandOutcome::Success) {
        report.failureReason = "final \"increased\" filter failed: outcome=" +
                                std::string(ToString(increasedResult.outcome)) +
                                " diskResult=" + std::string(ToString(increasedResult.diskResult));
        return report;
    }
    report.finalIncreasedOk = true;
    report.finalIncreasedCandidateCount = increasedResult.diskStats.survivingCandidateCount;

    std::vector<ListEntry> finalEntries = controller.List(10);
    if (finalEntries.size() != 1) {
        report.failureReason =
            "expected exactly 1 final surviving candidate, got " + std::to_string(finalEntries.size());
        return report;
    }
    report.finalHpAddress = finalEntries[0].address;
    report.finalHpValue = std::get<std::uint32_t>(finalEntries[0].value);
    report.finalAddressMatches = report.finalHpAddress == hpAddr && report.finalHpValue == 950;
    if (!report.finalAddressMatches) {
        report.failureReason = "final surviving candidate did not match the expected HP address/value";
        return report;
    }

    int lastGoodGeneration = 0;
    if (controller.Status().diskManifest.has_value()) {
        lastGoodGeneration = controller.Status().diskManifest->generation;
    }

    // --- Phase 6: exit/detach failure contract ---

    helper.SendExit();
    if (!helper.WaitForExit()) {
        report.failureReason = "helper did not exit after EXIT was sent";
        return report;
    }

    FilterCommandResult afterExitResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    report.processExitedContractOk =
        afterExitResult.outcome == FilterCommandOutcome::DiskFilterFailed && afterExitResult.diskResult == DiskScanOutcome::ProcessExited;
    if (!report.processExitedContractOk) {
        report.failureReason = "post-exit filter did not report DiskScanOutcome::ProcessExited";
        return report;
    }

    reader.Detach();
    report.notAttachedContractOk = !reader.IsAttached();
    if (report.notAttachedContractOk) {
        std::uint32_t discard = 0;
        report.notAttachedContractOk =
            reader.ReadBytes(hpAddr, &discard, sizeof(discard)) == ProcessReaderResult::NotAttached;
    }
    if (!report.notAttachedContractOk) {
        report.failureReason = "reader did not report NotAttached after Detach()";
        return report;
    }

    // The prior failed filter attempt already marked the on-disk manifest
    // Interrupted -- a second attempt must be rejected outright rather
    // than silently retried, so this deliberately does NOT re-exercise
    // the NotAttached reader path (see the field's doc comment).
    FilterCommandResult afterDetachResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    report.secondFilterAfterInterruptedRejectedOk =
        afterDetachResult.outcome == FilterCommandOutcome::DiskFilterFailed &&
        afterDetachResult.diskResult != DiskScanOutcome::CompleteCoverage;
    if (!report.secondFilterAfterInterruptedRejectedOk) {
        report.failureReason = "a second filter attempt against an already-Interrupted session was not rejected";
        return report;
    }

    std::string currentFileName = CurrentDataFileName(lastGoodGeneration);
    std::error_code existsEc;
    bool currentExists = std::filesystem::exists(config.outputDir / currentFileName, existsEc);
    bool tmpLingers = std::filesystem::exists(config.outputDir / (currentFileName + ".tmp"), existsEc);
    if (!currentExists || tmpLingers) {
        report.failureReason = "last known-good generation file was not intact after the failed post-exit/post-detach "
                                "filter attempts";
        return report;
    }
    GenerationReader directReader(config.outputDir / currentFileName);
    if (directReader.Open(1, CandidateValueType::U32, lastGoodGeneration) != CandidateStorageResult::Success) {
        report.failureReason = "last known-good generation file failed direct re-validation";
        return report;
    }
    std::uint64_t verifiedAddress = 0;
    std::uint8_t verifiedValueBytes[4] = {};
    if (!directReader.NextRecord(verifiedAddress, verifiedValueBytes) || verifiedAddress != hpAddr) {
        report.failureReason = "last known-good generation file's sole record did not match the expected HP address";
        return report;
    }
    report.lastGenerationFileIntact = true;

    report.success = true;
    return report;
}

} // namespace sekiro_haptics::process
