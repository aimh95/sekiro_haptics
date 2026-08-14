// Real Win32 integration test for Win32ProcessReader: launches the actual
// helper executable (process_reader_helper_main.cpp) as a genuinely
// separate process, attaches to it with the real Win32Api implementation
// (no fakes here), and verifies the full read-only attach/read/detach
// lifecycle against it. No sleep is used to wait for the helper to become
// ready -- the parent blocks reading the helper's stdout, which the helper
// only writes once it has actually initialized (a pipe read is the
// synchronization). No game or elevated privileges are required; the
// helper is a small, harmless, self-contained process.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "HelperProcess.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using namespace sekiro_haptics::process;

namespace {
std::uint64_t ParseKeyValue(const std::string& token) {
    return ParseHelperKeyValue(token);
}
} // namespace

SH_TEST(Win32ProcessReader_Integration_RealHelperProcess_AttachReadDetachLifecycle) {
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));
    SH_CHECK(readyLine.rfind("READY ", 0) == 0);

    std::uint32_t pid = 0;
    std::uintptr_t addr = 0;
    std::size_t len = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, addrTok, lenTok;
        iss >> tag >> pidTok >> addrTok >> lenTok;
        pid = static_cast<std::uint32_t>(ParseKeyValue(pidTok));
        addr = static_cast<std::uintptr_t>(ParseKeyValue(addrTok));
        len = static_cast<std::size_t>(ParseKeyValue(lenTok));
    }
    SH_CHECK(pid == helper.Pid());
    SH_CHECK(len == 32);

    // 1-2: launch (done above) + PID-based read-only attach.
    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(pid) == ProcessReaderResult::Success);
    SH_CHECK(reader.IsAttached());
    SH_CHECK(reader.IsAlive());

    // 3-4: read the helper-provided address; every byte matches exactly.
    std::vector<std::uint8_t> buffer(len, 0);
    ProcessReaderResult readResult = reader.ReadBytes(addr, buffer.data(), buffer.size());
    SH_CHECK(readResult == ProcessReaderResult::Success);

    static const std::uint8_t kExpected[32] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x23, 0x45, 0x67,
        0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x20, 0x30, 0x40,
    };
    SH_CHECK(std::memcmp(buffer.data(), kExpected, sizeof(kExpected)) == 0);

    // 5: after the helper exits, IsAlive()/ReadBytes() reflect that -- no
    // sleep-based polling, WaitForExit() blocks on the real process handle.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    SH_CHECK(reader.IsAlive() == false);

    std::vector<std::uint8_t> afterExit(len, 0);
    ProcessReaderResult afterExitResult = reader.ReadBytes(addr, afterExit.data(), afterExit.size());
    SH_CHECK(afterExitResult == ProcessReaderResult::ProcessExited);

    // 6: detach -> subsequent reads report NotAttached.
    reader.Detach();
    SH_CHECK(reader.IsAttached() == false);
    ProcessReaderResult afterDetachResult = reader.ReadBytes(addr, afterExit.data(), afterExit.size());
    SH_CHECK(afterDetachResult == ProcessReaderResult::NotAttached);

    // 7: HelperProcess's destructor (below, end of scope) closes the
    // process/thread/pipe handles it owns; Win32ProcessReader already
    // closed its own handle inside Detach() above.
}

SH_TEST(Win32ProcessReader_Integration_AttachByExactFilename_FindsRealHelperProcess) {
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));

    Win32ProcessReader reader;
    ProcessReaderResult result = reader.AttachByName(SH_PROCESS_READER_HELPER_EXE_NAME);

    SH_CHECK(result == ProcessReaderResult::Success);
    SH_CHECK(reader.Pid() == helper.Pid());

    reader.Detach();
    helper.SendExit();
    helper.WaitForExit(5000);
}

SH_TEST(Win32ProcessReader_Integration_ExecutableIdentityAndMainModule) {
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));

    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(helper.Pid()) == ProcessReaderResult::Success);

    // 3: the helper's full executable path.
    std::filesystem::path imagePath;
    ProcessInspectionResult pathResult = reader.GetImagePath(imagePath);
    SH_CHECK(pathResult == ProcessInspectionResult::Success);
    SH_CHECK(!imagePath.empty());
    SH_CHECK(imagePath.filename() == std::filesystem::path(SH_PROCESS_READER_HELPER_EXE).filename());

    // 4: main module identified (not assumed to be enumeration's first entry).
    ModuleInfo mainModule;
    ProcessInspectionResult moduleResult = reader.GetMainModule(mainModule);
    SH_CHECK(moduleResult == ProcessInspectionResult::Success);

    // 5: base address non-zero, no range overflow.
    SH_CHECK(mainModule.baseAddress != 0);
    SH_CHECK(mainModule.imageSize != 0);
    SH_CHECK(mainModule.baseAddress + mainModule.imageSize > mainModule.baseAddress);

    // 6: read the module's first 2 bytes via the existing ReadBytes() --
    // every PE image starts with the DOS header magic "MZ".
    std::uint8_t peHeader[2] = {};
    ProcessReaderResult peReadResult = reader.ReadBytes(mainModule.baseAddress, peHeader, sizeof(peHeader));
    SH_CHECK(peReadResult == ProcessReaderResult::Success);
    SH_CHECK(peHeader[0] == 'M');
    SH_CHECK(peHeader[1] == 'Z');

    // 7-8: SHA-256 identity, built twice against the same running
    // instance, matches both times.
    ExecutableIdentity identity1;
    ExecutableIdentity identity2;
    SH_CHECK(BuildExecutableIdentity(reader, identity1) == ProcessInspectionResult::Success);
    SH_CHECK(BuildExecutableIdentity(reader, identity2) == ProcessInspectionResult::Success);

    SH_CHECK(identity1.sha256 == identity2.sha256);
    SH_CHECK(identity1.fileSizeBytes == identity2.fileSizeBytes);
    SH_CHECK(identity1.fileSizeBytes > 0);
    SH_CHECK(identity1.mainModuleBaseAddress == mainModule.baseAddress);

    // 9: after the helper exits, path/module/identity queries all report
    // ProcessExited -- no sleep, WaitForExit() blocks on the real handle.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    std::filesystem::path pathAfterExit;
    SH_CHECK(reader.GetImagePath(pathAfterExit) == ProcessInspectionResult::ProcessExited);

    ModuleInfo moduleAfterExit;
    SH_CHECK(reader.GetMainModule(moduleAfterExit) == ProcessInspectionResult::ProcessExited);

    ExecutableIdentity identityAfterExit;
    SH_CHECK(BuildExecutableIdentity(reader, identityAfterExit) == ProcessInspectionResult::ProcessExited);

    // 10: detach -> NotAttached.
    reader.Detach();
    std::filesystem::path pathAfterDetach;
    SH_CHECK(reader.GetImagePath(pathAfterDetach) == ProcessInspectionResult::NotAttached);

    // 11: HelperProcess's destructor (end of scope) closes its own
    // process/thread/pipe handles; Win32ProcessReader already released its
    // handle inside Detach() above.
}
