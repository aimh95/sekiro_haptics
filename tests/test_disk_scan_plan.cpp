// Tests for PlanCandidateScan() / CheckInMemoryScanBudget() -- preflight
// computation only, no scanning, no file writes, no process memory reads.

#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessMemoryMap.hpp"
#include "FakeProcessReader.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>

using namespace sekiro_haptics::process;

namespace {
ProcessMemoryRegion MakeRegion(std::uintptr_t base, std::size_t size, MemoryRegionKind kind = MemoryRegionKind::Private) {
    ProcessMemoryRegion r;
    r.baseAddress = base;
    r.sizeBytes = size;
    r.kind = kind;
    return r;
}
} // namespace

SH_TEST(PlanCandidateScan_SmallScope_ComputesExactTotalsAndRecommendsInMemory) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 4096), MakeRegion(0x10000, 8192)});

    ScanPlanReport report;
    PlanCandidateScanOutcome outcome =
        PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                           std::filesystem::temp_directory_path(), kDefaultDiskScanMemoryBudgetBytes, report);

    SH_CHECK(outcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(report.regionCount == 2);
    SH_CHECK(report.totalScopeBytes == 4096 + 8192);
    SH_CHECK(report.comparableValueCount == (4096 / 4) + (8192 / 4));
    SH_CHECK(report.estimatedInMemoryRamBytes == report.comparableValueCount * sizeof(Candidate));
    SH_CHECK(report.recommendedStorageMode == RecommendedStorageMode::InMemory);
}

SH_TEST(PlanCandidateScan_TinyBudget_RecommendsDiskBacked) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 10 * 1024 * 1024)}); // 10 MiB -> 2.5M u32 candidates

    ScanPlanReport report;
    std::size_t tinyBudget = 1024; // deliberately far too small
    PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                       std::filesystem::temp_directory_path(), tinyBudget, report);

    SH_CHECK(report.recommendedStorageMode == RecommendedStorageMode::DiskBacked);
}

SH_TEST(PlanCandidateScan_ZeroByteScope_ReportsZeroTotals) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({});

    ScanPlanReport report;
    PlanCandidateScanOutcome outcome =
        PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::AllReadable, CandidateValueType::U32,
                           std::filesystem::temp_directory_path(), kDefaultDiskScanMemoryBudgetBytes, report);

    SH_CHECK(outcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(report.regionCount == 0);
    SH_CHECK(report.totalScopeBytes == 0);
    SH_CHECK(report.comparableValueCount == 0);
    SH_CHECK(report.estimatedInMemoryRamBytes == 0);
    SH_CHECK(report.recommendedStorageMode == RecommendedStorageMode::InMemory);
}

SH_TEST(PlanCandidateScan_MemoryMapNotAttached_ReturnsNotAttached) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.ForceResult(MemoryMapResult::NotAttached);

    ScanPlanReport report;
    PlanCandidateScanOutcome outcome =
        PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::AllReadable, CandidateValueType::U32,
                           std::filesystem::temp_directory_path(), kDefaultDiskScanMemoryBudgetBytes, report);
    SH_CHECK(outcome == PlanCandidateScanOutcome::NotAttached);
}

SH_TEST(PlanCandidateScan_MainModuleScope_ModuleNotFound_ReturnsModuleNotFound) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    inspector.ForceGetMainModuleResult(ProcessInspectionResult::MainModuleNotFound);
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 4096, MemoryRegionKind::Image)});

    ScanPlanReport report;
    PlanCandidateScanOutcome outcome =
        PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::MainModule, CandidateValueType::U32,
                           std::filesystem::temp_directory_path(), kDefaultDiskScanMemoryBudgetBytes, report);
    SH_CHECK(outcome == PlanCandidateScanOutcome::ModuleNotFound);
}

SH_TEST(PlanCandidateScan_InsufficientDiskSpace_FailsClosedButStillReportsEstimates) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    // Enormous synthetic region so requiredDiskSpaceBytes vastly exceeds
    // any real available disk space -- metadata only, never actually read.
    memoryMap.SetRegions({MakeRegion(0x1000, static_cast<std::size_t>(50ULL * 1024 * 1024 * 1024 * 1024))}); // 50 TiB

    ScanPlanReport report;
    PlanCandidateScanOutcome outcome =
        PlanCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                           std::filesystem::temp_directory_path(), kDefaultDiskScanMemoryBudgetBytes, report);

    SH_CHECK(outcome == PlanCandidateScanOutcome::InsufficientDiskSpace);
    // Even on failure, the estimates are still populated so a caller can
    // show the user *why* it failed, not just that it failed.
    SH_CHECK(report.totalScopeBytes > 0);
    SH_CHECK(report.requiredDiskSpaceBytes > report.availableDiskSpaceBytes);
}

SH_TEST(CheckInMemoryScanBudget_ExtremeValueCount_SaturatesInsteadOfOverflowing) {
    CandidateScanPlanTotals totals;
    totals.comparableValueCount = std::numeric_limits<std::uint64_t>::max() / 2;
    InMemoryBudgetCheckResult result = CheckInMemoryScanBudget(totals, kDefaultDiskScanMemoryBudgetBytes);
    SH_CHECK(result == InMemoryBudgetCheckResult::InMemoryBudgetExceeded);
}

SH_TEST(CheckInMemoryScanBudget_SmallCount_WithinBudget) {
    CandidateScanPlanTotals totals;
    totals.comparableValueCount = 1000;
    SH_CHECK(CheckInMemoryScanBudget(totals, 1024 * 1024) == InMemoryBudgetCheckResult::WithinBudget);
}

SH_TEST(CheckInMemoryScanBudget_ExactlyAtBudget_WithinBudget) {
    CandidateScanPlanTotals totals;
    totals.comparableValueCount = 10;
    std::size_t budget = static_cast<std::size_t>(10 * sizeof(Candidate));
    SH_CHECK(CheckInMemoryScanBudget(totals, budget) == InMemoryBudgetCheckResult::WithinBudget);
}

SH_TEST(CheckInMemoryScanBudget_OneByteOverBudget_Exceeded) {
    CandidateScanPlanTotals totals;
    totals.comparableValueCount = 10;
    std::size_t budget = static_cast<std::size_t>(10 * sizeof(Candidate) - 1);
    SH_CHECK(CheckInMemoryScanBudget(totals, budget) == InMemoryBudgetCheckResult::InMemoryBudgetExceeded);
}
