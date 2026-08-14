// End-to-end positive/negative fixture tests for the full
// "pattern parse -> unique scan -> match address -> rel32 target" pipeline,
// against fake in-memory module images (via FakeProcessReader) -- no real
// process, no real Sekiro data. See docs/05-process-access.md.

#include "sekiro_haptics/process/AobScanner.hpp"
#include "sekiro_haptics/process/RipRelative.hpp"
#include "testing.hpp"

#include "FakeProcessReader.hpp"

#include <cstdint>

using namespace sekiro_haptics::process;

namespace {
constexpr std::uintptr_t kModuleBase = 0x140000000ULL;
constexpr std::size_t kModuleSize = 0x2000;

// Places a synthetic "instruction" (2-byte opcode + 4-byte LE signed
// disp32 + 2 trailing sentinel bytes, matching the AOB text below) at
// `offset` within the fake module, with a displacement computed so the
// RIP-relative target lands exactly at `target`.
void PlaceInstruction(FakeProcessReader& reader, std::uintptr_t moduleAddress, std::size_t offset,
                       std::uintptr_t target) {
    std::uintptr_t matchAddress = moduleAddress + offset;
    std::uintptr_t instructionAddress = matchAddress + 2;
    std::uintptr_t nextInstructionAddress = instructionAddress + 6;
    std::int32_t disp32 = static_cast<std::int32_t>(static_cast<std::int64_t>(target) -
                                                      static_cast<std::int64_t>(nextInstructionAddress));

    std::uint8_t bytes[10];
    bytes[0] = 0xAB;
    bytes[1] = 0xCD;
    bytes[2] = 0x11;
    bytes[3] = 0x22;
    bytes[4] = static_cast<std::uint8_t>(disp32 & 0xFF);
    bytes[5] = static_cast<std::uint8_t>((disp32 >> 8) & 0xFF);
    bytes[6] = static_cast<std::uint8_t>((disp32 >> 16) & 0xFF);
    bytes[7] = static_cast<std::uint8_t>((disp32 >> 24) & 0xFF);
    bytes[8] = 0xEF;
    bytes[9] = 0x99;
    reader.PokeBytes(matchAddress, bytes, sizeof(bytes));
}

const char* kPatternText = "AB CD 11 22 ?? ?? ?? ?? EF 99";

/// Runs the full pipeline: parse -> unique scan -> RIP-relative resolve.
/// Returns Success with `outTarget` set only if every step succeeds.
AobScanResult ResolveViaFullPipeline(FakeProcessReader& reader, std::uintptr_t& outTarget) {
    AobPattern pattern;
    AobScanResult parseResult = ParseAobPattern(kPatternText, pattern);
    if (parseResult != AobScanResult::Success) {
        return parseResult;
    }

    std::uintptr_t matchAddress = 0;
    AobScanResult scanResult = ScanProcessRange(reader, kModuleBase, kModuleSize, pattern, matchAddress);
    if (scanResult != AobScanResult::Success) {
        return scanResult;
    }

    RipRelativeSpec spec;
    spec.matchAddress = matchAddress;
    spec.instructionOffset = 2;
    spec.displacementOffset = 4;
    spec.instructionLength = 6;
    spec.allowedSourceRangeBase = kModuleBase;
    spec.allowedSourceRangeSize = kModuleSize;
    spec.allowedTargetRangeBase = kModuleBase;
    spec.allowedTargetRangeSize = kModuleSize;

    return ResolveRipRelativeFromProcess(reader, spec, outTarget);
}

} // namespace

// ===========================================================================
// Positive fixture
// ===========================================================================

SH_TEST(AobFixture_Positive_UniquePatternWithWildcardAndValidRel32_ResolvesExactTarget) {
    FakeProcessReader reader;
    std::uintptr_t expectedTarget = kModuleBase + 0x500;
    PlaceInstruction(reader, kModuleBase, 0x100, expectedTarget);

    std::uintptr_t target = 0;
    AobScanResult result = ResolveViaFullPipeline(reader, target);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == expectedTarget);
}

// ===========================================================================
// Negative fixtures -- every one must fail to resolve an address.
// ===========================================================================

SH_TEST(AobFixture_Negative_NoPatternPresent_Fails) {
    FakeProcessReader reader; // nothing placed at all

    std::uintptr_t target = 0;
    AobScanResult result = ResolveViaFullPipeline(reader, target);

    SH_CHECK(result == AobScanResult::NoMatch);
}

SH_TEST(AobFixture_Negative_TwoOccurrences_Fails) {
    FakeProcessReader reader;
    PlaceInstruction(reader, kModuleBase, 0x100, kModuleBase + 0x500);
    PlaceInstruction(reader, kModuleBase, 0x300, kModuleBase + 0x600);

    std::uintptr_t target = 0;
    AobScanResult result = ResolveViaFullPipeline(reader, target);

    SH_CHECK(result == AobScanResult::MultipleMatches);
}

SH_TEST(AobFixture_Negative_SamePatternTwiceStraddlingChunkBoundary_Fails) {
    FakeProcessReader reader;
    // Two placements landing in visibly different chunks of a larger
    // module, one of them straddling the boundary -- still two distinct,
    // real matches, neither of which may be silently picked.
    PlaceInstruction(reader, kModuleBase, kAobScanChunkBytes - 3, kModuleBase + 0x500);
    PlaceInstruction(reader, kModuleBase, kAobScanChunkBytes + 0x100, kModuleBase + 0x600);

    AobPattern pattern;
    SH_CHECK(ParseAobPattern(kPatternText, pattern) == AobScanResult::Success);

    std::uintptr_t address = 0;
    AobScanResult result = ScanProcessRange(reader, kModuleBase, kAobScanChunkBytes * 2, pattern, address);

    SH_CHECK(result == AobScanResult::MultipleMatches);
}

SH_TEST(AobFixture_Negative_MalformedPattern_Fails) {
    FakeProcessReader reader;
    PlaceInstruction(reader, kModuleBase, 0x100, kModuleBase + 0x500);

    AobPattern pattern;
    AobScanResult parseResult = ParseAobPattern("AB CD 4? GG", pattern);

    SH_CHECK(parseResult == AobScanResult::InvalidPattern);
}

SH_TEST(AobFixture_Negative_TruncatedDisplacement_Fails) {
    FakeProcessReader reader;
    // The pattern's exact anchor bytes exist, but the module range is cut
    // short right after the match starts -- too short to contain the
    // declared instruction's displacement bytes at all once resolved.
    std::uint8_t bytes[10] = {0xAB, 0xCD, 0x11, 0x22, 0x00, 0x00, 0x00, 0x00, 0xEF, 0x99};
    std::uintptr_t matchAddress = kModuleBase + (kModuleSize - 4); // only 4 bytes remain in-range after this
    reader.PokeBytes(matchAddress, bytes, sizeof(bytes));

    AobPattern pattern;
    SH_CHECK(ParseAobPattern(kPatternText, pattern) == AobScanResult::Success);

    std::uintptr_t foundAddress = 0;
    AobScanResult scanResult = ScanProcessRange(reader, kModuleBase, kModuleSize, pattern, foundAddress);
    // The pattern itself doesn't even fit within the declared range near
    // the end -- caught as NoMatch (never a fabricated match).
    SH_CHECK(scanResult == AobScanResult::NoMatch);
}

SH_TEST(AobFixture_Negative_TargetOutOfDeclaredRange_Fails) {
    FakeProcessReader reader;
    std::uintptr_t outOfRangeTarget = kModuleBase + kModuleSize + 0x10000; // well past the module
    PlaceInstruction(reader, kModuleBase, 0x100, outOfRangeTarget);

    std::uintptr_t target = 0;
    AobScanResult result = ResolveViaFullPipeline(reader, target);

    SH_CHECK(result == AobScanResult::TargetOutOfRange);
}

SH_TEST(AobFixture_Negative_MidScanReadFailure_Fails) {
    FakeProcessReader reader;
    PlaceInstruction(reader, kModuleBase, 0x100, kModuleBase + 0x500); // a real match in the first chunk
    reader.FailReadAtCall(1);                                          // the second chunk's read then fails

    AobPattern pattern;
    SH_CHECK(ParseAobPattern(kPatternText, pattern) == AobScanResult::Success);

    std::uintptr_t foundAddress = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kModuleBase, kAobScanChunkBytes * 2, pattern, foundAddress);

    SH_CHECK(result == AobScanResult::ReadFailed);
    SH_CHECK(foundAddress == 0xDEAD);
}
