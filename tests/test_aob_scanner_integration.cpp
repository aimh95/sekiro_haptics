// Real Win32 integration test for AobScanner/RipRelative: launches the
// actual helper executable (process_reader_helper_main.cpp), attaches
// read-only via the real Win32ProcessReader, scans its self-reported AOB
// buffer range for a unique pattern straddling a real chunk boundary, and
// resolves a synthetic (made up for this test only, not any real
// instruction encoding or game data) RIP-relative displacement against
// the helper's own reported target address. No sleep is used -- the
// existing pipe/process-handle synchronization from HelperProcess.hpp is
// reused as-is.

#include "sekiro_haptics/process/AobScanner.hpp"
#include "sekiro_haptics/process/RipRelative.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "HelperProcess.hpp"

#include <sstream>
#include <string>

using namespace sekiro_haptics::process;

SH_TEST(AobScanner_Integration_RealHelperProcess_ScanAndResolveRipRelative) {
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));
    SH_CHECK(readyLine.rfind("READY ", 0) == 0);

    std::uint32_t pid = 0;
    std::uintptr_t aobAddr = 0;
    std::size_t aobLen = 0;
    std::size_t matchOffset = 0;
    std::uintptr_t expectedTarget = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, addrTok, lenTok, aobAddrTok, aobLenTok, matchOffsetTok, targetTok;
        iss >> tag >> pidTok >> addrTok >> lenTok >> aobAddrTok >> aobLenTok >> matchOffsetTok >> targetTok;
        pid = static_cast<std::uint32_t>(ParseHelperKeyValue(pidTok));
        aobAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(aobAddrTok));
        aobLen = static_cast<std::size_t>(ParseHelperKeyValue(aobLenTok));
        matchOffset = static_cast<std::size_t>(ParseHelperKeyValue(matchOffsetTok));
        expectedTarget = static_cast<std::uintptr_t>(ParseHelperKeyValue(targetTok));
    }

    // 1-2: launch (done above) + PID-based read-only attach.
    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(pid) == ProcessReaderResult::Success);

    // 3-4: scan exactly the helper-reported AOB range; the pattern
    // straddles a real chunk boundary (see process_reader_helper_main.cpp)
    // and must be found exactly once.
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("AB CD 11 22 ?? ?? ?? ?? EF 99", pattern) == AobScanResult::Success);

    std::uintptr_t matchAddress = 0;
    AobScanResult scanResult = ScanProcessRange(reader, aobAddr, aobLen, pattern, matchAddress);
    SH_CHECK(scanResult == AobScanResult::Success);

    // 5: the returned address matches the helper's own reported offset.
    SH_CHECK(matchAddress == aobAddr + matchOffset);

    // 6: resolve the synthetic rel32 embedded in the pattern; target must
    // match the helper's independently-reported target address exactly.
    ModuleInfo mainModule;
    SH_CHECK(reader.GetMainModule(mainModule) == ProcessInspectionResult::Success);

    RipRelativeSpec spec;
    spec.matchAddress = matchAddress;
    spec.instructionOffset = 2;
    spec.displacementOffset = 4;
    spec.instructionLength = 6;
    spec.allowedSourceRangeBase = aobAddr;
    spec.allowedSourceRangeSize = aobLen;
    spec.allowedTargetRangeBase = mainModule.baseAddress;
    spec.allowedTargetRangeSize = mainModule.imageSize;

    std::uintptr_t computedTarget = 0;
    AobScanResult ripResult = ResolveRipRelativeFromProcess(reader, spec, computedTarget);
    SH_CHECK(ripResult == AobScanResult::Success);
    SH_CHECK(computedTarget == expectedTarget);

    // 7: after the helper exits, a scan against the same (still attached,
    // but no longer alive) reader must not succeed -- no sleep, WaitForExit()
    // blocks on the real process handle.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    std::uintptr_t addressAfterExit = 0;
    AobScanResult scanAfterExit = ScanProcessRange(reader, aobAddr, aobLen, pattern, addressAfterExit);
    SH_CHECK(scanAfterExit == AobScanResult::ProcessExited);

    // 8: detach -> subsequent scans report NotAttached.
    reader.Detach();
    std::uintptr_t addressAfterDetach = 0;
    AobScanResult scanAfterDetach = ScanProcessRange(reader, aobAddr, aobLen, pattern, addressAfterDetach);
    SH_CHECK(scanAfterDetach == AobScanResult::NotAttached);

    // 9: HelperProcess's destructor (end of scope) closes its own
    // process/thread/pipe handles; Win32ProcessReader already released its
    // handle inside Detach() above.
}
