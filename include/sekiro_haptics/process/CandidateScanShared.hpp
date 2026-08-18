#pragma once

// Logic shared between the in-memory CandidateScanner (CandidateScanner.hpp)
// and the disk-backed scanner (DiskCandidateScanner.hpp) added in
// SEK-PROBE-001C: scope-to-region resolution, the pure arithmetic behind
// "how many aligned candidates does a region contain," and filter-value
// comparison. Extracted from CandidateScanner.cpp so both scan modes (and
// `plan`) use one identical implementation rather than two that could
// silently drift apart. BeginCandidateScan()/FilterCandidates()'s public
// signatures and behavior are unchanged by this extraction -- they simply
// call these functions instead of inlining the same logic.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sekiro_haptics::process {

/// Resolves `scope` against `allRegions` (as returned by
/// IProcessMemoryMap::EnumerateReadableRegions()) into the exact set of
/// regions a scan should read, clipping MainModule to the main module's
/// image range the same way BeginCandidateScan() always has.
/// `outTargetRegions` is only written to on Success.
CandidateScanResult ResolveScanScopeRegions(IProcessInspector& inspector, const std::vector<ProcessMemoryRegion>& allRegions,
                                             CandidateScanScope scope,
                                             std::vector<ProcessMemoryRegion>& outTargetRegions);

/// How many `valueSize`-aligned candidate values `region` contains, using
/// the same absolute-address alignment rule BeginCandidateScan() already
/// implements (a candidate's address is `region.baseAddress` rounded up to
/// the next multiple of `valueSize`, then every `valueSize` bytes after
/// that). Pure arithmetic, no process I/O -- safe to call for a region
/// larger than any real read this process will ever perform (e.g. a
/// synthetic multi-gigabyte region in a test), and entirely 64-bit so it
/// never overflows for realistic region sizes.
std::uint64_t ComputeRegionAlignedValueCount(const ProcessMemoryRegion& region, std::size_t valueSize);

/// Aggregate totals over a resolved region set -- the basis for both the
/// in-memory budget check and `plan`'s estimates.
struct CandidateScanPlanTotals {
    std::size_t regionCount = 0;
    /// Sum of region.sizeBytes -- the true scope extent, not inflated by
    /// chunk-read overlap bytes.
    std::uint64_t totalScopeBytes = 0;
    /// Sum of ComputeRegionAlignedValueCount() over every region.
    std::uint64_t comparableValueCount = 0;
};

/// Computes CandidateScanPlanTotals for `targetRegions` (already resolved
/// via ResolveScanScopeRegions) at `type`'s value size/alignment.
CandidateScanPlanTotals ComputeCandidateScanPlanTotals(const std::vector<ProcessMemoryRegion>& targetRegions,
                                                        CandidateValueType type);

/// Outcome of CheckInMemoryScanBudget().
enum class InMemoryBudgetCheckResult {
    WithinBudget,
    /// `totals.comparableValueCount * sizeof(Candidate)` would exceed the
    /// configured budget -- the in-memory scan must not be attempted.
    InMemoryBudgetExceeded,
};
const char* ToString(InMemoryBudgetCheckResult result);

/// A proactive, computed check for whether an in-memory
/// std::vector<Candidate> holding every value in `totals` would fit within
/// `memoryBudgetBytes`. Never relies on allocation failure as the safety
/// mechanism -- this is evaluated *before* BeginCandidateScan() is called,
/// using only region metadata (already-known totals), never process
/// memory content.
InMemoryBudgetCheckResult CheckInMemoryScanBudget(const CandidateScanPlanTotals& totals, std::size_t memoryBudgetBytes);

/// Applies `kind`'s comparison between `previous` and `current`, honoring
/// the integer/float distinction and this module's float epsilon/NaN
/// policy (see kCandidateFloatEpsilon). The single source of truth for
/// filter semantics, used by both FilterCandidates() (in-memory) and the
/// disk-backed scanner's dense/sparse filter passes.
bool EvaluateCandidateFilter(CandidateFilterKind kind, const CandidateValue& previous, const CandidateValue& current,
                              const CandidateValue* exactTarget);

} // namespace sekiro_haptics::process
