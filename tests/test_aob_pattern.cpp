// Unit tests for ParseAobPattern() -- pure text parsing, no scanning or
// process access. See docs/05-process-access.md for the full grammar.

#include "sekiro_haptics/process/AobPattern.hpp"
#include "testing.hpp"

#include <sstream>
#include <string>

using namespace sekiro_haptics::process;

SH_TEST(ParseAobPattern_ExactBytes_ParsesValuesAndExactMask) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 8B 05", pattern) == AobScanResult::Success);

    SH_CHECK(pattern.size() == 3);
    SH_CHECK(pattern.bytes[0] == 0x48);
    SH_CHECK(pattern.bytes[1] == 0x8B);
    SH_CHECK(pattern.bytes[2] == 0x05);
    SH_CHECK(pattern.mask[0] == true);
    SH_CHECK(pattern.mask[1] == true);
    SH_CHECK(pattern.mask[2] == true);
}

SH_TEST(ParseAobPattern_LowercaseHex_ParsesSameAsUppercase) {
    AobPattern lower;
    AobPattern upper;
    SH_CHECK(ParseAobPattern("4a 8b 0f", lower) == AobScanResult::Success);
    SH_CHECK(ParseAobPattern("4A 8B 0F", upper) == AobScanResult::Success);

    SH_CHECK(lower.bytes == upper.bytes);
    SH_CHECK(lower.mask == upper.mask);
}

SH_TEST(ParseAobPattern_MixedCaseWithinOneToken_Parses) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("4a", pattern) == AobScanResult::Success);
    SH_CHECK(pattern.bytes[0] == 0x4A);
}

SH_TEST(ParseAobPattern_SingleQuestionMark_IsWildcard) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 ? 05", pattern) == AobScanResult::Success);

    SH_CHECK(pattern.size() == 3);
    SH_CHECK(pattern.mask[0] == true);
    SH_CHECK(pattern.mask[1] == false);
    SH_CHECK(pattern.mask[2] == true);
}

SH_TEST(ParseAobPattern_DoubleQuestionMark_IsWildcard) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 ?? 05", pattern) == AobScanResult::Success);

    SH_CHECK(pattern.size() == 3);
    SH_CHECK(pattern.mask[1] == false);
}

SH_TEST(ParseAobPattern_ExactAndWildcardMixed_PreservesOrderAndMask) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 8B ?? 05 ? EF", pattern) == AobScanResult::Success);

    SH_CHECK(pattern.size() == 6);
    bool expectedMask[6] = {true, true, false, true, false, true};
    for (int i = 0; i < 6; ++i) {
        SH_CHECK(pattern.mask[i] == expectedMask[i]);
    }
    SH_CHECK(pattern.bytes[0] == 0x48);
    SH_CHECK(pattern.bytes[1] == 0x8B);
    SH_CHECK(pattern.bytes[3] == 0x05);
    SH_CHECK(pattern.bytes[5] == 0xEF);
}

SH_TEST(ParseAobPattern_EmptyString_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_OnlyWhitespace_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("   \t  ", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_MalformedHex_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 GG 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_NonHexSymbols_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 $$ 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_SingleHexDigit_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 4 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_ThreeOrMoreCharacterToken_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 8B0 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_PartialWildcardHexThenQuestionMark_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 4? 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_PartialWildcardQuestionMarkThenHex_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("48 ?F 05", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_WildcardOnlyPattern_ReturnsInvalidPattern) {
    AobPattern pattern;
    SH_CHECK(ParseAobPattern("? ?? ? ??", pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_AtMaxLength_Succeeds) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < kMaxAobPatternBytes; ++i) {
        oss << "AB ";
    }
    AobPattern pattern;
    SH_CHECK(ParseAobPattern(oss.str(), pattern) == AobScanResult::Success);
    SH_CHECK(pattern.size() == kMaxAobPatternBytes);
}

SH_TEST(ParseAobPattern_OneOverMaxLength_ReturnsInvalidPattern) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < kMaxAobPatternBytes + 1; ++i) {
        oss << "AB ";
    }
    AobPattern pattern;
    SH_CHECK(ParseAobPattern(oss.str(), pattern) == AobScanResult::InvalidPattern);
}

SH_TEST(ParseAobPattern_FailedParse_NeverLeavesPartiallyFilledPattern) {
    AobPattern pattern;
    pattern.bytes = {0xAA, 0xBB};
    pattern.mask = {true, true};

    SH_CHECK(ParseAobPattern("48 GG", pattern) == AobScanResult::InvalidPattern);

    // outPattern is untouched on failure.
    SH_CHECK(pattern.bytes.size() == 2);
    SH_CHECK(pattern.bytes[0] == 0xAA);
    SH_CHECK(pattern.bytes[1] == 0xBB);
}
