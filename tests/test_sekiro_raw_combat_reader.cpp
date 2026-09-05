// Unit tests for SEK-PROBE-001D Stage B's SekiroRawCombatReader. Fake-based
// -- no real process, no real Sekiro data; every byte here is synthetic
// fixture data. See test_sekiro_known_root_resolver.cpp for the Stage A
// resolver this builds on (not re-tested exhaustively here -- only the
// composition + field decode/validation this layer adds).

#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessReader.hpp"

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
    inspector.SetModule("fixture.exe", module);
}

/// Wires up a fully valid GameDataMan -> PlayerGameData chain: AOB pattern
/// + RIP-relative pointer slot -> kGameDataManAddr, then
/// kGameDataManAddr+0x8 -> kPlayerGameDataAddr.
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

/// Pokes HP/MaxHP/Posture/MaxPosture at PlayerGameData's hypothesized
/// offsets (+0x18/+0x1C/+0x34/+0x38).
void PokeCombatFields(FakeProcessReader& reader, std::int32_t hp, std::int32_t maxHp, std::int32_t posture,
                       std::int32_t maxPosture) {
    reader.PokeBytes(kPlayerGameDataAddr + 0x18, &hp, sizeof(hp));
    reader.PokeBytes(kPlayerGameDataAddr + 0x1C, &maxHp, sizeof(maxHp));
    reader.PokeBytes(kPlayerGameDataAddr + 0x34, &posture, sizeof(posture));
    reader.PokeBytes(kPlayerGameDataAddr + 0x38, &maxPosture, sizeof(maxPosture));
}

SekiroRawCombatReader MakeReader(FakeProcessReader& reader, FakeProcessInspector& inspector,
                                  ExecutableIdentity expected, ExecutableIdentity current) {
    return SekiroRawCombatReader(reader, inspector, MakeGameDataManSpec(), kPlayerGameDataOffset, std::move(expected),
                                  std::move(current));
}

} // namespace

// ===========================================================================
// Resolve() propagation
// ===========================================================================

SH_TEST(SekiroRawCombatReader_Resolve_FullChainSucceeds_ReturnsResolvedUnvalidated) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    CombatResolveResult result = combat.Resolve();

    SH_CHECK(result.status == CombatSnapshotStatus::ResolvedUnvalidated);
    SH_CHECK(result.gameDataManAddress == kGameDataManAddr);
    SH_CHECK(result.playerGameDataAddress == kPlayerGameDataAddr);
    SH_CHECK(result.generation == 1);
}

SH_TEST(SekiroRawCombatReader_Resolve_GameDataManSignatureMissing_NeverAttemptsPlayerGameData) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader; // pattern never poked

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    CombatResolveResult result = combat.Resolve();

    SH_CHECK(result.status == CombatSnapshotStatus::SignatureNotFound);
    SH_CHECK(result.playerGameDataAddress == 0);
}

SH_TEST(SekiroRawCombatReader_Resolve_PlayerGameDataPointerIsNull_ReturnsReadFailed) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    std::uint64_t zero = 0;
    reader.PokeBytes(kGameDataManAddr + kPlayerGameDataOffset, &zero, sizeof(zero)); // overwrite with null

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    CombatResolveResult result = combat.Resolve();

    SH_CHECK(result.status == CombatSnapshotStatus::ReadFailed); // NullPointer collapses to ReadFailed here
    SH_CHECK(result.playerGameDataAddress == 0);
}

// ===========================================================================
// ReadSnapshot() -- decode/validate
// ===========================================================================

SH_TEST(SekiroRawCombatReader_ReadSnapshot_ValidFields_DecodesCorrectlyAndIsValid) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 723, 999, 12, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    SH_CHECK(combat.Resolve().status == CombatSnapshotStatus::ResolvedUnvalidated);

    CombatSnapshot snapshot = combat.ReadSnapshot();
    SH_CHECK(snapshot.status == CombatSnapshotStatus::Valid);
    SH_CHECK(snapshot.hp == 723);
    SH_CHECK(snapshot.maxHp == 999);
    SH_CHECK(snapshot.posture == 12);
    SH_CHECK(snapshot.maxPosture == 100);
    SH_CHECK(snapshot.generation == 1);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_HpExceedsMaxHp_ReturnsInvariantViolation) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, /*hp=*/1500, /*maxHp=*/999, 0, 100); // hp > maxHp -- impossible

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    combat.Resolve();
    CombatSnapshot snapshot = combat.ReadSnapshot();

    SH_CHECK(snapshot.status == CombatSnapshotStatus::InvariantViolation);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_MaxHpZero_ReturnsInvariantViolation) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 0, 0, 0, 0); // never poked at all would also be all-zero -- explicit here

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    combat.Resolve();
    CombatSnapshot snapshot = combat.ReadSnapshot();

    SH_CHECK(snapshot.status == CombatSnapshotStatus::InvariantViolation);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_NegativePosture_ReturnsInvariantViolation) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, -5, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    combat.Resolve();
    CombatSnapshot snapshot = combat.ReadSnapshot();

    SH_CHECK(snapshot.status == CombatSnapshotStatus::InvariantViolation);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_BeforeResolve_ReturnsTemporarilyUnavailable) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    CombatSnapshot snapshot = combat.ReadSnapshot(); // Resolve() never called

    SH_CHECK(snapshot.status == CombatSnapshotStatus::TemporarilyUnavailable);
}

SH_TEST(SekiroRawCombatReader_Resolve_FailsAfterPriorSuccess_ReadSnapshotBecomesTemporarilyUnavailable) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    SH_CHECK(combat.Resolve().status == CombatSnapshotStatus::ResolvedUnvalidated);
    SH_CHECK(combat.ReadSnapshot().status == CombatSnapshotStatus::Valid);

    // Module disappears (e.g. reattach to a different process) -- the next
    // Resolve() must clear the previously-resolved state, never leaving a
    // stale PlayerGameData address usable.
    inspector.ForceFindModuleResult(ProcessInspectionResult::ModuleNotFound);
    CombatResolveResult resolveResult = combat.Resolve();
    SH_CHECK(resolveResult.status != CombatSnapshotStatus::ResolvedUnvalidated);

    CombatSnapshot afterFailure = combat.ReadSnapshot();
    SH_CHECK(afterFailure.status == CombatSnapshotStatus::TemporarilyUnavailable);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_FieldReadFails_ReturnsReadFailed) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    combat.Resolve();

    reader.SetAlive(false); // the field read itself now fails
    CombatSnapshot snapshot = combat.ReadSnapshot();

    SH_CHECK(snapshot.status == CombatSnapshotStatus::ReadFailed);
}

SH_TEST(SekiroRawCombatReader_ReadSnapshot_UsesExactlyOneContiguousRead) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader);
    PokeCombatFields(reader, 500, 999, 50, 100);

    SekiroRawCombatReader combat = MakeReader(reader, inspector, identity, identity);
    combat.Resolve();

    int callsBefore = reader.ReadCalls();
    CombatSnapshot snapshot = combat.ReadSnapshot();
    int callsAfter = reader.ReadCalls();

    SH_CHECK(snapshot.status == CombatSnapshotStatus::Valid);
    SH_CHECK(callsAfter - callsBefore == 1); // one ReadBytes() call covering all four fields
}
