// Real Linux child-process integration test for SEK-PROBE-001C-LINUX-E2E:
// exercises the actual, unmodified DiskCandidateScanner/CandidateStorage/
// ScanManifest/SignalProbeScanController code against a genuinely separate
// process's real memory (process_vm_readv + /proc/<pid>/maps), standing in
// for the real Sekiro/Windows attach this Linux sandbox cannot perform.
// No FakeProcessReader anywhere in this file. Small arena/memory budget
// here (kept fast for CTest) -- the same scenario runs at a much larger
// scale via the manual `sekiro_haptics_linux_probe_e2e` runner
// (linux_probe_e2e_main.cpp), sharing this exact implementation via
// LinuxProbeE2EScenario.cpp so the two paths can never silently diverge.

#include "LinuxChildProcessReader.hpp"
#include "LinuxHelperProcess.hpp"
#include "LinuxProbeE2EScenario.hpp"
#include "testing.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

} // namespace

SH_TEST(DiskCandidateScanner_LinuxE2E_RealChildProcess_DamageUnchangedHealCycle) {
    LinuxProbeE2EConfig config;
    config.helperExePath = SH_LINUX_SIGNAL_PROBE_HELPER_EXE;
    config.arenaSizeBytes = 1 * 1024 * 1024; // 1 MiB -- fast for CTest
    // 4 MiB: still well below the ~6 MiB in-memory Candidate-vector
    // estimate for this arena (262144 u32 values * sizeof(Candidate)),
    // forcing the disk-backed path, but comfortably above
    // DiskCandidateScanner's own fixed internal chunk-buffer floor
    // (~kCandidateScanChunkBytes-scale, independent of this budget value)
    // so peakBufferedBytes <= configured budget holds meaningfully rather
    // than being an unsatisfiable floor mismatch.
    config.memoryBudgetBytes = 4 * 1024 * 1024;
    config.outputDir = FreshDir("sh_linux_probe_e2e_ctest");

    LinuxProbeE2EReport report = RunLinuxProbeE2EScenario(config);

    SH_CHECK(!report.permissionDenied); // see the ticket's stop-and-report-EPERM requirement
    SH_CHECK(report.handshakeOk);
    SH_CHECK(report.procMapsValidationOk);
    SH_CHECK(report.directCrossProcessReadOk);
    SH_CHECK(report.directReadHpValue == 1000);
    SH_CHECK(report.planOk);
    SH_CHECK(report.diskBackedRecommended);
    SH_CHECK(report.baselineCompleteCoverage);
    SH_CHECK(report.baselineProcessedBytes == config.arenaSizeBytes);
    SH_CHECK(report.baselineCoveragePercent > 99.9);
    SH_CHECK(report.baselineFileSizeBytes > 0);
    SH_CHECK(report.baselinePeakBufferedBytes <= config.memoryBudgetBytes);
    SH_CHECK(report.firstDecreasedOk);
    SH_CHECK(report.firstDecreasedCandidateCount == 2);
    SH_CHECK(report.unchangedOk);
    SH_CHECK(report.unchangedCandidateCount == 1);
    SH_CHECK(report.finalIncreasedOk);
    SH_CHECK(report.finalIncreasedCandidateCount == 1);
    SH_CHECK(report.finalAddressMatches);
    SH_CHECK(report.finalHpAddress == report.expectedHpAddress);
    SH_CHECK(report.finalHpValue == 950);
    SH_CHECK(report.processExitedContractOk);
    SH_CHECK(report.notAttachedContractOk);
    SH_CHECK(report.secondFilterAfterInterruptedRejectedOk);
    SH_CHECK(report.lastGenerationFileIntact);
    SH_CHECK(report.success);
    if (!report.success) {
        // Surfaced via the assertion trail above too, but keep the exact
        // reason visible in test output for fast diagnosis.
        SH_CHECK(report.failureReason.empty());
    }

    std::filesystem::remove_all(config.outputDir);
}

SH_TEST(DiskCandidateScanner_LinuxE2E_MalformedHandshake_ReportsCleanFailureWithoutCrashing) {
    LinuxProbeE2EConfig config;
    // /bin/echo is not our real helper -- it never speaks the READY
    // protocol, so this exercises the scenario's handshake-parsing
    // failure path against a real (if uncooperative) child process rather
    // than a Fake. Still our own direct child (fork/exec), never an
    // arbitrary pre-existing system process, and never actually attached
    // to for memory reads (the scenario fails before that point).
    config.helperExePath = "/bin/echo";
    config.arenaSizeBytes = 4096;
    config.memoryBudgetBytes = 256;
    config.outputDir = FreshDir("sh_linux_probe_e2e_malformed");

    LinuxProbeE2EReport report = RunLinuxProbeE2EScenario(config);

    SH_CHECK(!report.success);
    SH_CHECK(!report.handshakeOk);
    SH_CHECK(!report.failureReason.empty());

    std::filesystem::remove_all(config.outputDir);
}

SH_TEST(DiskCandidateScanner_LinuxE2E_HelperExitsImmediatelyAfterReady_LivenessDetectedCorrectly) {
    LinuxHelperProcess helper;
    SH_CHECK(helper.Start(SH_LINUX_SIGNAL_PROBE_HELPER_EXE, {"4096"}));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));
    SH_CHECK(readyLine.rfind("READY ", 0) == 0);

    pid_t pid = 0;
    std::uintptr_t arenaBase = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, baseTok;
        iss >> tag >> pidTok >> baseTok;
        pid = static_cast<pid_t>(ParseLinuxHelperKeyValue(pidTok));
        arenaBase = static_cast<std::uintptr_t>(ParseLinuxHelperKeyValue(baseTok));
    }

    LinuxChildProcessReader reader(pid, arenaBase, 4096);
    SH_CHECK(reader.AttachByPid(static_cast<std::uint32_t>(pid)) == ProcessReaderResult::Success);
    SH_CHECK(reader.IsAlive());

    // No DAMAGE/HEAL/scan attempted -- exit immediately after the handshake.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit());

    SH_CHECK(!reader.IsAlive());
    std::uint32_t discard = 0;
    ProcessReaderResult afterExitRead = reader.ReadBytes(arenaBase, &discard, sizeof(discard));
    SH_CHECK(afterExitRead == ProcessReaderResult::ProcessExited);

    reader.Detach();
    ProcessReaderResult afterDetachRead = reader.ReadBytes(arenaBase, &discard, sizeof(discard));
    SH_CHECK(afterDetachRead == ProcessReaderResult::NotAttached);
}

SH_TEST(LinuxChildProcessReader_ReadBytes_PartialAndFullFailureAtUnmappedBoundary) {
    constexpr std::size_t kPageBytes = 4096;
    void* mapped = mmap(nullptr, kPageBytes * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    SH_CHECK(mapped != MAP_FAILED);

    auto* bytes = static_cast<unsigned char*>(mapped);
    for (std::size_t i = 0; i < kPageBytes; ++i) {
        bytes[i] = static_cast<unsigned char>(0xAB);
    }

    // Deterministically unmap the second page so the boundary at
    // +kPageBytes is guaranteed unmapped, rather than relying on
    // environment-dependent adjacent-mapping luck.
    SH_CHECK(munmap(bytes + kPageBytes, kPageBytes) == 0);

    std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mapped);
    // A process is always allowed to process_vm_readv itself -- this is a
    // real invocation of the same syscall path used against a genuinely
    // separate process, exercising ReadBytes()'s success/partial/failure
    // branches deterministically rather than relying on real cross-process
    // page-layout luck.
    LinuxChildProcessReader selfReader(getpid(), base, kPageBytes);
    SH_CHECK(selfReader.AttachByPid(static_cast<std::uint32_t>(getpid())) == ProcessReaderResult::Success);

    // Full success: entirely within the mapped page.
    unsigned char fullBuf[4] = {};
    SH_CHECK(selfReader.ReadBytes(base, fullBuf, sizeof(fullBuf)) == ProcessReaderResult::Success);
    SH_CHECK(std::memcmp(fullBuf, "\xAB\xAB\xAB\xAB", 4) == 0);

    // Partial: requests 8 bytes starting 4 bytes before the mapped page
    // ends -- only the first 4 are actually transferable.
    unsigned char partialBuf[8] = {};
    ProcessReaderResult partialResult = selfReader.ReadBytes(base + kPageBytes - 4, partialBuf, sizeof(partialBuf));
    SH_CHECK(partialResult == ProcessReaderResult::PartialRead);

    // Full failure: entirely within the now-unmapped second page.
    unsigned char failBuf[4] = {};
    ProcessReaderResult failResult = selfReader.ReadBytes(base + kPageBytes, failBuf, sizeof(failBuf));
    SH_CHECK(failResult == ProcessReaderResult::ReadFailed);
    SH_CHECK(selfReader.LastErrno() != 0);

    munmap(mapped, kPageBytes); // release the still-mapped first page
}

SH_TEST(LinuxChildProcessReader_ClaimedArenaLargerThanRealMapping_ValidationFails) {
    LinuxHelperProcess helper;
    SH_CHECK(helper.Start(SH_LINUX_SIGNAL_PROBE_HELPER_EXE, {"4096"}));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));

    pid_t pid = 0;
    std::uintptr_t arenaBase = 0;
    std::size_t arenaSize = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, baseTok, sizeTok;
        iss >> tag >> pidTok >> baseTok >> sizeTok;
        pid = static_cast<pid_t>(ParseLinuxHelperKeyValue(pidTok));
        arenaBase = static_cast<std::uintptr_t>(ParseLinuxHelperKeyValue(baseTok));
        arenaSize = static_cast<std::size_t>(ParseLinuxHelperKeyValue(sizeTok));
    }

    // Claim a region far larger than what the helper actually mapped --
    // EnumerateReadableRegions() must refuse to vouch for a range /proc/
    // <pid>/maps doesn't actually back, never trusting the caller-supplied
    // size blindly.
    LinuxChildProcessReader reader(pid, arenaBase, arenaSize * 1000);
    SH_CHECK(reader.AttachByPid(static_cast<std::uint32_t>(pid)) == ProcessReaderResult::Success);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);
    SH_CHECK(result == MemoryMapResult::EnumerationFailed);
    SH_CHECK(regions.empty());

    helper.SendExit();
    SH_CHECK(helper.WaitForExit());
}
