#include "sekiro_haptics/process/CandidateScanner.hpp"

#include "sekiro_haptics/process/CandidateScanShared.hpp"

#include <algorithm>
#include <cstring>

namespace sekiro_haptics::process {

namespace {

std::uint32_t ReadU32LE(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

CandidateScanResult MapReadFailure(ProcessReaderResult result) {
    switch (result) {
        case ProcessReaderResult::NotAttached:
            return CandidateScanResult::NotAttached;
        case ProcessReaderResult::ProcessExited:
            return CandidateScanResult::ProcessExited;
        case ProcessReaderResult::PartialRead:
            return CandidateScanResult::PartialRead;
        default:
            return CandidateScanResult::ReadFailed;
    }
}

} // namespace

std::size_t CandidateValueTypeSize(CandidateValueType type) {
    switch (type) {
        case CandidateValueType::U8:
            return 1;
        case CandidateValueType::U16:
            return 2;
        case CandidateValueType::U32:
        case CandidateValueType::I32:
        case CandidateValueType::F32:
            return 4;
    }
    return 4;
}

CandidateValue DecodeCandidateValue(CandidateValueType type, const std::uint8_t* bytes) {
    switch (type) {
        case CandidateValueType::U8:
            return CandidateValue{bytes[0]};
        case CandidateValueType::U16:
            return CandidateValue{static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8))};
        case CandidateValueType::U32:
            return CandidateValue{ReadU32LE(bytes)};
        case CandidateValueType::I32:
            return CandidateValue{static_cast<std::int32_t>(ReadU32LE(bytes))};
        case CandidateValueType::F32: {
            std::uint32_t raw = ReadU32LE(bytes);
            float f = 0.0f;
            std::memcpy(&f, &raw, sizeof(f));
            return CandidateValue{f};
        }
    }
    return CandidateValue{std::uint32_t{0}};
}

const char* ToString(CandidateScanScope scope) {
    switch (scope) {
        case CandidateScanScope::MainModule:
            return "main-module";
        case CandidateScanScope::PrivateReadable:
            return "private-readable";
        case CandidateScanScope::AllReadable:
            return "all-readable";
    }
    return "unknown";
}

const char* ToString(CandidateValueType type) {
    switch (type) {
        case CandidateValueType::U8:
            return "u8";
        case CandidateValueType::U16:
            return "u16";
        case CandidateValueType::U32:
            return "u32";
        case CandidateValueType::I32:
            return "i32";
        case CandidateValueType::F32:
            return "f32";
    }
    return "unknown";
}

CandidateScanResult BeginCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                        const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                        CandidateValueType type, std::vector<Candidate>& outCandidates,
                                        std::size_t maxCandidates, std::uint64_t maxScanBytes) {
    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult mapResult = memoryMap.EnumerateReadableRegions(regions);
    if (mapResult != MemoryMapResult::Success) {
        if (mapResult == MemoryMapResult::NotAttached) {
            return CandidateScanResult::NotAttached;
        }
        if (mapResult == MemoryMapResult::ProcessExited) {
            return CandidateScanResult::ProcessExited;
        }
        return CandidateScanResult::MemoryMapUnavailable;
    }

    std::vector<ProcessMemoryRegion> targetRegions;
    CandidateScanResult scopeResult = ResolveScanScopeRegions(inspector, regions, scope, targetRegions);
    if (scopeResult != CandidateScanResult::Success) {
        return scopeResult;
    }

    std::size_t valueSize = CandidateValueTypeSize(type);
    std::size_t overlap = valueSize - 1;

    std::vector<Candidate> collected;
    std::uint64_t totalBytesScanned = 0;
    std::vector<std::uint8_t> chunkBuffer;

    for (const ProcessMemoryRegion& region : targetRegions) {
        std::size_t regionOffset = 0;
        while (regionOffset < region.sizeBytes) {
            std::size_t remaining = region.sizeBytes - regionOffset;
            std::size_t ownedChunkSize = std::min(kCandidateScanChunkBytes, remaining);
            std::size_t readSize = std::min(ownedChunkSize + overlap, remaining);

            if (totalBytesScanned + readSize > maxScanBytes) {
                return CandidateScanResult::CandidateLimitExceeded;
            }

            std::uintptr_t chunkAddress = region.baseAddress + regionOffset;
            if (chunkAddress < region.baseAddress) {
                return CandidateScanResult::AddressOverflow;
            }
            std::uintptr_t chunkEnd = chunkAddress + readSize;
            if (chunkEnd < chunkAddress) {
                return CandidateScanResult::AddressOverflow;
            }

            chunkBuffer.assign(readSize, 0);
            ProcessReaderResult readResult = reader.ReadBytes(chunkAddress, chunkBuffer.data(), readSize);
            if (readResult != ProcessReaderResult::Success) {
                return MapReadFailure(readResult);
            }
            totalBytesScanned += readSize;

            std::size_t addressRemainder = chunkAddress % valueSize;
            std::size_t alignedStart = (addressRemainder == 0) ? 0 : (valueSize - addressRemainder);

            for (std::size_t i = alignedStart; i + valueSize <= readSize && i < ownedChunkSize; i += valueSize) {
                Candidate candidate;
                candidate.address = chunkAddress + i;
                candidate.type = type;
                candidate.value = DecodeCandidateValue(type, &chunkBuffer[i]);
                collected.push_back(candidate);

                if (collected.size() > maxCandidates) {
                    return CandidateScanResult::CandidateLimitExceeded;
                }
            }

            regionOffset += ownedChunkSize;
        }
    }

    outCandidates = std::move(collected);
    return CandidateScanResult::Success;
}

CandidateFilterResult FilterCandidates(IProcessReader& reader, std::vector<Candidate>& candidates,
                                        CandidateFilterKind kind, const CandidateValue* exactTarget) {
    if (kind == CandidateFilterKind::ExactValue && exactTarget == nullptr) {
        return CandidateFilterResult::InvalidTarget;
    }

    std::vector<Candidate> survivors;
    survivors.reserve(candidates.size());

    for (const Candidate& candidate : candidates) {
        std::size_t valueSize = CandidateValueTypeSize(candidate.type);
        std::uint8_t buffer[4] = {};
        ProcessReaderResult readResult = reader.ReadBytes(candidate.address, buffer, valueSize);

        if (readResult == ProcessReaderResult::NotAttached) {
            return CandidateFilterResult::NotAttached;
        }
        if (readResult == ProcessReaderResult::ProcessExited) {
            return CandidateFilterResult::ProcessExited;
        }
        if (readResult != ProcessReaderResult::Success) {
            // A hard failure or partial read for just this one address --
            // drop only this candidate; the process is still attached and
            // alive, so this is not a whole-command failure.
            continue;
        }

        CandidateValue currentValue = DecodeCandidateValue(candidate.type, buffer);
        if (!EvaluateCandidateFilter(kind, candidate.value, currentValue, exactTarget)) {
            continue;
        }

        Candidate updated = candidate;
        updated.value = currentValue;
        survivors.push_back(updated);
    }

    candidates = std::move(survivors);
    return CandidateFilterResult::Success;
}

const char* ToString(CandidateScanResult result) {
    switch (result) {
        case CandidateScanResult::Success:
            return "Success";
        case CandidateScanResult::NotAttached:
            return "NotAttached";
        case CandidateScanResult::ProcessExited:
            return "ProcessExited";
        case CandidateScanResult::ReadFailed:
            return "ReadFailed";
        case CandidateScanResult::PartialRead:
            return "PartialRead";
        case CandidateScanResult::ModuleNotFound:
            return "ModuleNotFound";
        case CandidateScanResult::MemoryMapUnavailable:
            return "MemoryMapUnavailable";
        case CandidateScanResult::CandidateLimitExceeded:
            return "CandidateLimitExceeded";
        case CandidateScanResult::AddressOverflow:
            return "AddressOverflow";
    }
    return "Unknown";
}

const char* ToString(CandidateFilterResult result) {
    switch (result) {
        case CandidateFilterResult::Success:
            return "Success";
        case CandidateFilterResult::NotAttached:
            return "NotAttached";
        case CandidateFilterResult::ProcessExited:
            return "ProcessExited";
        case CandidateFilterResult::InvalidTarget:
            return "InvalidTarget";
    }
    return "Unknown";
}

} // namespace sekiro_haptics::process
