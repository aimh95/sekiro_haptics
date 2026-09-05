// Unit tests for SEK-PROBE-001D Stage A's SekiroKnownRootResolver /
// SekiroChildPointerResolver. Entirely Fake-based -- no real process, no
// real Sekiro data, no real AOB pattern; every byte here is synthetic
// fixture data exercising the resolver's own logic (identity gating, unique
// AOB matching via the unchanged AobScanner, RIP-relative via the unchanged
// RipRelative, one pointer dereference, generation tracking, and fail-closed
// behavior on every error path). See docs/07-combat-signal-reader.md.

#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessReader.hpp"

#include <cstring>
#include <string>

using namespace sekiro_haptics::process;

namespace {

constexpr std::uintptr_t kModuleBase = 0x140000000;
// Deliberately smaller than AobScanner's own chunk size (kAobScanChunkBytes,
// 64 KiB) so a full scan always takes exactly one ReadBytes() call --
// keeps the exact-call-index tests below independent of chunking details.
constexpr std::size_t kModuleSize = 0x1000;

// mov rax, [rip+disp32]; <arbitrary suffix byte> -- 8 bytes total, matching
// the real GameDataMan-candidate AOB's own leading instruction shape
// (instructionOffset=0, displacementOffset=3, instructionLength=7). The
// trailing 0x99 keeps the pattern from being "wildcards only" and gives it a
// second anchor byte, same spirit as the real ticket's longer pattern.
const std::string kPatternText = "48 8B 05 ?? ?? ?? ?? 99";
constexpr std::size_t kInstructionOffset = 0;
constexpr std::size_t kDisplacementOffset = 3;
constexpr std::size_t kInstructionLength = 7;

ExecutableIdentity MakeIdentity(std::uint64_t fileSizeBytes, std::uint8_t sha256Fill) {
    ExecutableIdentity id;
    id.fileSizeBytes = fileSizeBytes;
    id.sha256.bytes.fill(sha256Fill);
    return id;
}

KnownRootSpec MakeSpec() {
    KnownRootSpec spec;
    spec.rootId = "TestRoot";
    spec.moduleName = "fixture.exe";
    AobPattern pattern;
    ParseAobPattern(kPatternText, pattern); // well-formed by construction
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

/// Places the pattern's fixed bytes at `patternAddr`, computes the disp32
/// so the RIP-relative target lands exactly at `pointerSlotAddr`, and pokes
/// an 8-byte pointer value at that slot equal to `objectAddr` (0 leaves it
/// a live null pointer).
void SetupValidChain(FakeProcessReader& reader, std::uintptr_t patternAddr, std::uintptr_t pointerSlotAddr,
                      std::uintptr_t objectAddr) {
    const std::uint8_t patternBytes[] = {0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x99};
    reader.PokeBytes(patternAddr, patternBytes, sizeof(patternBytes));

    auto nextInstructionAddress = static_cast<std::int64_t>(patternAddr + kInstructionLength);
    auto disp64 = static_cast<std::int64_t>(pointerSlotAddr) - nextInstructionAddress;
    auto disp32 = static_cast<std::int32_t>(disp64);
    std::uint8_t dispBytes[4];
    std::memcpy(dispBytes, &disp32, 4);
    reader.PokeBytes(patternAddr + kDisplacementOffset, dispBytes, 4);

    std::uint64_t objVal = objectAddr;
    reader.PokeBytes(pointerSlotAddr, &objVal, sizeof(objVal));
}

constexpr std::uintptr_t kPatternAddr = kModuleBase + 0x10;
constexpr std::uintptr_t kPointerSlotAddr = kModuleBase + 0x800;
constexpr std::uintptr_t kObjectAddr = kModuleBase + 0x9000;

} // namespace

// ===========================================================================
// SekiroKnownRootResolver -- success path
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_UniqueMatch_ResolvesSuccessfully) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::Resolved);
    SH_CHECK(result.objectAddress == kObjectAddr);
    SH_CHECK(result.generation == 1);
    SH_CHECK(resolver.Current().objectAddress == kObjectAddr);
}

// ===========================================================================
// AOB match count
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_NoMatch_ReturnsSignatureNotFound) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader; // pattern never poked -- module reads as all zero

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::SignatureNotFound);
    SH_CHECK(result.objectAddress == 0);
    SH_CHECK(result.generation == 0);
}

SH_TEST(SekiroKnownRootResolver_Resolve_MultipleMatches_ReturnsAmbiguousSignature) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);
    // A second, independent match elsewhere in the module.
    const std::uint8_t patternBytes[] = {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0x99};
    reader.PokeBytes(kModuleBase + 0x400, patternBytes, sizeof(patternBytes));

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::AmbiguousSignature);
    SH_CHECK(result.objectAddress == 0);
}

// ===========================================================================
// RIP-relative failure propagation
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_RipRelativeTargetOutsideModule_ReturnsAddressCalculationFailed) {
    // The full displacement/overflow arithmetic itself is already
    // exhaustively unit-tested at the RipRelative.hpp layer
    // (test_rip_relative.cpp) -- this only verifies the resolver correctly
    // propagates *a* RIP-relative failure (target-range violation) as
    // AddressCalculationFailed and never proceeds to a pointer dereference.
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    // Target computed to land far outside [kModuleBase, kModuleBase+kModuleSize).
    SetupValidChain(reader, kPatternAddr, kModuleBase + 0x10000000, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::AddressCalculationFailed);
    SH_CHECK(result.objectAddress == 0);
}

// ===========================================================================
// Identity gating
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_IdentityMismatch_ReturnsUnsupportedBuild_NoScanAttempted) {
    ExecutableIdentity expected = MakeIdentity(0x1000, 0xAB);
    ExecutableIdentity current = MakeIdentity(0x9999, 0xCD); // different build entirely
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, expected, current);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::UnsupportedBuild);
    SH_CHECK(result.objectAddress == 0);
    SH_CHECK(inspector.FindModuleCalls() == 0);
    SH_CHECK(reader.ReadCalls() == 0);
}

// ===========================================================================
// Null / partial-read pointer dereference
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_PointerSlotIsNull_ReturnsNullPointer) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, 0); // live pointer slot holds 0

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::NullPointer);
    SH_CHECK(result.objectAddress == 0);
}

SH_TEST(SekiroKnownRootResolver_Resolve_PointerSlotPartialRead_ReturnsReadFailed) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);

    // Discover exactly how many ReadBytes() calls a full successful resolve
    // takes, so the partial-read injection targets the *last* call (the
    // pointer-slot dereference) without hardcoding AobScanner's own
    // internal chunking details.
    int totalCalls = 0;
    {
        FakeProcessReader dryRunReader;
        SetupValidChain(dryRunReader, kPatternAddr, kPointerSlotAddr, kObjectAddr);
        SekiroKnownRootResolver dryRun(dryRunReader, inspector, spec, identity, identity);
        ResolvedRoot dryResult = dryRun.Resolve();
        SH_CHECK(dryResult.result == RootResolveResult::Resolved);
        totalCalls = dryRunReader.ReadCalls();
        SH_CHECK(totalCalls >= 1);
    }

    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);
    reader.ForcePartialReadAtCall(totalCalls - 1, 4); // pointer slot expects 8 bytes

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::ReadFailed);
    SH_CHECK(result.objectAddress == 0);
}

// ===========================================================================
// Process exit / not attached
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_NotAttached_ReturnsNotAttached) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);
    reader.SetAttached(false);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::NotAttached);
    SH_CHECK(result.objectAddress == 0);
}

SH_TEST(SekiroKnownRootResolver_Resolve_ProcessExited_ReturnsProcessExited) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);
    reader.SetAlive(false);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot result = resolver.Resolve();

    SH_CHECK(result.result == RootResolveResult::ProcessExited);
    SH_CHECK(result.objectAddress == 0);
}

// ===========================================================================
// Stale root never reused after failure
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_AfterSuccessThenModuleGone_NeverReusesStaleAddress) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot first = resolver.Resolve();
    SH_CHECK(first.result == RootResolveResult::Resolved);
    SH_CHECK(first.objectAddress == kObjectAddr);

    // Simulate the module disappearing (e.g. a detach/reattach to a
    // different process) between resolves.
    inspector.ForceFindModuleResult(ProcessInspectionResult::ModuleNotFound);
    ResolvedRoot second = resolver.Resolve();

    SH_CHECK(second.result == RootResolveResult::ModuleNotFound);
    SH_CHECK(second.objectAddress == 0); // never the stale kObjectAddr
    SH_CHECK(resolver.Current().objectAddress == 0);
}

// ===========================================================================
// Generation tracking
// ===========================================================================

SH_TEST(SekiroKnownRootResolver_Resolve_SameObjectAcrossResolves_GenerationUnchanged) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot first = resolver.Resolve();
    ResolvedRoot second = resolver.Resolve();

    SH_CHECK(first.generation == 1);
    SH_CHECK(second.generation == 1);
    SH_CHECK(second.objectAddress == kObjectAddr);
}

SH_TEST(SekiroKnownRootResolver_Resolve_ObjectAddressChanges_GenerationIncrements) {
    ExecutableIdentity identity = MakeIdentity(0x1000, 0xAB);
    KnownRootSpec spec = MakeSpec();
    constexpr std::uintptr_t kSecondObjectAddr = kModuleBase + 0xA000;

    FakeProcessInspector inspector;
    SetupModule(inspector);
    FakeProcessReader reader;
    SetupValidChain(reader, kPatternAddr, kPointerSlotAddr, kObjectAddr);

    SekiroKnownRootResolver resolver(reader, inspector, spec, identity, identity);
    ResolvedRoot first = resolver.Resolve();
    SH_CHECK(first.generation == 1);

    // The live pointer slot now holds a different object address (e.g. the
    // game reallocated it).
    std::uint64_t newVal = kSecondObjectAddr;
    reader.PokeBytes(kPointerSlotAddr, &newVal, sizeof(newVal));
    ResolvedRoot second = resolver.Resolve();

    SH_CHECK(second.result == RootResolveResult::Resolved);
    SH_CHECK(second.objectAddress == kSecondObjectAddr);
    SH_CHECK(second.generation == 2);

    // Resolving again with the same (new) value must not bump generation further.
    ResolvedRoot third = resolver.Resolve();
    SH_CHECK(third.generation == 2);
}

// ===========================================================================
// SekiroChildPointerResolver
// ===========================================================================

SH_TEST(SekiroChildPointerResolver_Resolve_ValidOffset_ReadsChildPointer) {
    constexpr std::uintptr_t kParentAddr = kModuleBase + 0x5000;
    constexpr std::int64_t kOffset = 0x8;
    constexpr std::uintptr_t kChildAddr = kModuleBase + 0x6000;

    FakeProcessReader reader;
    std::uint64_t childVal = kChildAddr;
    reader.PokeBytes(kParentAddr + kOffset, &childVal, sizeof(childVal));

    SekiroChildPointerResolver child(reader, kOffset);
    ResolvedRoot result = child.Resolve(kParentAddr);

    SH_CHECK(result.result == RootResolveResult::Resolved);
    SH_CHECK(result.objectAddress == kChildAddr);
    SH_CHECK(result.generation == 1);
}

SH_TEST(SekiroChildPointerResolver_Resolve_ZeroParentAddress_ReturnsNullPointer_NoRead) {
    FakeProcessReader reader;
    SekiroChildPointerResolver child(reader, 0x8);
    ResolvedRoot result = child.Resolve(0);

    SH_CHECK(result.result == RootResolveResult::NullPointer);
    SH_CHECK(result.objectAddress == 0);
    SH_CHECK(reader.ReadCalls() == 0);
}

SH_TEST(SekiroChildPointerResolver_Resolve_ChildPointerIsZero_ReturnsNullPointer) {
    constexpr std::uintptr_t kParentAddr = kModuleBase + 0x5000;
    FakeProcessReader reader; // offset bytes never poked -- reads as 0

    SekiroChildPointerResolver child(reader, 0x8);
    ResolvedRoot result = child.Resolve(kParentAddr);

    SH_CHECK(result.result == RootResolveResult::NullPointer);
    SH_CHECK(result.objectAddress == 0);
}

SH_TEST(SekiroChildPointerResolver_Resolve_ReadFails_ReturnsReadFailed) {
    constexpr std::uintptr_t kParentAddr = kModuleBase + 0x5000;
    FakeProcessReader reader;
    std::uint64_t childVal = kModuleBase + 0x6000;
    reader.PokeBytes(kParentAddr + 0x8, &childVal, sizeof(childVal));
    reader.FailReadAtCall(0);

    SekiroChildPointerResolver child(reader, 0x8);
    ResolvedRoot result = child.Resolve(kParentAddr);

    SH_CHECK(result.result == RootResolveResult::ReadFailed);
    SH_CHECK(result.objectAddress == 0);
}

SH_TEST(SekiroChildPointerResolver_Resolve_OffsetOverflowsAddressSpace_ReturnsAddressCalculationFailed) {
    // parentAddress is within 0x10 of the top of the address space; a +0x100
    // offset overflows.
    std::uintptr_t nearTop = static_cast<std::uintptr_t>(-1) - 0x10;
    FakeProcessReader reader;

    SekiroChildPointerResolver child(reader, 0x100);
    ResolvedRoot result = child.Resolve(nearTop);

    SH_CHECK(result.result == RootResolveResult::AddressCalculationFailed);
    SH_CHECK(result.objectAddress == 0);
    SH_CHECK(reader.ReadCalls() == 0);
}

SH_TEST(SekiroChildPointerResolver_Resolve_NegativeOffsetUnderflows_ReturnsAddressCalculationFailed) {
    // parentAddress is small enough that subtracting 0x100 would go below 0.
    constexpr std::uintptr_t kTinyParentAddr = 0x10;
    FakeProcessReader reader;

    SekiroChildPointerResolver child(reader, -0x100);
    ResolvedRoot result = child.Resolve(kTinyParentAddr);

    SH_CHECK(result.result == RootResolveResult::AddressCalculationFailed);
    SH_CHECK(result.objectAddress == 0);
    SH_CHECK(reader.ReadCalls() == 0);
}

SH_TEST(SekiroChildPointerResolver_Resolve_AddressChanges_GenerationIncrements) {
    constexpr std::uintptr_t kParentAddr = kModuleBase + 0x5000;
    constexpr std::uintptr_t kChildAddrA = kModuleBase + 0x6000;
    constexpr std::uintptr_t kChildAddrB = kModuleBase + 0x7000;

    FakeProcessReader reader;
    std::uint64_t valA = kChildAddrA;
    reader.PokeBytes(kParentAddr + 0x8, &valA, sizeof(valA));

    SekiroChildPointerResolver child(reader, 0x8);
    ResolvedRoot first = child.Resolve(kParentAddr);
    SH_CHECK(first.generation == 1);

    ResolvedRoot second = child.Resolve(kParentAddr); // same value again
    SH_CHECK(second.generation == 1);

    std::uint64_t valB = kChildAddrB;
    reader.PokeBytes(kParentAddr + 0x8, &valB, sizeof(valB));
    ResolvedRoot third = child.Resolve(kParentAddr);
    SH_CHECK(third.result == RootResolveResult::Resolved);
    SH_CHECK(third.objectAddress == kChildAddrB);
    SH_CHECK(third.generation == 2);
}
