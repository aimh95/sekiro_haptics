// Real Win32 integration test for SEK-PROBE-001C-2's disk-backed scan path
// wired into SignalProbeScanController: launches the actual helper
// executable (process_reader_helper_main.cpp, shared with the other
// process/AOB/probe integration tests), attaches read-only, runs the real
// plan -> begin-disk -> filter chain against real ReadProcessMemory calls,
// simulates a CLI restart via a second, independent controller instance,
// resumes the on-disk session, and confirms a post-exit filter attempt
// never corrupts the last complete generation file. No sleep anywhere --
// HelperProcess's pipe/process-handle synchronization is reused as-is.
//
// g_watchIncrement and g_watchDecrement (process_reader_helper_main.cpp)
// both start at exactly 0xCAFE1000 -- a distinctive magic value, not a
// common small integer -- so "filter exact 0xCAFE1000" reliably seeds
// exactly a 2-candidate generation as the first filter (a fresh baseline's
// first filter may not be "unchanged", see DiskCandidateScanner.hpp's
// InitialFilterTooBroad, but "exact" is allowed). An earlier version of
// this test used a plain 1000 for both, which turned out to coincidentally
// also match unrelated compiler-generated data elsewhere in the real
// module -- the first time this test actually ran against a real Windows
// process, it caught exactly this (see docs/06-signal-discovery-probe.md).
//
// NOTE: this file is registered in tests/CMakeLists.txt only inside the
// if(TARGET sekiro_haptics_win32_process) block and has never been
// executed in this Linux development sandbox -- it requires a real
// Windows process to attach to. See docs/06-signal-discovery-probe.md.

#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/SignalProbeScanController.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "HelperProcess.hpp"

#include <filesystem>
#include <sstream>
#include <string>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path DiskIntegrationSessionDir() {
    return std::filesystem::temp_directory_path() / "sh_disk_candidate_scanner_integration_tests";
}

} // namespace

SH_TEST(DiskCandidateScanner_Integration_RealHelperProcess_PlanBeginDiskFilterChainAndResume) {
    // 1: launch the real helper process.
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));
    SH_CHECK(readyLine.rfind("READY ", 0) == 0);

    std::uint32_t pid = 0;
    std::uintptr_t incrementAddr = 0, decrementAddr = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, addrTok, lenTok, aobAddrTok, aobLenTok, matchOffsetTok, targetTok, sentinelTok,
            incrementTok, decrementTok, toggleTok, noiseTok;
        iss >> tag >> pidTok >> addrTok >> lenTok >> aobAddrTok >> aobLenTok >> matchOffsetTok >> targetTok >>
            sentinelTok >> incrementTok >> decrementTok >> toggleTok >> noiseTok;
        pid = static_cast<std::uint32_t>(ParseHelperKeyValue(pidTok));
        incrementAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(incrementTok));
        decrementAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(decrementTok));
    }

    // 2: PID-based read-only attach.
    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(pid) == ProcessReaderResult::Success);

    ExecutableIdentity identity;
    SH_CHECK(BuildExecutableIdentity(reader, identity) == ProcessInspectionResult::Success);
    ModuleInfo mainModule;
    SH_CHECK(reader.GetMainModule(mainModule) == ProcessInspectionResult::Success);

    ScanControllerIdentity controllerIdentity;
    controllerIdentity.executableFileSizeBytes = identity.fileSizeBytes;
    controllerIdentity.sha256Hex = ToHex(identity.sha256);
    controllerIdentity.pid = reader.Pid();
    controllerIdentity.mainModuleBaseAddress = mainModule.baseAddress;
    controllerIdentity.mainModuleImageSize = mainModule.imageSize;

    std::filesystem::path sessionDir = DiskIntegrationSessionDir();
    std::error_code rmEc;
    std::filesystem::remove_all(sessionDir, rmEc);
    std::filesystem::create_directories(sessionDir);

    SignalProbeControllerConfig config;
    config.outputDir = sessionDir;

    SignalProbeScanController controller(reader, reader, reader, config, controllerIdentity);

    // 3: plan -- must not touch disk or the target process's memory content.
    PlanCommandResult planResult = controller.Plan(CandidateValueType::U32, CandidateScanScope::MainModule);
    SH_CHECK(planResult.outcome == PlanCandidateScanOutcome::Success);
    SH_CHECK(planResult.report.regionCount > 0);

    // 4: begin-disk over the real main module.
    BeginDiskCommandResult beginDiskResult =
        controller.BeginDisk(CandidateValueType::U32, CandidateScanScope::MainModule);
    SH_CHECK(beginDiskResult.ranScan);
    SH_CHECK(beginDiskResult.scanOutcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(controller.Mode() == ScanMode::Disk);

    // 5: "exact 0xCAFE1000" as the first filter -- both watchIncrement and
    // watchDecrement start at exactly that distinctive value, so both (and
    // only those two) survive into generation 1.
    CandidateValue exactStart = std::uint32_t{0xCAFE1000};
    FilterCommandResult exactResult = controller.Filter(CandidateFilterKind::ExactValue, &exactStart);
    SH_CHECK(exactResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(exactResult.diskStats.survivingCandidateCount == 2);
    SH_CHECK(controller.Status().diskManifest->generation == 1);

    // 6: DECREMENT then INCREMENT via real pipe commands, each acked.
    SH_CHECK(helper.SendCommandAndWaitForAck("DECREMENT")); // watchDecrement 0xCAFE1000 -> 0xCAFE0FF6
    SH_CHECK(helper.SendCommandAndWaitForAck("INCREMENT")); // watchIncrement 0xCAFE1000 -> 0xCAFE100A

    // 7: "increased" -- only watchIncrement survives (watchDecrement went down).
    FilterCommandResult increasedResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    SH_CHECK(increasedResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(increasedResult.diskStats.survivingCandidateCount == 1);
    SH_CHECK(controller.Status().diskManifest->generation == 2);
    {
        std::vector<ListEntry> entries = controller.List(10);
        SH_CHECK(entries.size() == 1);
        SH_CHECK(entries[0].address == incrementAddr);
    }

    // 8: "unchanged" with no intervening command -- the sole survivor's
    // value hasn't moved since the last read, so it must still be there.
    FilterCommandResult unchangedResult = controller.Filter(CandidateFilterKind::Unchanged, nullptr);
    SH_CHECK(unchangedResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(unchangedResult.diskStats.survivingCandidateCount == 1);
    SH_CHECK(controller.Status().diskManifest->generation == 3);

    // 9: INCREMENT again, then "increased" -- generation 4.
    SH_CHECK(helper.SendCommandAndWaitForAck("INCREMENT")); // watchIncrement 0xCAFE100A -> 0xCAFE1014
    FilterCommandResult increasedAgainResult = controller.Filter(CandidateFilterKind::Increased, nullptr);
    SH_CHECK(increasedAgainResult.outcome == FilterCommandOutcome::Success);
    SH_CHECK(increasedAgainResult.diskStats.survivingCandidateCount == 1);
    SH_CHECK(controller.Status().diskManifest->generation == 4);
    int lastGoodGeneration = controller.Status().diskManifest->generation;

    // 10: a second, independent controller -- simulates a CLI restart
    // (fresh process, fresh reader, fresh state) pointed at the same
    // on-disk session directory and the same stable identity.
    Win32ProcessReader restartedReader;
    SH_CHECK(restartedReader.AttachByPid(pid) == ProcessReaderResult::Success);
    SignalProbeScanController restartedController(restartedReader, restartedReader, restartedReader, config,
                                                    controllerIdentity);
    SH_CHECK(restartedController.Mode() == ScanMode::None); // nothing carried over from the old instance

    // 11: resume.
    ResumeCommandResult resumeResult = restartedController.Resume();
    SH_CHECK(resumeResult.outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(restartedController.Mode() == ScanMode::Disk);
    SH_CHECK(resumeResult.manifest.generation == lastGoodGeneration);
    SH_CHECK(resumeResult.manifest.candidateCount == 1);

    // 12: status/list sanity checks against the resumed session.
    StatusSnapshot resumedStatus = restartedController.Status();
    SH_CHECK(resumedStatus.mode == ScanMode::Disk);
    SH_CHECK(resumedStatus.diskManifest.has_value());
    SH_CHECK(resumedStatus.diskManifest->generation == lastGoodGeneration);
    std::vector<ListEntry> resumedEntries = restartedController.List(10);
    SH_CHECK(resumedEntries.size() == 1);
    SH_CHECK(resumedEntries[0].address == incrementAddr);

    // 13: exit the helper -- real process-exit synchronization, no sleep.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    // 14: a further filter attempt against the now-exited process must
    // fail closed, never fabricate a result.
    FilterCommandResult afterExitResult = restartedController.Filter(CandidateFilterKind::Increased, nullptr);
    SH_CHECK(afterExitResult.outcome == FilterCommandOutcome::DiskFilterFailed);
    SH_CHECK(afterExitResult.diskResult == DiskScanOutcome::ProcessExited);

    // 15: detach -> a subsequent operation still fails closed. Note this is
    // NOT NotAttached: step 14's ProcessExited failure already persisted
    // manifest.state = Interrupted to disk, and FilterDiskCandidates()
    // checks manifest.state == CandidatesComplete *before* ever touching
    // the reader -- so once a session is interrupted, every further filter
    // attempt is rejected on that stale state alone (CorruptFile) rather
    // than re-discovering whatever the reader's own current problem is.
    // That's the correct, defensible behavior (no more generations may be
    // layered onto an already-interrupted chain without an explicit
    // Resume() first) -- it just means this assertion cannot observe
    // NotAttached specifically here.
    restartedReader.Detach();
    reader.Detach();
    FilterCommandResult afterDetachResult = restartedController.Filter(CandidateFilterKind::Increased, nullptr);
    SH_CHECK(afterDetachResult.outcome == FilterCommandOutcome::DiskFilterFailed);
    SH_CHECK(afterDetachResult.diskResult == DiskScanOutcome::CorruptFile);

    // 16: the failed post-exit/post-detach filter attempts above must never
    // have clobbered the last known-good complete generation file -- verify
    // by reading it directly, independent of the controller.
    std::string currentFileName = CurrentDataFileName(lastGoodGeneration);
    SH_CHECK(std::filesystem::exists(sessionDir / currentFileName));
    SH_CHECK(!std::filesystem::exists(sessionDir / (currentFileName + ".tmp")));
    GenerationReader directReader(sessionDir / currentFileName);
    SH_CHECK(directReader.Open(1, CandidateValueType::U32, lastGoodGeneration) == CandidateStorageResult::Success);
    std::uint64_t verifiedAddress = 0;
    std::uint8_t verifiedValueBytes[4] = {};
    SH_CHECK(directReader.NextRecord(verifiedAddress, verifiedValueBytes));
    SH_CHECK(verifiedAddress == incrementAddr);
    SH_CHECK(!directReader.NextRecord(verifiedAddress, verifiedValueBytes)); // exactly one record

    directReader.Close();
    std::filesystem::remove_all(sessionDir);
}
