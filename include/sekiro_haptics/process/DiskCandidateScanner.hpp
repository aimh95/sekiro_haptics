#pragma once

// Disk-backed candidate scanner (SEK-PROBE-001C): the RAM-bounded
// alternative to CandidateScanner.hpp's in-memory BeginCandidateScan()/
// FilterCandidates(), for scopes too large to hold as a std::vector<Candidate>
// (Sekiro's real `private-readable` scope is ~10-11GB across ~5,300
// regions -- a 16GB machine cannot hold that as 24-byte-per-value
// candidates in RAM). RAM usage here is bounded by `memoryBudgetBytes`
// (default 512MiB), independent of scope size or candidate count -- see
// docs/06-signal-discovery-probe.md for the full contract this
// implements: complete-coverage-or-fail-closed, crash-safe publish, and
// resumability.

#include "sekiro_haptics/process/CandidateScanShared.hpp"
#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace sekiro_haptics::process {

/// Default RAM budget for a disk-backed scan's own buffers (process read
/// buffer, file I/O buffer, decode/compare buffer, region/metadata table).
/// Deliberately small and fixed -- see DiskScanStats::peakBufferedBytes.
inline constexpr std::size_t kDefaultDiskScanMemoryBudgetBytes = 512ULL * 1024 * 1024;

/// A later filter pass over a sparse generation groups consecutive
/// address-sorted survivors into windows of at most this many bytes and
/// issues one coalesced ReadBytes() call per window, instead of one call
/// per candidate. Reuses CandidateScanner's own chunk-size constant since
/// both represent "how much of the address space to read in one call."
inline constexpr std::size_t kGenerationFilterCoalesceWindowBytes = kCandidateScanChunkBytes;

/// Outcome of a disk-backed scan/filter/resume/plan operation.
enum class DiskScanOutcome {
    CompleteCoverage,
    /// Stopped before every selected region/byte was processed (e.g. a
    /// mid-scan read failure). Never collapsed into "no candidates found"
    /// -- callers must treat this as an inconclusive result, not a
    /// negative one.
    IncompleteCoverage,
    Interrupted,
    ReadFailed,
    WriteFailed,
    FlushFailed,
    InsufficientDiskSpace,
    NotAttached,
    ProcessExited,
    ModuleNotFound,
    MemoryMapUnavailable,
    AddressOverflow,
    /// Resume: the current process/build identity does not match the
    /// manifest's recorded identity -- nothing from the old session is reused.
    SessionMismatch,
    /// The first filter applied to a fresh baseline requested `Unchanged`,
    /// which is rejected -- see FilterDiskCandidates()'s doc comment.
    InitialFilterTooBroad,
    InvalidTarget,
    CorruptFile,
};
const char* ToString(DiskScanOutcome outcome);

/// Progress/budget statistics for one BeginDiskCandidateScan() or
/// FilterDiskCandidates() call.
struct DiskScanStats {
    std::size_t configuredMemoryBudgetBytes = 0;
    /// The largest sum of this call's own owned buffers observed at any
    /// point -- NOT a measurement of total OS working set. See
    /// docs/06-signal-discovery-probe.md.
    std::size_t peakBufferedBytes = 0;
    std::uint64_t processedBytes = 0;
    double coveragePercent = 0.0;
    std::uint64_t survivingCandidateCount = 0;
    std::size_t regionsProcessed = 0;
    std::size_t regionsTotal = 0;
    /// Filter passes only: candidates dropped due to a per-address read
    /// failure (process still attached/alive) rather than failing the
    /// filter -- counted rather than silently discarded.
    std::uint64_t droppedCandidateCount = 0;
    std::string droppedCandidateReasonSummary;
};

/// Reads the entire selected scope once and writes it to
/// `<sessionDir>/baseline.bin` via BaselineWriter, publishing only on full
/// success (see CandidateStorage.hpp's crash-safety discipline). Also
/// writes `<sessionDir>/scan-manifest.json`. `identity` is the caller's
/// already-computed ScanSessionIdentity (this function does not compute
/// executable/module identity itself -- that's ExecutableIdentity's job,
/// Win32-only and out of scope for this OS-independent module).
///
/// `onProgress`, if set, is called periodically (region/byte counts only
/// -- never memory content) during the scan.
///
/// Returns CompleteCoverage only if every selected region was processed,
/// every expected byte was read, no read/write/flush failure occurred, the
/// process stayed attached and alive throughout, and the published
/// baseline's own size/count cross-checks pass. Any other outcome leaves
/// no complete baseline.bin behind (an existing one, if any, is untouched).
DiskScanOutcome BeginDiskCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                        const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                        CandidateValueType type, const std::filesystem::path& sessionDir,
                                        const ScanSessionIdentity& identity, std::size_t memoryBudgetBytes,
                                        DiskScanStats& outStats,
                                        const std::function<void(const DiskScanStats&)>& onProgress = {});

/// Applies one filter pass. If the session's current generation is 0 (a
/// fresh baseline, not yet filtered), this is the "first filter": reads
/// baseline.bin densely, region by region, coalescing process reads per
/// I/O chunk (never one ReadBytes() call per candidate) and writes
/// surviving candidates to `<sessionDir>/candidates-0001.bin`.
/// `kind == CandidateFilterKind::Unchanged` is rejected as the first
/// filter (InitialFilterTooBroad) -- baseline `unchanged` retains nearly
/// every value and would defeat the entire point of filtering down from a
/// multi-gigabyte baseline.
///
/// If the current generation is >=1, this reads the current generation's
/// sparse, address-sorted candidates-NNNN.bin, coalesces process reads
/// across nearby addresses, and writes survivors to the next generation.
/// Normal filter semantics (including Unchanged) apply from here on.
///
/// Publishes the new generation and advances the manifest only on full
/// success; any failure leaves the previous complete generation (or
/// baseline) untouched -- see CandidateStorage.hpp.
///
/// `onProgress`, if set, is called periodically (region/byte/candidate
/// counts only -- never memory content) during the pass, same contract as
/// BeginDiskCandidateScan()'s `onProgress`.
DiskScanOutcome FilterDiskCandidates(IProcessReader& reader, const std::filesystem::path& sessionDir,
                                      CandidateFilterKind kind, const CandidateValue* exactTarget,
                                      std::size_t memoryBudgetBytes, DiskScanStats& outStats,
                                      const std::function<void(const DiskScanStats&)>& onProgress = {});

/// Validates a session directory's scan-manifest.json against
/// `currentIdentity` (all 6 identity fields) and `reader.IsAlive()`, and
/// validates the newest complete data file (baseline.bin if
/// generation==0, else the highest candidates-NNNN.bin) against the
/// manifest's own recorded counts. On success, `outManifest` reflects the
/// resumed session's state, ready for a further FilterDiskCandidates()
/// call. Any mismatch is SessionMismatch -- nothing from the old session
/// is reused, and nothing is deleted. Documented limitation: matching PID
/// alone never proves the same process *instance* -- the full 6-field
/// tuple (including the ASLR-dependent module base) is the best available
/// evidence; there is no cheaper OS-level check in scope here.
DiskScanOutcome ResumeDiskCandidateSession(const std::filesystem::path& sessionDir,
                                            const ScanSessionIdentity& currentIdentity, IProcessReader& reader,
                                            ScanManifest& outManifest);

/// Outcome of PlanCandidateScan().
enum class PlanCandidateScanOutcome {
    Success,
    NotAttached,
    ProcessExited,
    ModuleNotFound,
    MemoryMapUnavailable,
    AddressOverflow,
    InsufficientDiskSpace,
};
const char* ToString(PlanCandidateScanOutcome outcome);

enum class RecommendedStorageMode {
    InMemory,
    DiskBacked,
};
const char* ToString(RecommendedStorageMode mode);

/// Preflight estimate for a scan, computed from region metadata only --
/// never touches process memory content, never writes a file.
struct ScanPlanReport {
    std::size_t regionCount = 0;
    std::uint64_t totalScopeBytes = 0;
    std::uint64_t comparableValueCount = 0;
    /// sizeof(Candidate) * comparableValueCount -- what BeginCandidateScan()
    /// would need to allocate.
    std::uint64_t estimatedInMemoryRamBytes = 0;
    /// Header + region directory + totalScopeBytes-worth of packed values
    /// + footer -- baseline.bin's exact size if this scan runs to completion.
    std::uint64_t estimatedBaselineFileBytes = 0;
    /// The largest a single filter generation file could be: every
    /// candidate surviving with its full per-record overhead (address +
    /// value + header/footer) -- a pessimistic upper bound, not an
    /// average-case guess.
    std::uint64_t estimatedMaxGenerationFileBytes = 0;
    std::uint64_t availableDiskSpaceBytes = 0;
    /// Baseline + one worst-case generation + manifest headroom --
    /// computed pessimistically, never optimistically hidden.
    std::uint64_t requiredDiskSpaceBytes = 0;
    std::size_t memoryBudgetBytes = 0;
    RecommendedStorageMode recommendedStorageMode = RecommendedStorageMode::InMemory;
    /// The coverage a run targets if started now (always 100 -- this
    /// field exists so a future partial-resume scenario has somewhere to
    /// report a lower planned target; nothing in this implementation ever
    /// plans for less than full coverage).
    double expectedCoveragePercent = 100.0;
};

/// Computes a ScanPlanReport for `scope`/`type` without scanning, without
/// creating a session directory, and without writing any file.
/// `outputDrivePath` is the filesystem the eventual session directory
/// would live on (used only for the free-disk-space query). Fails closed
/// with InsufficientDiskSpace if `outputDrivePath`'s free space is less
/// than `requiredDiskSpaceBytes` -- computed before any `begin-disk` call
/// touches disk.
PlanCandidateScanOutcome PlanCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                            const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                            CandidateValueType type, const std::filesystem::path& outputDrivePath,
                                            std::size_t memoryBudgetBytes, ScanPlanReport& outReport);

/// The file name (no directory) a session's data file has for a given
/// `generation`: "baseline.bin" for 0, "candidates-NNNN.bin" for >=1 --
/// the exact naming BeginDiskCandidateScan()/FilterDiskCandidates() use.
/// Exposed so callers (the CLI's `status` command) can report which file
/// is current without duplicating the naming scheme.
std::string CurrentDataFileName(int generation);

} // namespace sekiro_haptics::process
