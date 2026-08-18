// Tests for SignalProbeScanController and SignalProbeCommandProcessor
// (SEK-PROBE-001C-2): the OS-independent command/state layer that wires
// the disk-backed scanner into the signal-probe CLI. Fakes + real
// temporary session directories only -- no <windows.h>, no real process,
// no sleep_for (every operation here is synchronous).

#include "sekiro_haptics/process/SignalProbeCommandProcessor.hpp"
#include "sekiro_haptics/process/SignalProbeScanController.hpp"
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

ScanControllerIdentity MakeControllerIdentity() {
    ScanControllerIdentity id;
    id.executableFileSizeBytes = 12345;
    id.sha256Hex = "deadbeef";
    id.pid = 999;
    id.mainModuleBaseAddress = 0x140000000;
    id.mainModuleImageSize = 0x1000;
    return id;
}

// Mirrors SignalProbeScanController::BuildScanSessionIdentity() so tests
// can independently pre-populate a session directory that a controller
// constructed with `id` will accept on Resume().
ScanSessionIdentity ToScanSessionIdentity(const ScanControllerIdentity& id, CandidateValueType type,
                                           CandidateScanScope scope) {
    ScanSessionIdentity out;
    out.executableFileSizeBytes = id.executableFileSizeBytes;
    out.sha256Hex = id.sha256Hex;
    out.pid = id.pid;
    out.mainModuleBaseAddress = id.mainModuleBaseAddress;
    out.mainModuleImageSize = id.mainModuleImageSize;
    out.valueType = ToString(type);
    out.scope = ToString(scope);
    return out;
}

void PokeU32(FakeProcessReader& reader, std::uintptr_t address, std::uint32_t value) {
    reader.PokeBytes(address, &value, 4);
}

} // namespace

// --- Plan ----------------------------------------------------------------

SH_TEST(Plan_DoesNotCreateFiles) {
    // Matches real usage: main.cpp always creates the output directory
    // itself before constructing the controller (Plan()'s own disk-space
    // preflight requires an existing path to query free space on).
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_plan_nofiles");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    PlanCommandResult result = controller.Plan(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.outcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(std::filesystem::is_empty(sessionDir)); // no file or snapshot created

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Plan_ReportsExpectedByteValueRamDiskNumbers) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_plan_numbers");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 4096), MakeRegion(0x10000, 8192)});

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    config.memoryBudgetBytes = kDefaultDiskScanMemoryBudgetBytes;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    PlanCommandResult result = controller.Plan(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.outcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(result.report.regionCount == 2);
    SH_CHECK(result.report.totalScopeBytes == 4096 + 8192);
    SH_CHECK(result.report.comparableValueCount == (4096 / 4) + (8192 / 4));
    SH_CHECK(result.report.estimatedInMemoryRamBytes == result.report.comparableValueCount * sizeof(Candidate));
    SH_CHECK(result.report.memoryBudgetBytes == kDefaultDiskScanMemoryBudgetBytes);

    std::filesystem::remove_all(sessionDir);
}

// --- Begin (in-memory) -----------------------------------------------------

SH_TEST(Begin_SmallScope_Succeeds) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_begin_small");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 u32 values
    for (int i = 0; i < 4; ++i) {
        PokeU32(reader, 0x1000 + i * 4, static_cast<std::uint32_t>(100 + i));
    }

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginCommandResult result = controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.outcome == BeginCommandOutcome::Success);
    SH_CHECK(result.candidateCount == 4);
    SH_CHECK(controller.Mode() == ScanMode::InMemory);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Begin_LargeScope_ReturnsInMemoryBudgetExceededBeforeAllocation) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_begin_budget");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)}); // 4 u32 values -- any nonzero budget below 4*sizeof(Candidate) rejects

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    config.memoryBudgetBytes = 1; // deliberately far too small
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginCommandResult result = controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.outcome == BeginCommandOutcome::InMemoryBudgetExceeded);
    SH_CHECK(result.totals.comparableValueCount == 4);
    SH_CHECK(controller.Mode() == ScanMode::None);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Begin_LargeMaxCandidatesFlag_CannotBypassBudget) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_begin_budget_maxcand");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    config.memoryBudgetBytes = 1;
    config.maxCandidates = 100'000'000; // deliberately enormous -- must not bypass the budget check
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginCommandResult result = controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.outcome == BeginCommandOutcome::InMemoryBudgetExceeded);
    SH_CHECK(controller.Mode() == ScanMode::None);

    std::filesystem::remove_all(sessionDir);
}

// --- BeginDisk ---------------------------------------------------------

SH_TEST(BeginDisk_Success_SwitchesModeToDisk) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_begindisk_ok");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)}); // 2 u32 values
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 200);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginDiskCommandResult result = controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.ranScan);
    SH_CHECK(result.planOutcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(result.scanOutcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(controller.Mode() == ScanMode::Disk);

    StatusSnapshot snapshot = controller.Status();
    SH_CHECK(snapshot.diskManifest.has_value());
    SH_CHECK(snapshot.diskManifest->generation == 0);
    SH_CHECK(snapshot.diskManifest->totalValueCount == 2);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(BeginDisk_IncompleteOrFailed_DoesNotActivateSession) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_begindisk_fail");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 16)});
    reader.FailReadAtCall(0); // the real scan's first read fails -- preflight itself never reads memory content

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginDiskCommandResult result = controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(result.ranScan);
    SH_CHECK(result.scanOutcome != DiskScanOutcome::CompleteCoverage);
    SH_CHECK(controller.Mode() == ScanMode::None);

    std::filesystem::remove_all(sessionDir);
}

// --- Filter --------------------------------------------------------------

SH_TEST(Filter_DiskFirstFilterUnchanged_Rejected) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_filter_first_unchanged");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 200);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);

    FilterCommandResult result = controller.Filter(CandidateFilterKind::Unchanged, nullptr);
    SH_CHECK(result.outcome == FilterCommandOutcome::DiskFilterFailed);
    SH_CHECK(result.diskResult == DiskScanOutcome::InitialFilterTooBroad);
    SH_CHECK(controller.Mode() == ScanMode::Disk);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Filter_DiskFirstFilterDecreased_Succeeds) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_filter_first_decreased");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 100);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);

    PokeU32(reader, 0x1000, 50); // decreased
    // 0x1004 stays 100 -- unchanged, must not survive a "decreased" filter.

    FilterCommandResult result = controller.Filter(CandidateFilterKind::Decreased, nullptr);
    SH_CHECK(result.outcome == FilterCommandOutcome::Success);
    SH_CHECK(result.diskStats.survivingCandidateCount == 1);

    StatusSnapshot snapshot = controller.Status();
    SH_CHECK(snapshot.diskManifest->generation == 1);
    SH_CHECK(snapshot.diskManifest->candidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Filter_DiskSubsequentUnchangedThenIncreased_Succeeds) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_filter_chain");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 100);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);

    PokeU32(reader, 0x1000, 50); // decreased -- seeds generation 1 with just this address
    SH_CHECK(controller.Filter(CandidateFilterKind::Decreased, nullptr).outcome == FilterCommandOutcome::Success);
    SH_CHECK(controller.Status().diskManifest->generation == 1);

    // No memory change -- "unchanged" must keep the sole survivor.
    FilterCommandResult unchangedResult = controller.Filter(CandidateFilterKind::Unchanged, nullptr);
    SH_CHECK(unchangedResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(unchangedResult.diskStats.survivingCandidateCount == 1);
    SH_CHECK(controller.Status().diskManifest->generation == 2);

    PokeU32(reader, 0x1000, 80); // increased from 50
    FilterCommandResult increasedResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    SH_CHECK(increasedResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(increasedResult.diskStats.survivingCandidateCount == 1);
    SH_CHECK(controller.Status().diskManifest->generation == 3);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Filter_NoActiveScan_ReturnsNoActiveScan) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_filter_none");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    FilterCommandResult result = controller.Filter(CandidateFilterKind::Changed, nullptr);
    SH_CHECK(result.outcome == FilterCommandOutcome::NoActiveScan);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Filter_InMemoryMode_DispatchesToFilterCandidates) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_filter_inmemory");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 100);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);

    PokeU32(reader, 0x1000, 999); // changed
    FilterCommandResult result = controller.Filter(CandidateFilterKind::Changed, nullptr);
    SH_CHECK(result.outcome == FilterCommandOutcome::Success);
    SH_CHECK(result.inMemoryRemainingCount == 1);

    std::filesystem::remove_all(sessionDir);
}

// --- Status/List ---------------------------------------------------------

SH_TEST(Status_ReflectsInMemoryMode) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_status_inmemory");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 12)}); // 3 u32 values

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);

    StatusSnapshot snapshot = controller.Status();
    SH_CHECK(snapshot.mode == ScanMode::InMemory);
    SH_CHECK(snapshot.inMemoryType.has_value() && *snapshot.inMemoryType == CandidateValueType::U32);
    SH_CHECK(snapshot.inMemoryScope.has_value() && *snapshot.inMemoryScope == CandidateScanScope::PrivateReadable);
    SH_CHECK(snapshot.inMemoryCandidateCount == 3);
    SH_CHECK(!snapshot.diskManifest.has_value());

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Status_ReflectsDiskModeFields) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_status_disk");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);

    StatusSnapshot snapshot = controller.Status();
    SH_CHECK(snapshot.mode == ScanMode::Disk);
    SH_CHECK(snapshot.diskManifest.has_value());
    SH_CHECK(snapshot.diskManifest->identity.valueType == "u32");
    SH_CHECK(snapshot.diskManifest->identity.scope == "private-readable");
    SH_CHECK(snapshot.diskManifest->generation == 0);
    SH_CHECK(snapshot.inMemoryCandidateCount == 0);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(List_DiskMode_StreamsOnlyRequestedCountWithoutFullLoad) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_list_disk");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 40)}); // 10 u32 values
    for (int i = 0; i < 10; ++i) {
        PokeU32(reader, 0x1000 + i * 4, static_cast<std::uint32_t>(1000 + i));
    }

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);

    std::vector<ListEntry> entries = controller.List(3);
    SH_CHECK(entries.size() == 3);
    SH_CHECK(entries[0].address == 0x1000);
    SH_CHECK(std::get<std::uint32_t>(entries[0].value) == 1000);
    SH_CHECK(entries[2].address == 0x1000 + 8);
    SH_CHECK(std::get<std::uint32_t>(entries[2].value) == 1002);

    std::vector<ListEntry> capped = controller.List(1000);
    SH_CHECK(capped.size() == 10); // never more than what actually exists

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(List_InMemoryMode_Unchanged) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_list_inmemory");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 111);
    PokeU32(reader, 0x1004, 222);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);

    std::vector<ListEntry> entries = controller.List(2);
    SH_CHECK(entries.size() == 2);
    SH_CHECK(entries[0].address == 0x1000);
    SH_CHECK(std::get<std::uint32_t>(entries[0].value) == 111);
    SH_CHECK(entries[1].address == 0x1004);
    SH_CHECK(std::get<std::uint32_t>(entries[1].value) == 222);

    std::filesystem::remove_all(sessionDir);
}

// --- Resume ----------------------------------------------------------------

SH_TEST(Resume_Success_RestoresActiveDiskSession) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_resume_ok");

    ScanControllerIdentity id = MakeControllerIdentity();

    {
        FakeProcessReader setupReader;
        FakeProcessInspector setupInspector;
        FakeProcessMemoryMap setupMap;
        setupMap.SetRegions({MakeRegion(0x1000, 8)});
        PokeU32(setupReader, 0x1000, 1);
        PokeU32(setupReader, 0x1004, 2);
        DiskScanStats stats;
        SH_CHECK(BeginDiskCandidateScan(setupReader, setupInspector, setupMap, CandidateScanScope::PrivateReadable,
                                          CandidateValueType::U32, sessionDir,
                                          ToScanSessionIdentity(id, CandidateValueType::U32, CandidateScanScope::PrivateReadable),
                                          kDefaultDiskScanMemoryBudgetBytes, stats) == DiskScanOutcome::CompleteCoverage);
    }

    // A fresh controller, simulating a CLI restart -- no Begin/BeginDisk
    // called on it yet.
    FakeProcessReader liveReader;
    liveReader.SetAlive(true);
    FakeProcessInspector liveInspector;
    FakeProcessMemoryMap liveMap;
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(liveReader, liveInspector, liveMap, config, id);

    ResumeCommandResult result = controller.Resume();
    SH_CHECK(result.outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(controller.Mode() == ScanMode::Disk);
    SH_CHECK(controller.Status().diskManifest->generation == 0);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Resume_IdentityMismatch_Rejected) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_resume_mismatch");

    ScanControllerIdentity id = MakeControllerIdentity();

    {
        FakeProcessReader setupReader;
        FakeProcessInspector setupInspector;
        FakeProcessMemoryMap setupMap;
        setupMap.SetRegions({MakeRegion(0x1000, 8)});
        DiskScanStats stats;
        SH_CHECK(BeginDiskCandidateScan(setupReader, setupInspector, setupMap, CandidateScanScope::PrivateReadable,
                                          CandidateValueType::U32, sessionDir,
                                          ToScanSessionIdentity(id, CandidateValueType::U32, CandidateScanScope::PrivateReadable),
                                          kDefaultDiskScanMemoryBudgetBytes, stats) == DiskScanOutcome::CompleteCoverage);
    }

    ScanControllerIdentity mismatched = id;
    mismatched.pid += 1;

    FakeProcessReader liveReader;
    FakeProcessInspector liveInspector;
    FakeProcessMemoryMap liveMap;
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(liveReader, liveInspector, liveMap, config, mismatched);

    ResumeCommandResult result = controller.Resume();
    SH_CHECK(result.outcome == DiskScanOutcome::SessionMismatch);
    SH_CHECK(controller.Mode() == ScanMode::None);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Resume_CorruptOrMissingManifest_Rejected) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_resume_missing");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    ResumeCommandResult result = controller.Resume();
    SH_CHECK(result.outcome == DiskScanOutcome::CorruptFile);
    SH_CHECK(controller.Mode() == ScanMode::None);

    std::filesystem::remove_all(sessionDir);
}

// --- Cross-cutting controller invariants ------------------------------

SH_TEST(Controller_InMemoryThenDiskSession_NoCrossContamination) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_no_cross_contam");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);
    SH_CHECK(controller.Mode() == ScanMode::InMemory);
    SH_CHECK(controller.Status().inMemoryCandidateCount == 2);

    SH_CHECK(controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::PrivateReadable).scanOutcome ==
              DiskScanOutcome::CompleteCoverage);
    SH_CHECK(controller.Mode() == ScanMode::Disk);
    SH_CHECK(controller.Status().inMemoryCandidateCount == 0); // cleared by the disk session taking over

    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);
    SH_CHECK(controller.Mode() == ScanMode::InMemory);
    SH_CHECK(!controller.Status().diskManifest.has_value()); // cleared by the in-memory session taking over

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(Controller_OperationFailure_LeavesStateUnchanged) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_ctrl_failure_leaves_state");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)}); // 2 u32 values

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());

    BeginCommandResult first = controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(first.outcome == BeginCommandOutcome::Success);
    SH_CHECK(first.candidateCount == 2);

    // A subsequent Begin() that fails must not disturb the prior successful
    // session -- matches the original CLI's exact behavior.
    memoryMap.ForceResult(MemoryMapResult::EnumerationFailed);
    BeginCommandResult second = controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable);
    SH_CHECK(second.outcome == BeginCommandOutcome::ScanFailed);

    SH_CHECK(controller.Mode() == ScanMode::InMemory);
    SH_CHECK(controller.Status().inMemoryCandidateCount == 2);

    std::filesystem::remove_all(sessionDir);
}

// --- SignalProbeCommandProcessor -----------------------------------------

SH_TEST(SignalProbeCommandProcessor_UnhandledVerb_FallsThrough) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_proc_unhandled");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SignalProbeCommandProcessor processor(controller);

    for (const char* command : {"identity", "regions", "watch 0x1000 u32 hp", "mark checkpoint", "stop", "quit",
                                 "bogus-command"}) {
        auto result = processor.Process(command);
        SH_CHECK(!result.handled);
    }

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(SignalProbeCommandProcessor_MalformedFilterCommand_ReportsUsage) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_proc_malformed_filter");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SignalProbeCommandProcessor processor(controller);

    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);

    auto result = processor.Process("filter sideways");
    SH_CHECK(result.handled);
    SH_CHECK(!result.outputLines.empty());
    SH_CHECK(result.outputLines[0].find("usage") != std::string::npos);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(SignalProbeCommandProcessor_InvalidValueTypeOrScope_ReportsUsage) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_proc_invalid_type_scope");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SignalProbeCommandProcessor processor(controller);

    auto beginResult = processor.Process("begin u9000 not-a-real-scope");
    SH_CHECK(beginResult.handled);
    SH_CHECK(!beginResult.outputLines.empty());
    SH_CHECK(beginResult.outputLines[0].find("usage") != std::string::npos);
    SH_CHECK(controller.Mode() == ScanMode::None);

    auto planResult = processor.Process("plan f32");
    SH_CHECK(planResult.handled);
    SH_CHECK(!planResult.outputLines.empty());
    SH_CHECK(planResult.outputLines[0].find("usage") != std::string::npos);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(SignalProbeCommandProcessor_InvalidListCount_HandledGracefully) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_proc_invalid_list_count");

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SignalProbeCommandProcessor processor(controller);

    SH_CHECK(controller.Begin(CandidateValueType::U32, CandidateScanScope::PrivateReadable).outcome ==
              BeginCommandOutcome::Success);

    auto result = processor.Process("list not-a-number");
    SH_CHECK(result.handled);
    SH_CHECK(!result.outputLines.empty());
    SH_CHECK(result.outputLines[0].find("invalid count") != std::string::npos);

    auto defaultCountResult = processor.Process("list");
    SH_CHECK(defaultCountResult.handled);
    SH_CHECK(defaultCountResult.outputLines.size() == 2); // default count of 10, capped by the 2 real candidates

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(SignalProbeCommandProcessor_PlanBeginDiskFilterStatusList_RoundTrip) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_probe_proc_roundtrip");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    PokeU32(reader, 0x1000, 100);
    PokeU32(reader, 0x1004, 100);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;
    SignalProbeScanController controller(reader, inspector, memoryMap, config, MakeControllerIdentity());
    SignalProbeCommandProcessor processor(controller);

    auto planResult = processor.Process("plan u32 private-readable");
    SH_CHECK(planResult.handled);
    SH_CHECK(std::filesystem::is_empty(sessionDir)); // plan never creates files

    auto beginDiskResult = processor.Process("begin-disk u32 private-readable");
    SH_CHECK(beginDiskResult.handled);
    SH_CHECK(controller.Mode() == ScanMode::Disk);

    PokeU32(reader, 0x1000, 50);
    auto filterResult = processor.Process("filter decreased");
    SH_CHECK(filterResult.handled);
    SH_CHECK(controller.Status().diskManifest->generation == 1);

    auto statusResult = processor.Process("status");
    SH_CHECK(statusResult.handled);
    bool sawGeneration = false;
    for (const std::string& line : statusResult.outputLines) {
        if (line.find("generation=1") != std::string::npos) {
            sawGeneration = true;
        }
    }
    SH_CHECK(sawGeneration);

    auto listResult = processor.Process("list 10");
    SH_CHECK(listResult.handled);
    SH_CHECK(listResult.outputLines.size() == 1);

    auto exactFilterResult = processor.Process("filter exact 50");
    SH_CHECK(exactFilterResult.handled);

    std::filesystem::remove_all(sessionDir);
}
