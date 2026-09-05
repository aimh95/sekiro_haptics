#pragma once

// OS-independent scan-session controller for the signal-discovery probe
// CLI (SEK-PROBE-001C-2). Owns "which scan is active and what state is it
// in" and every operation the CLI's plan/begin/begin-disk/filter/resume/
// status/list commands need -- but knows nothing about console I/O,
// command-line parsing, or Win32. Only depends on IProcessReader/
// IProcessInspector/IProcessMemoryMap, CandidateScanner.hpp,
// DiskCandidateScanner.hpp, and std::filesystem, so it builds and is
// testable (with Fakes) on any OS -- apps/sekiro_signal_probe/main.cpp is
// the thin Win32 adapter that constructs a real Win32ProcessReader and
// hands it to this class.
//
// identity/regions/watch/mark/stop/quit stay in main.cpp entirely --
// this controller has no opinion about them.

#include "sekiro_haptics/process/CandidateScanShared.hpp"
#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Which kind of scan session (if any) is currently active. Exactly one of
/// InMemory/Disk's state is populated at a time -- starting one clears the
/// other, so there is never ambiguity about which the next `filter`/
/// `status`/`list` should act on.
enum class ScanMode {
    None,
    InMemory,
    Disk,
};
const char* ToString(ScanMode mode);

/// The identity fields stable for the whole probe session (until the
/// process is re-attached) -- as opposed to ScanSessionIdentity
/// (ScanManifest.hpp), which also carries the value type/scope of one
/// particular scan. Kept separate so the controller isn't forced to guess
/// a type/scope before the user has chosen one via `begin-disk`.
struct ScanControllerIdentity {
    std::uint64_t executableFileSizeBytes = 0;
    std::string sha256Hex;
    std::uint32_t pid = 0;
    std::uintptr_t mainModuleBaseAddress = 0;
    std::size_t mainModuleImageSize = 0;
};

struct SignalProbeControllerConfig {
    std::filesystem::path outputDir;
    std::size_t memoryBudgetBytes = kDefaultDiskScanMemoryBudgetBytes;
    /// Legacy in-memory secondary caps -- applied only *after*
    /// CheckInMemoryScanBudget() has already passed; cannot be used to
    /// bypass the budget check.
    std::size_t maxCandidates = kDefaultMaxCandidates;
    std::uint64_t maxScanBytes = kDefaultMaxScanBytes;
};

struct PlanCommandResult {
    PlanCandidateScanOutcome outcome = PlanCandidateScanOutcome::Success;
    ScanPlanReport report;
};

enum class BeginCommandOutcome {
    Success,
    /// The in-memory scan was refused before any candidate allocation --
    /// see `totals` for the estimate that triggered it.
    InMemoryBudgetExceeded,
    /// The budget check passed but BeginCandidateScan() itself failed --
    /// see `scanResult`.
    ScanFailed,
};
const char* ToString(BeginCommandOutcome outcome);

struct BeginCommandResult {
    BeginCommandOutcome outcome = BeginCommandOutcome::ScanFailed;
    /// Valid when outcome is Success or ScanFailed.
    CandidateScanResult scanResult = CandidateScanResult::Success;
    /// Valid when outcome is InMemoryBudgetExceeded (the estimate that was
    /// rejected -- lets the caller report "needed N bytes, budget is M").
    CandidateScanPlanTotals totals;
    /// Valid when outcome is Success.
    std::size_t candidateCount = 0;
};

struct BeginDiskCommandResult {
    /// The PlanCandidateScan() preflight this always runs first.
    PlanCandidateScanOutcome planOutcome = PlanCandidateScanOutcome::Success;
    /// Only meaningful if planOutcome == Success and ranScan is true.
    DiskScanOutcome scanOutcome = DiskScanOutcome::CompleteCoverage;
    DiskScanStats stats;
    /// False if the preflight itself failed (e.g. InsufficientDiskSpace) --
    /// nothing touched disk and BeginDiskCandidateScan() was never called.
    bool ranScan = false;
};

enum class FilterCommandOutcome {
    Success,
    /// Mode() == None -- neither `begin` nor `begin-disk` has succeeded yet.
    NoActiveScan,
    /// Malformed filter command itself (unknown kind text, "exact" with no
    /// value, or a value that doesn't parse for the current type) --
    /// caught before either underlying filter is even attempted.
    InvalidCommand,
    /// See `inMemoryResult`.
    InMemoryFilterFailed,
    /// See `diskResult` (also covers InitialFilterTooBroad/InvalidTarget).
    DiskFilterFailed,
};
const char* ToString(FilterCommandOutcome outcome);

struct FilterCommandResult {
    FilterCommandOutcome outcome = FilterCommandOutcome::NoActiveScan;
    /// Valid when the in-memory path ran (Success or InMemoryFilterFailed).
    CandidateFilterResult inMemoryResult = CandidateFilterResult::Success;
    /// Valid when the disk path ran (Success or DiskFilterFailed).
    DiskScanOutcome diskResult = DiskScanOutcome::CompleteCoverage;
    /// Valid when the in-memory path succeeded.
    std::size_t inMemoryRemainingCount = 0;
    /// Valid when the disk path ran at all.
    DiskScanStats diskStats;
};

struct ResumeCommandResult {
    DiskScanOutcome outcome = DiskScanOutcome::CompleteCoverage;
    /// Valid on Success (== DiskScanOutcome::CompleteCoverage).
    ScanManifest manifest;
};

struct StatusSnapshot {
    ScanMode mode = ScanMode::None;

    std::optional<CandidateValueType> inMemoryType;
    std::optional<CandidateScanScope> inMemoryScope;
    std::size_t inMemoryCandidateCount = 0;

    /// Populated when mode == Disk.
    std::optional<ScanManifest> diskManifest;
    /// Stats from the most recent BeginDisk()/Filter() call -- carries
    /// peakBufferedBytes/droppedCandidateCount, which ScanManifest itself
    /// doesn't record.
    DiskScanStats lastDiskStats;
};

struct ListEntry {
    std::uintptr_t address = 0;
    CandidateValueType type = CandidateValueType::U32;
    CandidateValue value{std::uint32_t{0}};
};

/// Owns one probe session's scan state and every plan/begin/begin-disk/
/// filter/resume/status/list operation. Not copyable (holds reference
/// members to the process-access interfaces it was constructed with).
class SignalProbeScanController {
public:
    /// `reader`/`inspector`/`memoryMap` must outlive this controller (the
    /// same convention IProcessReader-consuming code already uses
    /// throughout this project).
    SignalProbeScanController(IProcessReader& reader, IProcessInspector& inspector, const IProcessMemoryMap& memoryMap,
                               SignalProbeControllerConfig config, ScanControllerIdentity identity);

    SignalProbeScanController(const SignalProbeScanController&) = delete;
    SignalProbeScanController& operator=(const SignalProbeScanController&) = delete;

    /// Computes estimates only -- never scans, never creates a session
    /// directory or any file.
    PlanCommandResult Plan(CandidateValueType type, CandidateScanScope scope);

    /// The legacy in-memory path, gated by CheckInMemoryScanBudget()
    /// before any allocation. On Success, switches Mode() to InMemory and
    /// clears any active disk session. On any failure, current state
    /// (whichever mode was active before, if any) is left untouched --
    /// matches the original CLI's exact behavior.
    BeginCommandResult Begin(CandidateValueType type, CandidateScanScope scope);

    /// Runs PlanCandidateScan() first; only calls BeginDiskCandidateScan()
    /// if that preflight succeeds. Only on DiskScanOutcome::CompleteCoverage
    /// does Mode() switch to Disk (clearing any in-memory session) --
    /// otherwise current state is untouched.
    ///
    /// `onProgress`, if set, is forwarded as-is to BeginDiskCandidateScan().
    BeginDiskCommandResult BeginDisk(CandidateValueType type, CandidateScanScope scope,
                                      const std::function<void(const DiskScanStats&)>& onProgress = {});

    /// Dispatches to the in-memory or disk filter depending on Mode().
    /// `exactTarget` must be non-null iff `kind == ExactValue`. `onProgress`,
    /// if set, is forwarded as-is to FilterDiskCandidates() -- never called
    /// for the in-memory path (that filter pass has no comparable
    /// long-running phase to report on).
    FilterCommandResult Filter(CandidateFilterKind kind, const CandidateValue* exactTarget,
                                const std::function<void(const DiskScanStats&)>& onProgress = {});

    /// Validates config_.outputDir's session against `identity_` (peeking
    /// the manifest's own recorded value type/scope first, since `resume`
    /// takes no arguments of its own to compare against -- see
    /// SignalProbeScanController.cpp for why that's still a meaningful
    /// check). On success, switches Mode() to Disk.
    ResumeCommandResult Resume();

    StatusSnapshot Status() const;

    /// In-memory mode: the first `count` of the active candidate vector.
    /// Disk mode: streams the first `count` entries directly from the
    /// current baseline/generation file (BaselineReader/GenerationReader)
    /// -- never loads the whole file, regardless of `count` or how large
    /// the underlying file is.
    std::vector<ListEntry> List(std::size_t count) const;

    ScanMode Mode() const;

    /// The value type of the active scan, in either mode -- nullopt if
    /// Mode() == None. Used by callers (the command processor) that need
    /// to parse a value for the current type without caring which mode is
    /// active.
    std::optional<CandidateValueType> CurrentValueType() const;

private:
    void ClearInMemorySession();
    void ClearDiskSession();
    ScanSessionIdentity BuildScanSessionIdentity(CandidateValueType type, CandidateScanScope scope) const;

    IProcessReader& reader_;
    IProcessInspector& inspector_;
    const IProcessMemoryMap& memoryMap_;
    SignalProbeControllerConfig config_;
    ScanControllerIdentity identity_;

    ScanMode mode_ = ScanMode::None;

    std::vector<Candidate> candidates_;
    std::optional<CandidateValueType> inMemoryType_;
    std::optional<CandidateScanScope> inMemoryScope_;

    std::optional<ScanManifest> diskManifest_;
    DiskScanStats lastDiskStats_;
};

} // namespace sekiro_haptics::process
