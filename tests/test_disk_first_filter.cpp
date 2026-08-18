// Tests for FilterDiskCandidates()'s first-filter-from-baseline path:
// policy (no Unchanged as the first filter), read coalescing, fallback on
// a failed coalesced read, and drop-statistics. No real process.

#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessMemoryMap.hpp"
#include "FakeProcessReader.hpp"

#include <cstring>
#include <filesystem>

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

SH_TEST(FilterDiskCandidates_FirstFilterUnchanged_ReturnsInitialFilterTooBroad) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_toobroad");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});
    for (int i = 0; i < 4; ++i) {
        std::uint32_t v = 10;
        reader.PokeBytes(0x1000 + i * 4, &v, 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Unchanged, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::InitialFilterTooBroad);
    SH_CHECK(!std::filesystem::exists(sessionDir / "candidates-0001.bin"));

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_FirstFilterDecreased_KeepsOnlyDecreasedValuesAddressSorted) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_decreased");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 u32 values

    std::uint32_t baseline[4] = {100, 200, 300, 400};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &baseline[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    // index 0: decreases, index 1: unchanged, index 2: increases, index 3: decreases
    std::uint32_t updated0 = 50;
    reader.PokeBytes(0x1000, &updated0, 4);
    std::uint32_t updated2 = 350;
    reader.PokeBytes(0x1000 + 8, &updated2, 4);
    std::uint32_t updated3 = 10;
    reader.PokeBytes(0x1000 + 12, &updated3, 4);

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Decreased, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 2);

    SH_CHECK(std::filesystem::exists(sessionDir / "candidates-0001.bin"));

    ScanManifest manifest;
    std::string err;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifest, err));
    SH_CHECK(manifest.generation == 1);
    SH_CHECK(manifest.candidateCount == 2);
    SH_CHECK(manifest.state == ScanManifestState::CandidatesComplete);

    GenerationReader genReader(sessionDir / "candidates-0001.bin");
    SH_CHECK(genReader.Open(2, CandidateValueType::U32, 1) == CandidateStorageResult::Success);
    std::uint64_t addr = 0;
    std::uint8_t buf[4] = {};
    SH_CHECK(genReader.NextRecord(addr, buf));
    SH_CHECK(addr == 0x1000);
    std::uint32_t v = 0;
    std::memcpy(&v, buf, 4);
    SH_CHECK(v == 50);
    SH_CHECK(genReader.NextRecord(addr, buf));
    SH_CHECK(addr == 0x100C);
    std::memcpy(&v, buf, 4);
    SH_CHECK(v == 10);
    SH_CHECK(genReader.NextRecord(addr, buf) == false);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_FirstFilterChanged_Accepted) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_changed");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)}); // 2 values
    std::uint32_t baseline[2] = {1, 2};
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &baseline[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    std::uint32_t updated0 = 99; // changed
    reader.PokeBytes(0x1000, &updated0, 4);
    // index 1 unchanged

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_FirstFilterIncreased_Accepted) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_increased");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 4)});
    std::uint32_t baseline = 10;
    reader.PokeBytes(0x1000, &baseline, 4);

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    std::uint32_t updated = 20;
    reader.PokeBytes(0x1000, &updated, 4);

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Increased, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_FirstFilterExactValue_Accepted) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_exact");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    std::uint32_t baseline[2] = {1, 2};
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &baseline[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    std::uint32_t updated0 = 777, updated1 = 42;
    reader.PokeBytes(0x1000, &updated0, 4);
    reader.PokeBytes(0x1004, &updated1, 4);

    CandidateValue target{std::uint32_t{777}};
    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::ExactValue, &target, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_ExactValueNoTargetProvided_ReturnsInvalidTargetImmediately) {
    // No baseline exists at all -- InvalidTarget must be checked before
    // anything about session state is touched.
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_no_target");
    FakeProcessReader reader;

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::ExactValue, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::InvalidTarget);
}

SH_TEST(FilterDiskCandidates_FirstFilter_CoalescesReadsAcrossManyCandidatesInOneChunk) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_coalesce");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    const int count = 100;
    memoryMap.SetRegions({MakeRegion(0x1000, count * 4)});
    for (int i = 0; i < count; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(1000 + i);
        reader.PokeBytes(0x1000 + i * 4, &v, 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);
    int callsAfterBegin = reader.ReadCalls();
    SH_CHECK(callsAfterBegin == 1); // one region, fits in one chunk

    for (int i = 0; i < count; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(2000 + i);
        reader.PokeBytes(0x1000 + i * 4, &v, 4);
    }

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == static_cast<std::uint64_t>(count));

    int callsAfterFilter = reader.ReadCalls();
    // Exactly ONE additional ReadBytes() call for the whole filter pass --
    // proves 100 candidates were read via a single coalesced call, not 100
    // individual ones.
    SH_CHECK(callsAfterFilter - callsAfterBegin == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_CoalescedReadFails_FallsBackToPerAddressReadsAndStillSucceeds) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_fallback");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 values
    std::uint32_t baseline[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &baseline[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);
    int callsAfterBegin = reader.ReadCalls();

    std::uint32_t updated[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &updated[i], 4);
    }

    reader.FailReadAtCall(callsAfterBegin); // the coalesced filter read fails

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 4);
    SH_CHECK(filterStats.droppedCandidateCount == 0);

    int callsAfterFilter = reader.ReadCalls();
    // 1 failed coalesced call + 4 per-address fallback calls.
    SH_CHECK(callsAfterFilter - callsAfterBegin == 5);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(FilterDiskCandidates_OnePerAddressFallbackReadFails_DropsOnlyThatCandidateAndCountsIt) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_filter_drop");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 values
    std::uint32_t baseline[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &baseline[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);
    int callsAfterBegin = reader.ReadCalls();

    std::uint32_t updated[4] = {10, 20, 30, 40};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &updated[i], 4);
    }

    // Fail the coalesced read (forces fallback), and fail the fallback
    // attempt for the second candidate specifically (address 0x1004).
    reader.FailReadAtCalls({callsAfterBegin, callsAfterBegin + 2});

    DiskScanStats filterStats;
    DiskScanOutcome outcome =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes, filterStats);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 3);
    SH_CHECK(filterStats.droppedCandidateCount == 1);
    SH_CHECK(!filterStats.droppedCandidateReasonSummary.empty());

    std::filesystem::remove_all(sessionDir);
}
