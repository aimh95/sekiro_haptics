// Tests for chaining multiple FilterDiskCandidates() calls across
// generations: decreased -> unchanged -> increased, an interrupted filter
// preserving the prior generation, and resuming to continue filtering.

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

SH_TEST(GenerationChain_DecreasedThenUnchangedThenIncreased_TracksCorrectSurvivors) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_chain_full");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 u32 values

    std::uint32_t initial[4] = {100, 100, 100, 100};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &initial[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    // step 1: addr0 decreases, addr1 unchanged, addr2 increases, addr3 decreases
    std::uint32_t step1[4] = {50, 100, 150, 30};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &step1[i], 4);
    }

    DiskScanStats filter1Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Decreased, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filter1Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter1Stats.survivingCandidateCount == 2); // addr0, addr3

    ScanManifest manifestAfter1;
    std::string err;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifestAfter1, err));
    SH_CHECK(manifestAfter1.generation == 1);
    SH_CHECK(manifestAfter1.candidateCount == 2);

    // No memory change -- both survivors should pass "unchanged" (only
    // valid now that generation >= 1).
    DiskScanStats filter2Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Unchanged, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filter2Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter2Stats.survivingCandidateCount == 2);

    ScanManifest manifestAfter2;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifestAfter2, err));
    SH_CHECK(manifestAfter2.generation == 2);

    // addr0 increases, addr3 stays the same.
    std::uint32_t updatedAddr0 = 80;
    reader.PokeBytes(0x1000, &updatedAddr0, 4);

    DiskScanStats filter3Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Increased, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filter3Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter3Stats.survivingCandidateCount == 1);

    ScanManifest manifestAfter3;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifestAfter3, err));
    SH_CHECK(manifestAfter3.generation == 3);
    SH_CHECK(manifestAfter3.candidateCount == 1);

    GenerationReader finalReader(sessionDir / "candidates-0003.bin");
    SH_CHECK(finalReader.Open(1, CandidateValueType::U32, 3) == CandidateStorageResult::Success);
    std::uint64_t addr = 0;
    std::uint8_t buf[4] = {};
    SH_CHECK(finalReader.NextRecord(addr, buf));
    SH_CHECK(addr == 0x1000);
    std::uint32_t v = 0;
    std::memcpy(&v, buf, 4);
    SH_CHECK(v == 80);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(GenerationChain_InterruptedFilter_PreservesPriorGenerationAndDoesNotAdvance) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_chain_interrupted");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)}); // 2 values
    std::uint32_t initial[2] = {100, 100};
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &initial[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    std::uint32_t step1[2] = {50, 100}; // addr0 decreases
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &step1[i], 4);
    }

    DiskScanStats filter1Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Decreased, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filter1Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter1Stats.survivingCandidateCount == 1);

    std::uintmax_t gen1Size = std::filesystem::file_size(sessionDir / "candidates-0001.bin");

    reader.SetAlive(false); // simulate the target process disappearing

    DiskScanStats filter2Stats;
    DiskScanOutcome outcome2 =
        FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes, filter2Stats);
    SH_CHECK(outcome2 == DiskScanOutcome::ProcessExited);

    // Generation 1 must be untouched; generation 2 must never be published.
    SH_CHECK(std::filesystem::exists(sessionDir / "candidates-0001.bin"));
    SH_CHECK(std::filesystem::file_size(sessionDir / "candidates-0001.bin") == gen1Size);
    SH_CHECK(!std::filesystem::exists(sessionDir / "candidates-0002.bin"));

    ScanManifest manifest;
    std::string err;
    SH_CHECK(ReadScanManifest(sessionDir / "scan-manifest.json", manifest, err));
    SH_CHECK(manifest.generation == 1); // did not advance
    SH_CHECK(manifest.state == ScanManifestState::Interrupted);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(GenerationChain_ResumeAfterSimulatedRestart_CanContinueFiltering) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_chain_resume_continue");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    std::uint32_t initial[2] = {100, 100};
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &initial[i], 4);
    }

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    // "Restart": a fresh resume call against the same session dir, as a
    // new CLI invocation would perform, using the same reader (a real CLI
    // would re-attach first; here the fake is already attached/alive).
    ScanManifest resumedManifest;
    DiskScanOutcome resumeOutcome = ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, resumedManifest);
    SH_CHECK(resumeOutcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(resumedManifest.generation == 0);
    SH_CHECK(resumedManifest.state == ScanManifestState::BaselineComplete);

    std::uint32_t step1[2] = {50, 100};
    for (int i = 0; i < 2; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &step1[i], 4);
    }

    DiskScanStats filterStats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Decreased, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filterStats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filterStats.survivingCandidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(GenerationChain_SparseFilter_CoalescesReadsForNearbyCandidates) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_chain_sparse_coalesce");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    // Candidates spaced closely together so they land in the same
    // coalescing window, and one far away so it does not.
    memoryMap.SetRegions({MakeRegion(0x1000, 4 * 4), MakeRegion(0x200000, 4)});
    std::uint32_t near[4] = {1, 1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &near[i], 4);
    }
    std::uint32_t far = 1;
    reader.PokeBytes(0x200000, &far, 4);

    DiskScanStats beginStats;
    SH_CHECK(BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                                     sessionDir, MakeIdentity(), kDefaultDiskScanMemoryBudgetBytes,
                                     beginStats) == DiskScanOutcome::CompleteCoverage);

    // First filter: nothing changes, so use "exact" to select all 5 as
    // survivors into generation 1 (avoids the no-Unchanged-as-first-filter
    // restriction while keeping every candidate for the sparse pass below).
    CandidateValue target{std::uint32_t{1}};
    DiskScanStats filter1Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::ExactValue, &target, kDefaultDiskScanMemoryBudgetBytes,
                                   filter1Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter1Stats.survivingCandidateCount == 5);

    int callsBeforeSparseFilter = reader.ReadCalls();

    // Change every value so all 5 survive "changed" on the sparse pass.
    std::uint32_t updatedNear[4] = {2, 2, 2, 2};
    for (int i = 0; i < 4; ++i) {
        reader.PokeBytes(0x1000 + i * 4, &updatedNear[i], 4);
    }
    std::uint32_t updatedFar = 2;
    reader.PokeBytes(0x200000, &updatedFar, 4);

    DiskScanStats filter2Stats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Changed, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filter2Stats) == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(filter2Stats.survivingCandidateCount == 5);

    int callsAfterSparseFilter = reader.ReadCalls();
    // The 4 nearby candidates coalesce into 1 read; the far one needs its
    // own window -- 2 reads total, not 5.
    SH_CHECK(callsAfterSparseFilter - callsBeforeSparseFilter == 2);

    std::filesystem::remove_all(sessionDir);
}
