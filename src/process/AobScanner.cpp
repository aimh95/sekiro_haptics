#include "sekiro_haptics/process/AobScanner.hpp"

#include <algorithm>
#include <vector>

namespace sekiro_haptics::process {

namespace {

bool MatchesAt(const std::uint8_t* buffer, std::size_t start, const AobPattern& pattern) {
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern.mask[i] && buffer[start + i] != pattern.bytes[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

AobScanResult ScanBuffer(const std::uint8_t* buffer, std::size_t bufferSize, const AobPattern& pattern,
                          std::size_t& outOffset) {
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size()) {
        return AobScanResult::InvalidPattern;
    }
    if (pattern.size() > bufferSize) {
        return AobScanResult::NoMatch;
    }

    std::size_t matchCount = 0;
    std::size_t firstMatch = 0;
    std::size_t lastStart = bufferSize - pattern.size();
    for (std::size_t start = 0; start <= lastStart; ++start) {
        if (MatchesAt(buffer, start, pattern)) {
            if (matchCount == 0) {
                firstMatch = start;
            }
            ++matchCount;
        }
    }

    if (matchCount == 0) {
        return AobScanResult::NoMatch;
    }
    if (matchCount > 1) {
        return AobScanResult::MultipleMatches;
    }
    outOffset = firstMatch;
    return AobScanResult::Success;
}

AobScanResult ScanProcessRange(IProcessReader& reader, std::uintptr_t rangeBase, std::size_t rangeSize,
                                const AobPattern& pattern, std::uintptr_t& outAddress) {
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size()) {
        return AobScanResult::InvalidPattern;
    }
    if (rangeBase == 0 || rangeSize == 0) {
        return AobScanResult::InvalidRange;
    }
    std::uintptr_t rangeEnd = rangeBase + rangeSize;
    if (rangeEnd < rangeBase) {
        return AobScanResult::InvalidRange;
    }
    if (pattern.size() > rangeSize) {
        return AobScanResult::NoMatch;
    }
    if (!reader.IsAttached()) {
        return AobScanResult::NotAttached;
    }

    std::size_t patternSize = pattern.size();
    std::size_t overlap = patternSize - 1;
    std::vector<std::uintptr_t> matches;
    std::vector<std::uint8_t> chunkBuffer;

    std::size_t chunkStartOffset = 0;
    while (chunkStartOffset < rangeSize) {
        std::size_t remaining = rangeSize - chunkStartOffset;
        std::size_t ownedChunkSize = std::min(kAobScanChunkBytes, remaining);
        std::size_t readSize = std::min(ownedChunkSize + overlap, remaining);

        chunkBuffer.assign(readSize, 0);
        ProcessReaderResult readResult =
            reader.ReadBytes(rangeBase + chunkStartOffset, chunkBuffer.data(), readSize);
        switch (readResult) {
            case ProcessReaderResult::Success:
                break;
            case ProcessReaderResult::NotAttached:
                return AobScanResult::NotAttached;
            case ProcessReaderResult::ProcessExited:
                return AobScanResult::ProcessExited;
            case ProcessReaderResult::PartialRead:
                return AobScanResult::PartialRead;
            default:
                return AobScanResult::ReadFailed;
        }

        // Only start offsets within this chunk's own (non-overlap)
        // portion are counted here -- a match starting at or beyond
        // ownedChunkSize belongs to the NEXT chunk's own window (which
        // will scan it as one of its own, smaller-offset start
        // positions), so it is never counted twice.
        std::size_t maxStart = readSize >= patternSize ? readSize - patternSize + 1 : 0;
        maxStart = std::min(maxStart, ownedChunkSize);
        for (std::size_t start = 0; start < maxStart; ++start) {
            if (MatchesAt(chunkBuffer.data(), start, pattern)) {
                matches.push_back(rangeBase + chunkStartOffset + start);
            }
        }

        chunkStartOffset += ownedChunkSize;
    }

    // A process that exited in the instant after the last chunk read
    // (but before we act on the results) must not have its matches used.
    if (!reader.IsAlive()) {
        return AobScanResult::ProcessExited;
    }

    if (matches.empty()) {
        return AobScanResult::NoMatch;
    }
    if (matches.size() > 1) {
        return AobScanResult::MultipleMatches;
    }

    std::uintptr_t matchAddress = matches.front();
    std::uintptr_t matchEnd = matchAddress + patternSize;
    if (matchAddress < rangeBase || matchEnd < matchAddress || matchEnd > rangeEnd) {
        return AobScanResult::InvalidRange;
    }

    outAddress = matchAddress;
    return AobScanResult::Success;
}

} // namespace sekiro_haptics::process
