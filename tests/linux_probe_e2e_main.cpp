// Manual Linux end-to-end runner for SEK-PROBE-001C-LINUX-E2E, for a human
// (or Claude) to run directly in this Linux development sandbox at a
// realistic scale -- not part of `ctest`. Shares its entire scenario
// implementation with the small, fast CTest-registered integration test
// (test_disk_candidate_scanner_linux_e2e.cpp) via LinuxProbeE2EScenario.cpp,
// so this and the automated test can never silently diverge. Never prints
// raw arena memory contents -- only counts, addresses, sizes, and
// pass/fail booleans.

#include "LinuxProbeE2EScenario.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

using namespace sekiro_haptics::process;

namespace {

struct RunnerOptions {
    std::size_t arenaMiB = 256;
    std::size_t memoryBudgetMiB = 64;
    bool keepOutput = false;
};

bool ParseArgs(int argc, char** argv, RunnerOptions& out, std::string& outError) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto takeValue = [&](const char* flag, std::string& outValue) -> bool {
            if (i + 1 >= argc) {
                outError = std::string(flag) + " requires a value";
                return false;
            }
            outValue = argv[++i];
            return true;
        };

        if (arg == "--arena-mib") {
            std::string value;
            if (!takeValue("--arena-mib", value)) return false;
            out.arenaMiB = static_cast<std::size_t>(std::stoull(value));
        } else if (arg == "--memory-budget-mib") {
            std::string value;
            if (!takeValue("--memory-budget-mib", value)) return false;
            out.memoryBudgetMiB = static_cast<std::size_t>(std::stoull(value));
        } else if (arg == "--keep-output") {
            out.keepOutput = true;
        } else {
            outError = "unknown argument: " + arg;
            return false;
        }
    }
    return true;
}

std::string BoolWord(bool value) {
    return value ? "yes" : "no";
}

} // namespace

int main(int argc, char** argv) {
    RunnerOptions options;
    std::string parseError;
    if (!ParseArgs(argc, argv, options, parseError)) {
        std::cerr << parseError << "\n";
        std::cerr << "usage: sekiro_haptics_linux_probe_e2e [--arena-mib N] [--memory-budget-mib N] [--keep-output]\n";
        return 2;
    }

    char tmpTemplate[] = "/tmp/sh_linux_probe_e2e_XXXXXX";
    char* createdDir = mkdtemp(tmpTemplate);
    if (createdDir == nullptr) {
        std::cerr << "mkdtemp failed: " << std::strerror(errno) << "\n";
        return 2;
    }
    std::filesystem::path outputDir(createdDir);

    LinuxProbeE2EConfig config;
    config.helperExePath = SH_LINUX_SIGNAL_PROBE_HELPER_EXE;
    config.arenaSizeBytes = options.arenaMiB * 1024ULL * 1024ULL;
    config.memoryBudgetBytes = options.memoryBudgetMiB * 1024ULL * 1024ULL;
    config.outputDir = outputDir;

    std::cout << "=== SEK-PROBE-001C-LINUX-E2E manual runner ===\n";
    std::cout << "arena: " << options.arenaMiB << " MiB, memory budget: " << options.memoryBudgetMiB << " MiB\n";
    std::cout << "output dir: " << outputDir.string() << "\n\n";

    LinuxProbeE2EReport report = RunLinuxProbeE2EScenario(config);

    std::cout << "child PID:                      " << report.childPid << "\n";
    std::cout << "arena size:                     " << report.arenaSizeBytes << " bytes\n";
    std::cout << "real cross-process read OK:     " << BoolWord(report.directCrossProcessReadOk)
               << " (playerHp=" << report.directReadHpValue << ")\n";
    std::cout << "plan OK / disk-backed selected: " << BoolWord(report.planOk) << " / "
               << BoolWord(report.diskBackedRecommended) << "\n";
    std::cout << "baseline complete coverage:     " << BoolWord(report.baselineCompleteCoverage) << "\n";
    std::cout << "baseline processed bytes:       " << report.baselineProcessedBytes << "\n";
    std::cout << "baseline coverage percent:      " << report.baselineCoveragePercent << "\n";
    std::cout << "baseline file size:             " << report.baselineFileSizeBytes << " bytes\n";
    std::cout << "peak buffered bytes:            " << report.baselinePeakBufferedBytes << " (budget "
               << report.configuredMemoryBudgetBytes << ")\n";
    std::cout << "first decreased candidates:     " << report.firstDecreasedCandidateCount << "\n";
    std::cout << "negative-control unchanged:     " << report.unchangedCandidateCount << "\n";
    std::cout << "final increased candidates:     " << report.finalIncreasedCandidateCount << "\n";
    std::cout << "final HP address matches:       " << BoolWord(report.finalAddressMatches) << " (expected 0x"
               << std::hex << report.expectedHpAddress << ", got 0x" << report.finalHpAddress << std::dec
               << ", value=" << report.finalHpValue << ")\n";
    std::cout << "post-exit -> ProcessExited:     " << BoolWord(report.processExitedContractOk) << "\n";
    std::cout << "post-detach -> NotAttached:     " << BoolWord(report.notAttachedContractOk) << "\n";
    std::cout << "2nd filter after Interrupted rejected: " << BoolWord(report.secondFilterAfterInterruptedRejectedOk)
               << "\n";
    std::cout << "last generation file intact:    " << BoolWord(report.lastGenerationFileIntact) << "\n";
    std::cout << "temp output path:               " << outputDir.string() << "\n";

    if (!report.success) {
        std::cout << "failure reason:                 " << report.failureReason << "\n";
        if (report.permissionDenied) {
            std::cout << "\nPROCESS_VM_READV EPERM -- stopping without sudo or Yama ptrace_scope changes, as "
                          "required. Check /proc/sys/kernel/yama/ptrace_scope and sandbox ptrace/seccomp policy.\n";
        }
    }

    std::cout << "\n=== " << (report.success ? "PASS" : "FAIL") << " ===\n";

    if (options.keepOutput) {
        std::cout << "(--keep-output: leaving " << outputDir.string() << " in place)\n";
    } else {
        std::error_code ec;
        std::filesystem::remove_all(outputDir, ec);
    }

    return report.success ? 0 : 1;
}
