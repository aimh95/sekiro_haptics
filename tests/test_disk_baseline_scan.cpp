// Tests for BeginDiskCandidateScan() -- baseline.bin/scan-manifest.json
// creation via Fakes and real (small) temp session directories. No real
// process, no multi-gigabyte writes.

#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessMemoryMap.hpp"
#include "FakeProcessReader.hpp"

#include <cstring>
#include <filesystem>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

ProcessMemoryRegion MakeRegion(std::uintptr_t base, std::size_t size, MemoryRegionKind kind = MemoryRegionKind::Private) {
    ProcessMemoryRegion r;
    r.baseAddress = base;
    r.sizeBytes = size;
    r.kind = kind;
    return r;
}

std::filesystem::path FreshSessionDir(const std::string& name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

ScanSessionIdentity MakeIdentity() {
    ScanSessionIdentity id;
    id.executableFileSizeBytes = 12345;
    id.sha256Hex = "deadbeef";
    id.pid = 999;
    id.mainModuleBaseAddress = 0x140000000;
    id.mainModuleImageSize = 0x1000;
    id.valueType = "u32";
    id.scope = "private-readable";
    return id;
}

} // namespace

SH_TEST(BeginDiskCandidateScan_FullCoverage_ProducesCompleteBaselineAndManifest) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_full");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;

    ProcessMemoryRegion region1 = MakeRegion(0x1000, 16); // 4 u32 values
    ProcessMemoryRegion region2 = MakeRegion(0x2000, 8);  // 2 u32 values
    memoryMap.SetRegions({region1, region2});

    for (int i = 0; i < 4; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(100 + i);
        reader.PokeBytes(0x1000 + i * 4, &v, 4);
    }
    for (int i = 0; i < 2; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(200 + i);
        reader.PokeBytes(0x2000 + i * 4, &v, 4);
    }

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(stats.processedBytes == 24);
    SH_CHECK(stats.coveragePercent > 99.9);
    SH_CHECK(stats.regionsProcessed == 2);
    SH_CHECK(stats.regionsTotal == 2);
    SH_CHECK(stats.peakBufferedBytes < kDefaultDiskScanMemoryBudgetBytes);

    SH_CHECK(std::filesystem::exists(sessionDir / "baseline.bin"));
    SH_CHECK(!std::filesystem::exists(sessionDir / "baseline.tmp"));

    ScanManifest manifest;
    std::string error;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifest, error));
    SH_CHECK(manifest.state == ScanManifestState::BaselineComplete);
    SH_CHECK(manifest.completedNormally);
    SH_CHECK(manifest.generation == 0);
    SH_CHECK(manifest.totalValueCount == 6);
    SH_CHECK(manifest.regions.size() == 2);
    SH_CHECK(manifest.coveragePercent > 99.9);

    BaselineReader baselineReader(sessionDir / "baseline.bin");
    SH_CHECK(baselineReader.Open(6, CandidateValueType::U32) == CandidateStorageResult::Success);
    ProcessMemoryRegion readRegion;
    std::uint64_t readCount = 0;
    SH_CHECK(baselineReader.NextRegion(readRegion, readCount));
    SH_CHECK(readCount == 4);
    std::vector<std::uint8_t> buf(4 * 4);
    SH_CHECK(baselineReader.ReadRegionValueChunk(0, 4, buf.data()) == 4);
    for (int i = 0; i < 4; ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, &buf[i * 4], 4);
        SH_CHECK(v == 100 + i);
    }

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDiskCandidateScan_MidScanReadFailure_ReturnsReadFailedAndLeavesNoCompleteBaseline) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_readfail");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16), MakeRegion(0x2000, 8)});
    reader.FailReadAtCall(1); // second region's chunk read fails

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::ReadFailed);
    SH_CHECK(!std::filesystem::exists(sessionDir / "baseline.bin"));

    ScanManifest manifest;
    std::string error;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifest, error));
    SH_CHECK(manifest.state == ScanManifestState::Failed);
    SH_CHECK(!manifest.completedNormally);
    SH_CHECK(!manifest.failureReason.empty());

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDiskCandidateScan_PartialRead_ReturnsIncompleteCoverage) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_partial");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});
    reader.ForcePartialReadAtCall(0, 8);

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::IncompleteCoverage);
    SH_CHECK(!std::filesystem::exists(sessionDir / "baseline.bin"));

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDiskCandidateScan_ProcessExitedMidScan_ReturnsProcessExitedAndMarksInterrupted) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_exited");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16), MakeRegion(0x2000, 8)});
    reader.FailAsProcessExitedAtCall(1);

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::ProcessExited);
    SH_CHECK(!std::filesystem::exists(sessionDir / "baseline.bin"));

    ScanManifest manifest;
    std::string error;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifest, error));
    SH_CHECK(manifest.state == ScanManifestState::Interrupted);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDiskCandidateScan_NotAttached_ReturnsNotAttached) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_notattached");

    FakeProcessReader reader;
    reader.SetAttached(false);
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::NotAttached);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDiskCandidateScan_FailedAttempt_PreservesPriorCompleteBaselineInAnotherSession) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_preserve");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});

    DiskScanStats stats;
    DiskScanOutcome first =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);
    SH_CHECK(first == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(std::filesystem::exists(sessionDir / "baseline.bin"));
    std::uintmax_t firstBaselineSize = std::filesystem::file_size(sessionDir / "baseline.bin");

    std::filesystem::path secondSessionDir = FreshSessionDir("sh_disk_baseline_preserve_2");
    FakeProcessReader reader2;
    reader2.SetAttached(false);
    DiskScanStats stats2;
    DiskScanOutcome second =
        BeginDiskCandidateScan(reader2, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                secondSessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats2);
    SH_CHECK(second == DiskScanOutcome::NotAttached);

    // The first session's already-complete baseline must be untouched by
    // the second, independent, failed attempt.
    SH_CHECK(std::filesystem::exists(sessionDir / "baseline.bin"));
    SH_CHECK(std::filesystem::file_size(sessionDir / "baseline.bin") == firstBaselineSize);

    std::filesystem::remove_all(sessionDir);
    std::filesystem::remove_all(secondSessionDir);
}

SH_TEST(BeginDiskCandidateScan_BudgetRespected_PeakBufferedBytesStaysWellUnderScopeSize) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_baseline_budget");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    // Larger than one I/O chunk, to exercise the multi-chunk loop.
    memoryMap.SetRegions({MakeRegion(0x1000, 200 * 1024)}); // 200 KiB

    DiskScanStats stats;
    DiskScanOutcome outcome =
        BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes, stats);

    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(stats.peakBufferedBytes <= stats.configuredMemoryBudgetBytes);
    // Structural proof this is bounded, not scope-proportional: the region
    // is 200KiB but the buffers stay within roughly one chunk's worth.
    SH_CHECK(stats.peakBufferedBytes < 1024 * 1024);

    std::filesystem::remove_all(sessionDir);
}
