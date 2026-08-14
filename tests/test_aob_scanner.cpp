// Unit tests for ScanBuffer() (pure buffer scan) and ScanProcessRange()
// (chunked IProcessReader-based range scan). No real process is touched
// here -- ScanProcessRange is driven entirely through FakeProcessReader.

#include "sekiro_haptics/process/AobScanner.hpp"
#include "testing.hpp"

#include "FakeProcessReader.hpp"

#include <cstring>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

AobPattern ExactPattern(std::initializer_list<std::uint8_t> bytes) {
    AobPattern p;
    for (std::uint8_t b : bytes) {
        p.bytes.push_back(b);
        p.mask.push_back(true);
    }
    return p;
}

} // namespace

// ===========================================================================
// ScanBuffer
// ===========================================================================

SH_TEST(ScanBuffer_MatchAtStart_Succeeds) {
    std::uint8_t buffer[8] = {0x11, 0x22, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00};
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33});

    std::size_t offset = 999;
    AobScanResult result = ScanBuffer(buffer, sizeof(buffer), pattern, offset);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(offset == 0);
}

SH_TEST(ScanBuffer_MatchInMiddle_Succeeds) {
    std::uint8_t buffer[8] = {0x00, 0x00, 0x11, 0x22, 0x33, 0x00, 0x00, 0x00};
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::Success);
    SH_CHECK(offset == 2);
}

SH_TEST(ScanBuffer_MatchAtLastPossiblePosition_Succeeds) {
    std::uint8_t buffer[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33};
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::Success);
    SH_CHECK(offset == 5);
}

SH_TEST(ScanBuffer_WildcardByte_MatchesAnyValue) {
    std::uint8_t buffer[4] = {0x11, 0xFA, 0x33, 0x00};
    AobPattern pattern;
    pattern.bytes = {0x11, 0x00, 0x33};
    pattern.mask = {true, false, true};

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::Success);
    SH_CHECK(offset == 0);
}

SH_TEST(ScanBuffer_NoMatch_ReturnsNoMatchAndLeavesOutputUnchanged) {
    std::uint8_t buffer[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    AobPattern pattern = ExactPattern({0xAA, 0xBB});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::NoMatch);
    SH_CHECK(offset == 999);
}

SH_TEST(ScanBuffer_MultipleMatches_ReturnsMultipleMatchesAndLeavesOutputUnchanged) {
    std::uint8_t buffer[6] = {0x11, 0x22, 0x00, 0x11, 0x22, 0x00};
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::MultipleMatches);
    SH_CHECK(offset == 999);
}

SH_TEST(ScanBuffer_OverlappingMatches_BothDetectedAsMultipleMatches) {
    // "11 11 11" contains two overlapping occurrences of "11 11" (offset 0
    // and offset 1) -- neither must be silently ignored.
    std::uint8_t buffer[3] = {0x11, 0x11, 0x11};
    AobPattern pattern = ExactPattern({0x11, 0x11});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::MultipleMatches);
}

SH_TEST(ScanBuffer_PatternLargerThanBuffer_ReturnsNoMatch) {
    std::uint8_t buffer[2] = {0x11, 0x22};
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::NoMatch);
    SH_CHECK(offset == 999);
}

SH_TEST(ScanBuffer_LastByteMismatch_ComparesEveryByteNotJustPrefix) {
    std::uint8_t buffer[4] = {0x11, 0x22, 0x33, 0x99}; // last byte differs from pattern's last byte
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44});

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::NoMatch);
}

SH_TEST(ScanBuffer_EmptyPattern_ReturnsInvalidPattern) {
    std::uint8_t buffer[4] = {0, 1, 2, 3};
    AobPattern pattern; // no bytes at all

    std::size_t offset = 999;
    SH_CHECK(ScanBuffer(buffer, sizeof(buffer), pattern, offset) == AobScanResult::InvalidPattern);
    SH_CHECK(offset == 999);
}

// ===========================================================================
// ScanProcessRange
// ===========================================================================

namespace {
constexpr std::uintptr_t kBase = 0x10000;
} // namespace

SH_TEST(ScanProcessRange_SingleChunkMatch_Succeeds) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    reader.PokeBytes(kBase + 100, pattern.bytes.data(), pattern.bytes.size());

    std::uintptr_t address = 0;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(address == kBase + 100);
}

SH_TEST(ScanProcessRange_MatchStraddlingChunkBoundary_FoundExactlyOnce) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    // Placed so the 6 bytes span [chunk-3, chunk+3) -- straddling the
    // boundary between the first and second chunk.
    std::size_t patternOffset = kAobScanChunkBytes - 3;
    reader.PokeBytes(kBase + patternOffset, pattern.bytes.data(), pattern.bytes.size());

    std::uintptr_t address = 0;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes * 2, pattern, address);

    // Success (not MultipleMatches) proves it's found exactly once despite
    // being visible in both chunk 1's overlap tail and chunk 2's own read.
    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(address == kBase + patternOffset);
}

SH_TEST(ScanProcessRange_MatchesInDifferentChunks_ReturnsMultipleMatches) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    reader.PokeBytes(kBase + 100, pattern.bytes.data(), pattern.bytes.size());                       // chunk 0
    reader.PokeBytes(kBase + kAobScanChunkBytes + 100, pattern.bytes.data(), pattern.bytes.size());   // chunk 1

    std::uintptr_t address = 0;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes * 2, pattern, address);

    SH_CHECK(result == AobScanResult::MultipleMatches);
}

SH_TEST(ScanProcessRange_FirstChunkReadFails_ReturnsReadFailed) {
    FakeProcessReader reader;
    reader.FailReadAtCall(0);
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address);

    SH_CHECK(result == AobScanResult::ReadFailed);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_MiddleChunkReadFails_ReturnsReadFailed) {
    FakeProcessReader reader;
    reader.FailReadAtCall(1); // 2nd of 3 chunk reads
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes * 3, pattern, address);

    SH_CHECK(result == AobScanResult::ReadFailed);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_MatchFoundThenLaterChunkFails_DiscardsEarlierMatchAndFailsOverall) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    reader.PokeBytes(kBase + 100, pattern.bytes.data(), pattern.bytes.size()); // a real match in chunk 0
    reader.FailReadAtCall(1);                                                  // chunk 1's read then fails

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes * 2, pattern, address);

    SH_CHECK(result == AobScanResult::ReadFailed);
    SH_CHECK(address == 0xDEAD); // the chunk-0 match is never reported
}

SH_TEST(ScanProcessRange_PartialRead_ReturnsPartialRead) {
    FakeProcessReader reader;
    reader.ForcePartialReadAtCall(0, 10);
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address);

    SH_CHECK(result == AobScanResult::PartialRead);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_ProcessExited_ReturnsProcessExited) {
    FakeProcessReader reader;
    reader.SetAlive(false);
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address);

    SH_CHECK(result == AobScanResult::ProcessExited);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_NotAttached_ReturnsNotAttached) {
    FakeProcessReader reader;
    reader.SetAttached(false);
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0xDEAD;
    AobScanResult result = ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address);

    SH_CHECK(result == AobScanResult::NotAttached);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_ZeroSize_ReturnsInvalidRange) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0;
    SH_CHECK(ScanProcessRange(reader, kBase, 0, pattern, address) == AobScanResult::InvalidRange);
}

SH_TEST(ScanProcessRange_ZeroBase_ReturnsInvalidRange) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22});

    std::uintptr_t address = 0;
    SH_CHECK(ScanProcessRange(reader, 0, kAobScanChunkBytes, pattern, address) == AobScanResult::InvalidRange);
}

SH_TEST(ScanProcessRange_RangeOverflow_ReturnsInvalidRange) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22});
    std::uintptr_t nearMax = static_cast<std::uintptr_t>(-1) - 10;

    std::uintptr_t address = 0;
    SH_CHECK(ScanProcessRange(reader, nearMax, 1000, pattern, address) == AobScanResult::InvalidRange);
}

SH_TEST(ScanProcessRange_PatternLargerThanRange_ReturnsNoMatch) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});

    std::uintptr_t address = 0;
    SH_CHECK(ScanProcessRange(reader, kBase, 3, pattern, address) == AobScanResult::NoMatch);
}

SH_TEST(ScanProcessRange_NoMatch_ReturnsNoMatch) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0xAA, 0xBB, 0xCC});

    std::uintptr_t address = 0xDEAD;
    SH_CHECK(ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address) == AobScanResult::NoMatch);
    SH_CHECK(address == 0xDEAD);
}

SH_TEST(ScanProcessRange_InvalidPattern_ReturnsInvalidPattern) {
    FakeProcessReader reader;
    AobPattern pattern; // empty

    std::uintptr_t address = 0;
    SH_CHECK(ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address) == AobScanResult::InvalidPattern);
}

SH_TEST(ScanProcessRange_SuccessiveCalls_DoNotShareStateBetweenScans) {
    FakeProcessReader reader;
    AobPattern pattern = ExactPattern({0x11, 0x22, 0x33});
    reader.PokeBytes(kBase + 10, pattern.bytes.data(), pattern.bytes.size());
    reader.PokeBytes(kBase + 200, pattern.bytes.data(), pattern.bytes.size());

    std::uintptr_t address = 0;
    // First scan: two matches present -> MultipleMatches.
    SH_CHECK(ScanProcessRange(reader, kBase, kAobScanChunkBytes, pattern, address) == AobScanResult::MultipleMatches);

    // Second scan, over a narrower range covering only the first match --
    // must not be influenced by the previous call's result.
    SH_CHECK(ScanProcessRange(reader, kBase, 50, pattern, address) == AobScanResult::Success);
    SH_CHECK(address == kBase + 10);
}
