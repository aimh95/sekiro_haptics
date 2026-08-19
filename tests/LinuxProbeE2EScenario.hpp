#pragma once

// Shared implementation of the real Linux child-process disk-backed
// signal-discovery scenario (SEK-PROBE-001C-LINUX-E2E) -- Phase 1-6 of the
// ticket, used identically by both the CTest-registered integration test
// (test_disk_candidate_scanner_linux_e2e.cpp, small arena/budget) and the
// manual E2E runner (linux_probe_e2e_main.cpp, large arena/budget). One
// implementation, not two, so the automated and manual paths can never
// silently drift apart. Reuses the real, unmodified
// SignalProbeScanController / DiskCandidateScanner / CandidateStorage /
// ScanManifest production code -- this file only supplies the real Linux
// process (LinuxHelperProcess/LinuxChildProcessReader), never a
// duplicate/parallel scanner implementation.

#include <cstdint>
#include <filesystem>
#include <string>

namespace sekiro_haptics::process {

struct LinuxProbeE2EConfig {
    std::string helperExePath;
    std::size_t arenaSizeBytes = 64ULL * 1024 * 1024;
    std::size_t memoryBudgetBytes = 8ULL * 1024 * 1024;
    std::filesystem::path outputDir;
};

/// Populated incrementally as the scenario progresses -- a caller can
/// inspect whatever fields got set even after a failure (`failureReason`
/// non-empty), for either SH_CHECK-style assertions or human-readable
/// reporting.
struct LinuxProbeE2EReport {
    bool success = false;
    std::string failureReason;
    /// Set specifically when a step failed because process_vm_readv (or
    /// the /proc/<pid>/maps read backing region validation) returned
    /// EPERM -- the ticket requires this be reported precisely rather
    /// than silently retried or worked around via sudo/Yama changes.
    bool permissionDenied = false;

    std::int64_t childPid = 0;
    std::uintptr_t arenaBase = 0;
    std::size_t arenaSizeBytes = 0;
    std::uintptr_t expectedHpAddress = 0;

    bool handshakeOk = false;
    bool procMapsValidationOk = false;
    bool directCrossProcessReadOk = false;
    std::uint32_t directReadHpValue = 0;

    bool planOk = false;
    bool diskBackedRecommended = false;

    bool baselineCompleteCoverage = false;
    std::uint64_t baselineProcessedBytes = 0;
    double baselineCoveragePercent = 0.0;
    std::uintmax_t baselineFileSizeBytes = 0;
    std::size_t baselinePeakBufferedBytes = 0;
    std::size_t configuredMemoryBudgetBytes = 0;

    bool firstDecreasedOk = false;
    std::size_t firstDecreasedCandidateCount = 0;

    bool unchangedOk = false;
    std::size_t unchangedCandidateCount = 0;

    bool finalIncreasedOk = false;
    std::size_t finalIncreasedCandidateCount = 0;
    std::uintptr_t finalHpAddress = 0;
    std::uint32_t finalHpValue = 0;
    bool finalAddressMatches = false;

    bool processExitedContractOk = false;
    /// The IProcessReader-level contract: after Detach(), IsAttached() is
    /// false and a direct ReadBytes() call reports NotAttached. Checked
    /// directly against the reader, not by repeating controller.Filter()
    /// -- once one filter attempt has already failed and marked the
    /// on-disk manifest Interrupted, a second attempt correctly refuses to
    /// retry the session at all (CorruptFile, see
    /// secondFilterAfterInterruptedRejectedOk) without ever reaching the
    /// reader again, so that path cannot exercise this contract.
    bool notAttachedContractOk = false;
    /// A second filter attempt against a manifest already marked
    /// Interrupted by the first failed attempt must be rejected outright
    /// (never silently retried as if the session were still healthy) --
    /// this is the same guard ResumeDiskCandidateSession() also enforces.
    bool secondFilterAfterInterruptedRejectedOk = false;
    bool lastGenerationFileIntact = false;
};

/// Runs the full Phase 1-6 scenario against a real, freshly-forked helper
/// process. Never throws -- any failure is captured into
/// `report.failureReason` (and `report.permissionDenied` when applicable)
/// with `report.success == false`; whatever fields were already populated
/// before the failure remain valid for diagnostics. `config.outputDir`
/// must already exist (same convention as SignalProbeControllerConfig)
/// and is used as the disk-backed session directory -- the caller decides
/// whether to clean it up afterward (the CTest version always does; the
/// manual runner only does unless --keep-output is passed).
LinuxProbeE2EReport RunLinuxProbeE2EScenario(const LinuxProbeE2EConfig& config);

} // namespace sekiro_haptics::process
