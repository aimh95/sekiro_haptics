// Unit tests for ResolveAddressesForIdentity(): the orchestrator that
// selects a profile by exact build identity, then resolves each
// AddressSpec independently against a fake process. No real process, no
// real Sekiro data -- every value here is a synthetic "fixture." See
// docs/05-process-access.md for the full contract this tests.

#include "sekiro_haptics/process/AddressResolver.hpp"
#include "sekiro_haptics/process/SignatureProfileRepository.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessReader.hpp"
#include "SignatureProfileFixtureBuilders.hpp"

#include <cstring>
#include <string>

using namespace sekiro_haptics::process;

namespace {

// Module geometry used throughout these tests.
constexpr std::uintptr_t kModuleBase = 0x10000;
constexpr std::size_t kModuleSize = 0x20000;

// A 4-byte pattern that can be placed at known offsets.
const std::string kSimplePatternText = "AA BB CC DD";
const std::uint8_t kSimplePatternBytes[] = {0xAA, 0xBB, 0xCC, 0xDD};

ExecutableIdentity MakeFixtureIdentity() {
    ExecutableIdentity id;
    id.fileSizeBytes = 0x1000;
    // Build digest matching kFixtureSha256
    std::string lower = kFixtureSha256;
    for (std::size_t i = 0; i < 32; ++i) {
        auto hv = [](char c) -> int { return (c <= '9') ? c - '0' : c - 'a' + 10; };
        id.sha256.bytes[i] = static_cast<std::uint8_t>((hv(lower[i * 2]) << 4) | hv(lower[i * 2 + 1]));
    }
    id.mainModuleBaseAddress = kModuleBase;
    id.mainModuleImageSize = kModuleSize;
    return id;
}

std::string MakeResolverProfileJson(const std::string& patternText = kSimplePatternText,
                                     const std::string& resolutionKind = "match_address",
                                     const std::string& matchOffset = "0x0",
                                     const std::string& scanOffset = "0x100",
                                     const std::string& scanSize = "0x100",
                                     const std::string& targetRangeOffset = "0x0",
                                     const std::string& targetRangeSize = "0x20000") {
    AddressFields addr;
    addr.addressId = "test-addr-1";
    addr.moduleName = "fixture_helper.exe";
    addr.scanOffset = scanOffset;
    addr.scanSize = scanSize;
    addr.pattern = patternText;
    addr.resolutionKind = resolutionKind;
    addr.matchOffset = matchOffset;
    addr.targetRangeOffset = targetRangeOffset;
    addr.targetRangeSize = targetRangeSize;
    // RIP-relative defaults (used when resolutionKind == "rip_relative_32")
    addr.instructionOffset = "0x0";
    addr.displacementOffset = "0x4";
    addr.instructionLength = "0x8";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};

    return WrapSignatureProfiles({profile.Render()});
}

void SetupModuleAndPattern(FakeProcessInspector& inspector, FakeProcessReader& reader,
                            std::uintptr_t patternAddress) {
    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    reader.PokeBytes(patternAddress, kSimplePatternBytes, sizeof(kSimplePatternBytes));
}

} // namespace

// ===========================================================================
// Profile selection failures (no scan attempted)
// ===========================================================================

SH_TEST(AddressResolver_UnsupportedBuild_ReturnsNotSupported_NoScanAttempted) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_unsupported.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    // Identity that doesn't match any loaded profile
    ExecutableIdentity unknownId;
    unknownId.fileSizeBytes = 0x9999;
    unknownId.mainModuleBaseAddress = kModuleBase;
    unknownId.mainModuleImageSize = kModuleSize;

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, unknownId, inspector, reader);

    SH_CHECK(outcome.buildSupported == false);
    SH_CHECK(outcome.buildFailureReason == AddressResolutionReason::UnsupportedBuild);
    SH_CHECK(outcome.addresses.empty());
    // No module lookup or read should have been attempted
    SH_CHECK(inspector.FindModuleCalls() == 0);
    SH_CHECK(reader.ReadCalls() == 0);
}


// ===========================================================================
// Successful match_address resolution
// ===========================================================================

SH_TEST(AddressResolver_MatchAddress_DirectMatch_ResolvesCorrectly) {
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x0",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_direct.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    // Pattern placed at module_base + 0x150 (within scan range [0x100, 0x200))
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].address == patternAddr); // matchOffset = 0
    SH_CHECK(outcome.addresses[0].addressId == "test-addr-1");
}

SH_TEST(AddressResolver_MatchAddress_WithPositiveOffset_ResolvesCorrectly) {
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x10",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_pos_offset.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].address == patternAddr + 0x10);
}

SH_TEST(AddressResolver_MatchAddress_WithNegativeOffset_ResolvesCorrectly) {
    // matchOffset = -0x10, pattern at base + 0x150, result should be 0x150 - 0x10 + base = base + 0x140
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "-0x10",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_neg_offset.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].address == patternAddr - 0x10);
}

// ===========================================================================
// Successful rip_relative_32 resolution
// ===========================================================================

SH_TEST(AddressResolver_RipRelative_ResolvesCorrectly) {
    // Build a profile with rip_relative_32:
    // instructionOffset=0x0, displacementOffset=0x4, instructionLength=0x8
    // Pattern: "AA BB CC DD" (4 bytes) -- displacement is AFTER pattern bytes
    // Pattern at kModuleBase + 0x150
    // Displacement bytes at matchAddress + 4 (i.e. kModuleBase + 0x154)
    // Target = (matchAddress + 0) + 8 + displacement
    AddressFields addr;
    addr.addressId = "test-rip-1";
    addr.moduleName = "fixture_helper.exe";
    addr.scanOffset = "0x100";
    addr.scanSize = "0x100";
    addr.pattern = kSimplePatternText;
    addr.resolutionKind = "rip_relative_32";
    addr.instructionOffset = "0x0";
    addr.displacementOffset = "0x4";
    addr.instructionLength = "0x8";
    addr.targetRangeOffset = "0x0";
    addr.targetRangeSize = "0x20000";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_rip.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    std::uintptr_t patternAddr = kModuleBase + 0x150;
    reader.PokeBytes(patternAddr, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    // Write displacement at matchAddress + 4 = kModuleBase + 0x154
    // (after the 4-byte pattern, so pattern bytes are not overwritten)
    // Target = instructionAddress(patternAddr + 0) + instructionLength(8) + disp32
    // displacement = 0x100 -> Target = kModuleBase + 0x150 + 8 + 0x100 = kModuleBase + 0x258
    std::int32_t displacement = 0x100;
    std::uint8_t dispBytes[4];
    std::memcpy(dispBytes, &displacement, 4);
    reader.PokeBytes(patternAddr + 4, dispBytes, 4);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    std::uintptr_t expectedTarget = patternAddr + 8 + 0x100;
    SH_CHECK(outcome.addresses[0].address == expectedTarget);
}

// ===========================================================================
// Module not found -> individual address Disabled
// ===========================================================================

SH_TEST(AddressResolver_ModuleNotFound_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_no_module.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector; // no module registered
    FakeProcessReader reader;

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ModuleNotFound);
}

// ===========================================================================
// Pattern not found -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_PatternNotFound_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_no_pattern.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    // Module exists but pattern bytes are not present in scan range
    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);
    // Don't poke any pattern bytes -> scan finds nothing

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::PatternNotFound);
}

// ===========================================================================
// Pattern ambiguous (multiple matches) -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_PatternAmbiguous_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_ambiguous.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    // Place pattern at two different locations within scan range
    reader.PokeBytes(kModuleBase + 0x110, kSimplePatternBytes, sizeof(kSimplePatternBytes));
    reader.PokeBytes(kModuleBase + 0x180, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::PatternAmbiguous);
}

// ===========================================================================
// Target out of range -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_TargetOutOfRange_AddressDisabled) {
    // Target range is [0x0, 0x100) relative to module, but pattern is at 0x150
    // so matchAddress (0x10150) + offset(0) = 0x10150 which is outside target range
    // [0x10000, 0x10100)
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x0",
                                         "0x100", "0x100", "0x0", "0x100");
    auto path = WriteSignatureProfileTestJson("resolver_target_oor.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::TargetOutOfRange);
}


// ===========================================================================
// Process read failure -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_ProcessReadFailed_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_read_fail.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    // Force a read failure on the first ReadBytes call
    reader.FailReadAtCall(0);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ProcessReadFailed);
}

// ===========================================================================
// Partial address failure: one resolves, one fails independently
// ===========================================================================

SH_TEST(AddressResolver_MultipleAddresses_IndependentResolution) {
    // Two addresses in one profile: first one will succeed (pattern present),
    // second one will fail (pattern not in its scan range)
    AddressFields addr1;
    addr1.addressId = "addr-succeeds";
    addr1.moduleName = "fixture_helper.exe";
    addr1.scanOffset = "0x100";
    addr1.scanSize = "0x100";
    addr1.pattern = kSimplePatternText;
    addr1.resolutionKind = "match_address";
    addr1.matchOffset = "0x0";
    addr1.targetRangeOffset = "0x0";
    addr1.targetRangeSize = "0x20000";

    AddressFields addr2;
    addr2.addressId = "addr-fails";
    addr2.moduleName = "fixture_helper.exe";
    addr2.scanOffset = "0x500";  // different scan range
    addr2.scanSize = "0x100";
    addr2.pattern = "EE FF 11 22"; // pattern not present anywhere
    addr2.resolutionKind = "match_address";
    addr2.matchOffset = "0x0";
    addr2.targetRangeOffset = "0x0";
    addr2.targetRangeSize = "0x20000";

    ProfileFields profile;
    profile.addressesJson = {addr1.Render(), addr2.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_partial.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    // Place first pattern in scan range
    reader.PokeBytes(kModuleBase + 0x150, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 2);
    // First address resolved
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].addressId == "addr-succeeds");
    SH_CHECK(outcome.addresses[0].address == kModuleBase + 0x150);
    // Second address disabled (pattern not found)
    SH_CHECK(outcome.addresses[1].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[1].addressId == "addr-fails");
    SH_CHECK(outcome.addresses[1].reason == AddressResolutionReason::PatternNotFound);
}

// ===========================================================================
// Profile-level failure: scan range invalid (overflow at module boundary)
// ===========================================================================

SH_TEST(AddressResolver_ScanRangeOverflowsModule_AddressDisabled) {
    // scanOffset + scanSize > moduleSize
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x0",
                                         "0x1F000", "0x2000", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_scan_overflow.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize; // 0x20000
    inspector.SetModule("fixture_helper.exe", module);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::InvalidScanRange);
}

// ===========================================================================
// Fallback-free: size-only match never selects
// ===========================================================================

SH_TEST(AddressResolver_SizeOnlyMatch_NeverSelectsProfile) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_size_only.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    SetupModuleAndPattern(inspector, reader, kModuleBase + 0x150);

    // Same file size (0x1000) but different SHA
    ExecutableIdentity wrongSha = MakeFixtureIdentity();
    wrongSha.sha256.bytes[0] = 0xFF; // corrupt one byte

    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, wrongSha, inspector, reader);
    SH_CHECK(outcome.buildSupported == false);
    SH_CHECK(outcome.buildFailureReason == AddressResolutionReason::UnsupportedBuild);
    SH_CHECK(outcome.addresses.empty());
}

// ===========================================================================
// RIP-relative with negative displacement
// ===========================================================================

SH_TEST(AddressResolver_RipRelative_NegativeDisplacement_ResolvesCorrectly) {
    AddressFields addr;
    addr.addressId = "test-rip-neg";
    addr.moduleName = "fixture_helper.exe";
    addr.scanOffset = "0x1000";
    addr.scanSize = "0x1000";
    addr.pattern = kSimplePatternText;
    addr.resolutionKind = "rip_relative_32";
    addr.instructionOffset = "0x0";
    addr.displacementOffset = "0x4";
    addr.instructionLength = "0x8";
    addr.targetRangeOffset = "0x0";
    addr.targetRangeSize = "0x20000";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_rip_neg.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    // Pattern at base + 0x1500
    std::uintptr_t patternAddr = kModuleBase + 0x1500;
    reader.PokeBytes(patternAddr, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    // displacement at matchAddress + 4 (after 4-byte pattern)
    // displacement = -0x200 -> target = patternAddr + 8 + (-0x200) = patternAddr - 0x1F8
    std::int32_t displacement = -0x200;
    std::uint8_t dispBytes[4];
    std::memcpy(dispBytes, &displacement, 4);
    reader.PokeBytes(patternAddr + 4, dispBytes, 4);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses.size() == 1);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    std::uintptr_t expectedTarget = static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(patternAddr) + 8 + displacement);
    SH_CHECK(outcome.addresses[0].address == expectedTarget);
}

// ===========================================================================
// Disabled address never carries stale values
// ===========================================================================

SH_TEST(AddressResolver_DisabledAddress_HasZeroAddress) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_disabled_zero.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    // Module exists but no pattern -> PatternNotFound
    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].address == 0); // no stale value
}

// ===========================================================================
// Process not attached -> Disabled with ProcessReadFailed
// ===========================================================================

SH_TEST(AddressResolver_ProcessNotAttached_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_not_attached.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    reader.SetAttached(false);

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ProcessReadFailed);
}


// ===========================================================================
// Case-insensitive module match
// ===========================================================================

SH_TEST(AddressResolver_CaseInsensitiveModuleMatch_ResolvesCorrectly) {
    // Profile specifies "fixture_helper.exe" but inspector has "FIXTURE_HELPER.EXE"
    AddressFields addr;
    addr.addressId = "case-test";
    addr.moduleName = "fixture_helper.exe"; // lowercase in profile
    addr.scanOffset = "0x100";
    addr.scanSize = "0x100";
    addr.pattern = kSimplePatternText;
    addr.resolutionKind = "match_address";
    addr.matchOffset = "0x0";
    addr.targetRangeOffset = "0x0";
    addr.targetRangeSize = "0x20000";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_case.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    // Register module with UPPERCASE name
    ModuleInfo module;
    module.name = "FIXTURE_HELPER.EXE";
    module.path = "C:\\test\\FIXTURE_HELPER.EXE";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("FIXTURE_HELPER.EXE", module);

    reader.PokeBytes(kModuleBase + 0x150, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome.addresses[0].address == kModuleBase + 0x150);
}

// ===========================================================================
// Substring module name must NOT match
// ===========================================================================

SH_TEST(AddressResolver_SubstringModuleName_DoesNotMatch) {
    AddressFields addr;
    addr.addressId = "substr-test";
    addr.moduleName = "helper.exe"; // only a substring of "fixture_helper.exe"
    addr.scanOffset = "0x100";
    addr.scanSize = "0x100";
    addr.pattern = kSimplePatternText;
    addr.resolutionKind = "match_address";
    addr.matchOffset = "0x0";
    addr.targetRangeOffset = "0x0";
    addr.targetRangeSize = "0x20000";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_substr.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    // Register a module with full name "fixture_helper.exe" - won't match "helper.exe"
    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    reader.PokeBytes(kModuleBase + 0x150, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ModuleNotFound);
}

// ===========================================================================
// Partial read -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_PartialRead_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_partial_read.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    // Force partial read on first call
    reader.ForcePartialReadAtCall(0, 10);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ProcessReadFailed);
}

// ===========================================================================
// Process exited -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_ProcessExited_AddressDisabled) {
    auto json = MakeResolverProfileJson();
    auto path = WriteSignatureProfileTestJson("resolver_exited.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    reader.SetAlive(false); // process has exited

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::ProcessReadFailed);
}

// ===========================================================================
// MatchAddress offset overflow -> Disabled (AddressCalculationFailed)
// ===========================================================================

SH_TEST(AddressResolver_MatchOffsetOverflow_AddressDisabled) {
    // Use a very large positive offset that would overflow when added to match address
    // matchOffset = 0x7FFFFFFFFFFFFFFF (max int64), pattern at base+0x150
    // matchAddress(>0) + 0x7FFFFFFFFFFFFFFF will overflow int64 addition
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x7FFFFFFFFFFFFFFF",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_offset_overflow.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    // Either AddressCalculationFailed or TargetOutOfRange -- both valid for overflow
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::AddressCalculationFailed ||
             outcome.addresses[0].reason == AddressResolutionReason::TargetOutOfRange);
}

// ===========================================================================
// MatchAddress negative offset underflow -> Disabled
// ===========================================================================

SH_TEST(AddressResolver_MatchOffsetUnderflow_AddressDisabled) {
    // matchOffset = -0x7FFFFFFFFFFFFFFF (very large negative)
    // pattern at base+0x150, matchAddress + huge negative = underflow
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "-0x7FFFFFFFFFFFFFFF",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_offset_underflow.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    std::uintptr_t patternAddr = kModuleBase + 0x150;
    SetupModuleAndPattern(inspector, reader, patternAddr);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::AddressCalculationFailed);
}

// ===========================================================================
// RIP calculation failure (e.g. target out of range)
// ===========================================================================

SH_TEST(AddressResolver_RipCalculationFailed_TargetOutOfRange_AddressDisabled) {
    // displacement that pushes target outside allowed target range
    AddressFields addr;
    addr.addressId = "rip-oor";
    addr.moduleName = "fixture_helper.exe";
    addr.scanOffset = "0x100";
    addr.scanSize = "0x100";
    addr.pattern = kSimplePatternText;
    addr.resolutionKind = "rip_relative_32";
    addr.instructionOffset = "0x0";
    addr.displacementOffset = "0x4";
    addr.instructionLength = "0x8";
    // target range is tiny: [0x0, 0x100) relative to module
    addr.targetRangeOffset = "0x0";
    addr.targetRangeSize = "0x100";

    ProfileFields profile;
    profile.addressesJson = {addr.Render()};
    auto json = WrapSignatureProfiles({profile.Render()});
    auto path = WriteSignatureProfileTestJson("resolver_rip_oor.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    std::uintptr_t patternAddr = kModuleBase + 0x150;
    reader.PokeBytes(patternAddr, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    // displacement = 0x5000 -> target = patternAddr + 8 + 0x5000 = way outside [base, base+0x100)
    std::int32_t displacement = 0x5000;
    std::uint8_t dispBytes[4];
    std::memcpy(dispBytes, &displacement, 4);
    reader.PokeBytes(patternAddr + 4, dispBytes, 4);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::TargetOutOfRange);
}

// ===========================================================================
// Module-relative scan range overflow (scanOffset itself overflows with base)
// ===========================================================================

SH_TEST(AddressResolver_ScanOffsetOverflowsAddressSpace_AddressDisabled) {
    // scanOffset is very large so that moduleBase + scanOffset overflows
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x0",
                                         "0xFFFFFFFFFFFFF000", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_scan_addr_overflow.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;

    ModuleInfo module;
    module.name = "fixture_helper.exe";
    module.path = "C:\\test\\fixture_helper.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture_helper.exe", module);

    ExecutableIdentity identity = MakeFixtureIdentity();
    ProfileResolutionOutcome outcome = ResolveAddressesForIdentity(repo, identity, inspector, reader);

    SH_CHECK(outcome.buildSupported == true);
    SH_CHECK(outcome.addresses[0].state == AddressResolutionState::Disabled);
    SH_CHECK(outcome.addresses[0].reason == AddressResolutionReason::InvalidScanRange);
}

// ===========================================================================
// Runtime address is not reusable across resolves (no caching)
// ===========================================================================

SH_TEST(AddressResolver_NoAddressCaching_FreshResolveRequired) {
    auto json = MakeResolverProfileJson(kSimplePatternText, "match_address", "0x0",
                                         "0x100", "0x100", "0x0", "0x20000");
    auto path = WriteSignatureProfileTestJson("resolver_no_cache.json", json);
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    FakeProcessInspector inspector;
    FakeProcessReader reader;
    SetupModuleAndPattern(inspector, reader, kModuleBase + 0x150);

    ExecutableIdentity identity = MakeFixtureIdentity();

    // First resolve succeeds
    ProfileResolutionOutcome outcome1 = ResolveAddressesForIdentity(repo, identity, inspector, reader);
    SH_CHECK(outcome1.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome1.addresses[0].address == kModuleBase + 0x150);

    // Simulate detach/reattach: module at different base (ASLR changed)
    FakeProcessInspector inspector2;
    FakeProcessReader reader2;
    constexpr std::uintptr_t kNewBase = 0x70000;
    ModuleInfo module2;
    module2.name = "fixture_helper.exe";
    module2.path = "C:\\test\\fixture_helper.exe";
    module2.baseAddress = kNewBase;
    module2.imageSize = kModuleSize;
    inspector2.SetModule("fixture_helper.exe", module2);
    reader2.PokeBytes(kNewBase + 0x150, kSimplePatternBytes, sizeof(kSimplePatternBytes));

    // Second resolve with new inspector/reader gives new address
    ProfileResolutionOutcome outcome2 = ResolveAddressesForIdentity(repo, identity, inspector2, reader2);
    SH_CHECK(outcome2.addresses[0].state == AddressResolutionState::Resolved);
    SH_CHECK(outcome2.addresses[0].address == kNewBase + 0x150);

    // Addresses differ (ASLR)
    SH_CHECK(outcome1.addresses[0].address != outcome2.addresses[0].address);
}

