// Full-path integration test for AddressResolver: launches the real helper
// process, computes its ExecutableIdentity, loads a temporary profile JSON
// that exactly matches that identity, then resolves addresses via the real
// Win32ProcessReader against the real helper's memory. Validates:
//   1. Profile load → exact build selection → resolve → expected address
//   2. After helper exits → resolve fails (ProcessExited/ReadFailed)
//   3. After detach → resolve fails (NotAttached/ReadFailed)
// No sleep, no real game, no real Sekiro data, no GUI.

#include "sekiro_haptics/process/AddressResolver.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/SignatureProfileRepository.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "HelperProcess.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path IntegrationTestDir() {
    return std::filesystem::temp_directory_path() / "sh_resolver_integration";
}

std::filesystem::path WriteIntegrationJson(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(IntegrationTestDir());
    auto path = IntegrationTestDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

} // namespace

SH_TEST(AddressResolver_Integration_FullPath_ProfileToResolvedAddress) {
    // 1. Launch helper process
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

    // 2. Attach read-only
    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(pid) == ProcessReaderResult::Success);

    // 3. Compute the helper's ExecutableIdentity
    ExecutableIdentity identity;
    SH_CHECK(BuildExecutableIdentity(reader, identity) == ProcessInspectionResult::Success);
    SH_CHECK(identity.fileSizeBytes > 0);

    // 4. Get main module info for building the profile
    ModuleInfo mainModule;
    SH_CHECK(reader.GetMainModule(mainModule) == ProcessInspectionResult::Success);

    // 5. Build a temporary profile JSON matching this exact identity.
    // The AOB pattern "AB CD 11 22 ?? ?? ?? ?? EF 99" and RIP-relative
    // layout match process_reader_helper_main.cpp's g_aobBuffer.
    // scan range = [aobAddr - mainModule.baseAddress, aobLen)
    std::size_t scanOffsetRel = static_cast<std::size_t>(aobAddr - mainModule.baseAddress);
    // target range = entire module
    std::ostringstream profileJson;
    profileJson << "{\"profiles\":[{\"schemaVersion\":1,\"profileId\":\"integration-test-profile\","
                << "\"buildIdentity\":{\"fileSizeBytes\":\"0x" << std::hex << identity.fileSizeBytes
                << "\",\"sha256\":\"" << ToHex(identity.sha256) << "\"},"
                << "\"addresses\":[{\"addressId\":\"helper-rip-target\","
                << "\"moduleName\":\"" << SH_PROCESS_READER_HELPER_EXE_NAME << "\","
                << "\"scanOffset\":\"0x" << std::hex << scanOffsetRel << "\","
                << "\"scanSize\":\"0x" << std::hex << aobLen << "\","
                << "\"pattern\":\"AB CD 11 22 ?? ?? ?? ?? EF 99\","
                << "\"resolutionKind\":\"rip_relative_32\","
                << "\"instructionOffset\":\"0x2\","
                << "\"displacementOffset\":\"0x4\","
                << "\"instructionLength\":\"0x6\","
                << "\"targetRangeOffset\":\"0x0\","
                << "\"targetRangeSize\":\"0x" << std::hex << mainModule.imageSize << "\""
                << "}]}]}";

    auto profilePath = WriteIntegrationJson("resolver_integration.json", profileJson.str());
    SignatureProfileRepository repo;
    auto loadOutcome = repo.LoadFromFile(profilePath.string());
    SH_CHECK(loadOutcome.ok);
    SH_CHECK(loadOutcome.loadedCount == 1);

    // 6. Select profile and resolve
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, reader, reader);
    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].addressId == "helper-rip-target");
    SH_CHECK(outcome.addresses[0].address == expectedTarget);

    // 7. After helper exits, resolve must fail
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    ProfileResolutionOutcome outcomeAfterExit = ResolveAddressesForIdentity(repo, identity, reader, reader);
    // Profile selection still works (it's identity-based, not process-based)
    SH_CHECK(outcomeAfterExit.buildSupported == true);
    // But address resolution fails because process exited -- could be
    // ModuleNotFound (if module enumeration fails on dead process) or
    // ProcessReadFailed (if scan/read fails)
    SH_CHECK(outcomeAfterExit.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcomeAfterExit.addresses[0].reason == AddressResolutionReason::ProcessReadFailed ||
             outcomeAfterExit.addresses[0].reason == AddressResolutionReason::ModuleNotFound);

    // 8. After detach, resolve must also fail
    reader.Detach();
    ProfileResolutionOutcome outcomeAfterDetach = ResolveAddressesForIdentity(repo, identity, reader, reader);
    SH_CHECK(outcomeAfterDetach.buildSupported == true);
    SH_CHECK(outcomeAfterDetach.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcomeAfterDetach.addresses[0].reason == AddressResolutionReason::ProcessReadFailed ||
             outcomeAfterDetach.addresses[0].reason == AddressResolutionReason::ModuleNotFound);

    // 9. Cleanup: profile file
    std::filesystem::remove(profilePath);
    std::filesystem::remove(IntegrationTestDir());
}
