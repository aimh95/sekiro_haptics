#include "sekiro_haptics/process/AobPattern.hpp"

#include <sstream>

namespace sekiro_haptics::process {

namespace {

bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return 10 + (c - 'A');
}

} // namespace

const char* ToString(AobScanResult result) {
    switch (result) {
        case AobScanResult::Success:
            return "Success";
        case AobScanResult::InvalidPattern:
            return "InvalidPattern";
        case AobScanResult::InvalidRange:
            return "InvalidRange";
        case AobScanResult::NoMatch:
            return "NoMatch";
        case AobScanResult::MultipleMatches:
            return "MultipleMatches";
        case AobScanResult::NotAttached:
            return "NotAttached";
        case AobScanResult::ProcessExited:
            return "ProcessExited";
        case AobScanResult::ReadFailed:
            return "ReadFailed";
        case AobScanResult::PartialRead:
            return "PartialRead";
        case AobScanResult::AddressOverflow:
            return "AddressOverflow";
        case AobScanResult::InvalidDisplacementLayout:
            return "InvalidDisplacementLayout";
        case AobScanResult::TargetOutOfRange:
            return "TargetOutOfRange";
    }
    return "Unknown";
}

AobScanResult ParseAobPattern(const std::string& text, AobPattern& outPattern) {
    std::istringstream iss(text);
    std::string token;
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;
    bool sawExactByte = false;

    while (iss >> token) {
        if (token.size() == 1) {
            if (token[0] == '?') {
                bytes.push_back(0);
                mask.push_back(false);
                continue;
            }
            return AobScanResult::InvalidPattern;
        }

        if (token.size() == 2) {
            bool wild0 = token[0] == '?';
            bool wild1 = token[1] == '?';
            if (wild0 && wild1) {
                bytes.push_back(0);
                mask.push_back(false);
                continue;
            }
            if (wild0 || wild1) {
                return AobScanResult::InvalidPattern; // partial wildcard, e.g. "4?" or "?F"
            }
            if (!IsHexDigit(token[0]) || !IsHexDigit(token[1])) {
                return AobScanResult::InvalidPattern;
            }
            bytes.push_back(static_cast<std::uint8_t>((HexValue(token[0]) << 4) | HexValue(token[1])));
            mask.push_back(true);
            sawExactByte = true;
            continue;
        }

        return AobScanResult::InvalidPattern; // three or more characters
    }

    if (bytes.empty()) {
        return AobScanResult::InvalidPattern; // no tokens at all
    }
    if (bytes.size() > kMaxAobPatternBytes) {
        return AobScanResult::InvalidPattern;
    }
    if (!sawExactByte) {
        return AobScanResult::InvalidPattern; // wildcard-only pattern
    }

    AobPattern pattern;
    pattern.bytes = std::move(bytes);
    pattern.mask = std::move(mask);
    outPattern = std::move(pattern);
    return AobScanResult::Success;
}

} // namespace sekiro_haptics::process
