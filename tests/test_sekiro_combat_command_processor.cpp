// Unit tests for SekiroCombatCommandProcessor's combat-plan/combat-resolve/
// combat-status text parsing and output formatting. Fake-based, same fixture
// pattern as test_sekiro_raw_combat_reader.cpp.

#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"
#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessReader.hpp"

#include <algorithm>
#include <cstring>
#include <string>

using namespace sekiro_haptics::process;

namespace {

constexpr std::uintptr_t kModuleBase = 0x140000000;
constexpr std::size_t kModuleSize = 0x1000;
const std::string kPatternText = "48 8B 05 ?? ?? ?? ?? 99";
constexpr std::size_t kInstructionOffset = 0;
constexpr std::size_t kDisplacementOffset = 3;
constexpr std::size_t kInstructionLength = 7;
constexpr std::int64_t kPlayerGameDataOffset = 0x8;

constexpr std::uintptr_t kPatternAddr = kModuleBase + 0x10;
constexpr std::uintptr_t kGameDataManPtrSlotAddr = kModuleBase + 0x800;
constexpr std::uintptr_t kGameDataManAddr = 0x200000000ULL;
constexpr std::uintptr_t kPlayerGameDataAddr = 0x300000000ULL;

ExecutableIdentity MakeIdentity(std::uint64_t fileSizeBytes, std::uint8_t sha256Fill) {
    ExecutableIdentity id;
    id.fileSizeBytes = fileSizeBytes;
    id.sha256.bytes.fill(sha256Fill);
    return id;
}

KnownRootSpec MakeGameDataManSpec() {
    KnownRootSpec spec;
    spec.rootId = "GameDataMan";
    spec.moduleName = "fixture.exe";
    AobPattern pattern;
    ParseAobPattern(kPatternText, pattern);
    spec.pattern = pattern;
    spec.instructionOffset = kInstructionOffset;
    spec.displacementOffset = kDisplacementOffset;
    spec.instructionLength = kInstructionLength;
    return spec;
}

void SetupModule(FakeProcessInspector& inspector) {
    ModuleInfo module;
    module.name = "fixture.exe";
    module.path = "C:\\test\\fixture.exe";
    module.baseAddress = kModuleBase;
    module.imageSize = kModuleSize;
    inspector.SetModule("fixture.exe", module); // used by FindModuleExact() (the resolver)
    inspector.SetMainModule(module);            // used by GetMainModule() (combat-plan)
}

void SetupValidChain(FakeProcessReader& reader) {
    const std::uint8_t patternBytes[] = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x99};
    reader.PokeBytes(kPatternAddr, patternBytes, sizeof(patternBytes));

    auto nextInstructionAddress = static_cast<std::int64_t>(kPatternAddr + kInstructionLength);
    auto disp64 = static_cast<std::int64_t>(kGameDataManPtrSlotAddr) - nextInstructionAddress;
    auto disp32 = static_cast<std::int32_t>(disp64);
    std::uint8_t dispBytes[4];
    std::memcpy(dispBytes, &disp32, 4);
    reader.PokeBytes(kPatternAddr + kDisplacementOffset, dispBytes, 4);

    std::uint64_t gameDataManVal = kGameDataManAddr;
    reader.PokeBytes(kGameDataManPtrSlotAddr, &gameDataManVal, sizeof(gameDataManVal));

    std::uint64_t playerGameDataVal = kPlayerGameDataAddr;
    reader.PokeBytes(kGameDataManAddr + kPlayerGameDataOffset, &playerGameDataVal, sizeof(playerGameDataVal));
}

void PokeCombatFields(FakeProcessReader& reader, std::int32_t hp, std::int32_t maxHp, std::int32_t posture,
                       std::int32_t maxPosture) {
    reader.PokeBytes(kPlayerGameDataAddr + 0x18, &hp, sizeof(hp));
    reader.PokeBytes(kPlayerGameDataAddr + 0x1C, &maxHp, sizeof(maxHp));
    reader.PokeBytes(kPlayerGameDataAddr + 0x34, &posture, sizeof(posture));
    reader.PokeBytes(kPlayerGameDataAddr + 0x38, &maxPosture, sizeof(maxPosture));
}

bool AnyLineContains(const std::vector<std::string>& lines, const std::string& needle) {
    return std::any_of(lines.begin(), lines.end(),
                        [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

} // namespace

SH_TEST(SekiroCombatCommandProcessor_UnknownVerb_ReturnsUnhandled) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    auto result = processor.Process("identity");
    SH_CHECK(!result.handled);
    SH_CHECK(result.outputLines.empty());
}

SH_TEST(SekiroCombatCommandProcessor_CombatPlan_ModuleFound_ReportsBytesAndFullScanFalse) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    auto result = processor.Process("combat-plan");
    SH_CHECK(result.handled);
    SH_CHECK(AnyLineContains(result.outputLines, "aobScanRangeBytes=" + std::to_string(kModuleSize)));
    SH_CHECK(AnyLineContains(result.outputLines, "fullScanUsed=false"));
}

SH_TEST(SekiroCombatCommandProcessor_CombatResolve_Success_ReportsAddressesAndGeneration) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    auto result = processor.Process("combat-resolve");
    SH_CHECK(result.handled);
    SH_CHECK(AnyLineContains(result.outputLines, "status=ResolvedUnvalidated"));
    SH_CHECK(AnyLineContains(result.outputLines, "generation=1"));
}

SH_TEST(SekiroCombatCommandProcessor_CombatResolve_SignatureNotFound_ReportsStatusOnly) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader; // no pattern poked

    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    auto result = processor.Process("combat-resolve");
    SH_CHECK(result.handled);
    SH_CHECK(AnyLineContains(result.outputLines, "status=SignatureNotFound"));
}

SH_TEST(SekiroCombatCommandProcessor_CombatStatus_BeforeResolve_ReportsTemporarilyUnavailable) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);
    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    auto result = processor.Process("combat-status");
    SH_CHECK(result.handled);
    SH_CHECK(AnyLineContains(result.outputLines, "status=TemporarilyUnavailable"));
}

SH_TEST(SekiroCombatCommandProcessor_CombatStatus_AfterResolve_ReportsHpAndPosture) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);
    SekiroRawCombatReader rawReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, identity, identity);
    SekiroCombatSessionController controller(rawReader, inspector);
    SekiroCombatCommandProcessor processor(controller);

    SH_CHECK(processor.Process("combat-resolve").handled);
    auto result = processor.Process("combat-status");
    SH_CHECK(result.handled);
    SH_CHECK(AnyLineContains(result.outputLines, "status=Valid"));
    SH_CHECK(AnyLineContains(result.outputLines, "hp=500/999"));
    SH_CHECK(AnyLineContains(result.outputLines, "posture=50/100"));
}
