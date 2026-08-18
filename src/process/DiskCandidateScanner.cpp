#include "sekiro_haptics/process/DiskCandidateScanner.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

namespace sekiro_haptics::process {

namespace {

std::int64_t NowMonotonicUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t SaturatingMultiply(std::uint64_t a, std::uint64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    if (a > std::numeric_limits<std::uint64_t>::max() / b) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a * b;
}

std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a + b;
}

DiskScanOutcome MapReaderFailure(ProcessReaderResult result) {
    switch (result) {
        case ProcessReaderResult::NotAttached:
            return DiskScanOutcome::NotAttached;
        case ProcessReaderResult::ProcessExited:
            return DiskScanOutcome::ProcessExited;
        case ProcessReaderResult::PartialRead:
            return DiskScanOutcome::IncompleteCoverage;
        default:
            return DiskScanOutcome::ReadFailed;
    }
}

DiskScanOutcome MapMemoryMapFailure(MemoryMapResult result) {
    switch (result) {
        case MemoryMapResult::NotAttached:
            return DiskScanOutcome::NotAttached;
        case MemoryMapResult::ProcessExited:
            return DiskScanOutcome::ProcessExited;
        default:
            return DiskScanOutcome::MemoryMapUnavailable;
    }
}

/// "candidates-0001", "candidates-0002", ... -- matches the ticket's own
/// example naming exactly.
std::string GenerationFileBaseName(int generation) {
    std::ostringstream oss;
    oss << "candidates-" << std::setfill('0') << std::setw(4) << generation;
    return oss.str();
}

} // namespace

const char* ToString(DiskScanOutcome outcome) {
    switch (outcome) {
        case DiskScanOutcome::CompleteCoverage:
            return "CompleteCoverage";
        case DiskScanOutcome::IncompleteCoverage:
            return "IncompleteCoverage";
        case DiskScanOutcome::Interrupted:
            return "Interrupted";
        case DiskScanOutcome::ReadFailed:
            return "ReadFailed";
        case DiskScanOutcome::WriteFailed:
            return "WriteFailed";
        case DiskScanOutcome::FlushFailed:
            return "FlushFailed";
        case DiskScanOutcome::InsufficientDiskSpace:
            return "InsufficientDiskSpace";
        case DiskScanOutcome::NotAttached:
            return "NotAttached";
        case DiskScanOutcome::ProcessExited:
            return "ProcessExited";
        case DiskScanOutcome::ModuleNotFound:
            return "ModuleNotFound";
        case DiskScanOutcome::MemoryMapUnavailable:
            return "MemoryMapUnavailable";
        case DiskScanOutcome::AddressOverflow:
            return "AddressOverflow";
        case DiskScanOutcome::SessionMismatch:
            return "SessionMismatch";
        case DiskScanOutcome::InitialFilterTooBroad:
            return "InitialFilterTooBroad";
        case DiskScanOutcome::InvalidTarget:
            return "InvalidTarget";
        case DiskScanOutcome::CorruptFile:
            return "CorruptFile";
    }
    return "Unknown";
}

const char* ToString(PlanCandidateScanOutcome outcome) {
    switch (outcome) {
        case PlanCandidateScanOutcome::Success:
            return "Success";
        case PlanCandidateScanOutcome::NotAttached:
            return "NotAttached";
        case PlanCandidateScanOutcome::ProcessExited:
            return "ProcessExited";
        case PlanCandidateScanOutcome::ModuleNotFound:
            return "ModuleNotFound";
        case PlanCandidateScanOutcome::MemoryMapUnavailable:
            return "MemoryMapUnavailable";
        case PlanCandidateScanOutcome::AddressOverflow:
            return "AddressOverflow";
        case PlanCandidateScanOutcome::InsufficientDiskSpace:
            return "InsufficientDiskSpace";
    }
    return "Unknown";
}

std::string CurrentDataFileName(int generation) {
    if (generation == 0) {
        return "baseline.bin";
    }
    return GenerationFileBaseName(generation) + ".bin";
}

const char* ToString(RecommendedStorageMode mode) {
    switch (mode) {
        case RecommendedStorageMode::InMemory:
            return "InMemory";
        case RecommendedStorageMode::DiskBacked:
            return "DiskBacked";
    }
    return "Unknown";
}

// =====================================================================
// PlanCandidateScan
// =====================================================================

PlanCandidateScanOutcome PlanCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                            const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                            CandidateValueType type, const std::filesystem::path& outputDrivePath,
                                            std::size_t memoryBudgetBytes, ScanPlanReport& outReport) {
    (void)reader;

    std::vector<ProcessMemoryRegion> allRegions;
    MemoryMapResult mapResult = memoryMap.EnumerateReadableRegions(allRegions);
    if (mapResult != MemoryMapResult::Success) {
        if (mapResult == MemoryMapResult::NotAttached) {
            return PlanCandidateScanOutcome::NotAttached;
        }
        if (mapResult == MemoryMapResult::ProcessExited) {
            return PlanCandidateScanOutcome::ProcessExited;
        }
        return PlanCandidateScanOutcome::MemoryMapUnavailable;
    }

    std::vector<ProcessMemoryRegion> targetRegions;
    CandidateScanResult scopeResult = ResolveScanScopeRegions(inspector, allRegions, scope, targetRegions);
    if (scopeResult != CandidateScanResult::Success) {
        if (scopeResult == CandidateScanResult::ModuleNotFound) {
            return PlanCandidateScanOutcome::ModuleNotFound;
        }
        return PlanCandidateScanOutcome::MemoryMapUnavailable;
    }

    CandidateScanPlanTotals totals = ComputeCandidateScanPlanTotals(targetRegions, type);
    std::size_t valueSize = CandidateValueTypeSize(type);

    ScanPlanReport report;
    report.regionCount = totals.regionCount;
    report.totalScopeBytes = totals.totalScopeBytes;
    report.comparableValueCount = totals.comparableValueCount;
    report.memoryBudgetBytes = memoryBudgetBytes;

    report.estimatedInMemoryRamBytes = SaturatingMultiply(totals.comparableValueCount, sizeof(Candidate));

    std::uint64_t regionDirectoryBytes = SaturatingMultiply(static_cast<std::uint64_t>(totals.regionCount),
                                                              static_cast<std::uint64_t>(kBaselineRegionEntryBytes));
    std::uint64_t baselineValueBytes = SaturatingMultiply(totals.comparableValueCount, static_cast<std::uint64_t>(valueSize));
    report.estimatedBaselineFileBytes =
        SaturatingAdd(SaturatingAdd(SaturatingAdd(kBaselineHeaderBytes, regionDirectoryBytes), baselineValueBytes),
                       kBaselineFooterBytes);

    std::uint64_t generationRecordBytes = SaturatingMultiply(
        totals.comparableValueCount, static_cast<std::uint64_t>(8 + valueSize)); // worst case: every candidate survives
    report.estimatedMaxGenerationFileBytes =
        SaturatingAdd(SaturatingAdd(kGenerationHeaderBytes, generationRecordBytes), kGenerationFooterBytes);

    report.recommendedStorageMode = (CheckInMemoryScanBudget(totals, memoryBudgetBytes) == InMemoryBudgetCheckResult::WithinBudget)
                                         ? RecommendedStorageMode::InMemory
                                         : RecommendedStorageMode::DiskBacked;

    std::error_code spaceError;
    std::filesystem::space_info spaceInfo = std::filesystem::space(outputDrivePath, spaceError);
    report.availableDiskSpaceBytes = spaceError ? 0 : static_cast<std::uint64_t>(spaceInfo.available);

    // Baseline + one worst-case generation + generous headroom for the
    // manifest/session JSON and filesystem overhead -- computed
    // pessimistically, never optimistically hidden.
    constexpr std::uint64_t kManifestHeadroomBytes = 4ULL * 1024 * 1024;
    report.requiredDiskSpaceBytes = SaturatingAdd(
        SaturatingAdd(report.estimatedBaselineFileBytes, report.estimatedMaxGenerationFileBytes), kManifestHeadroomBytes);

    outReport = report;

    if (spaceError || report.availableDiskSpaceBytes < report.requiredDiskSpaceBytes) {
        return PlanCandidateScanOutcome::InsufficientDiskSpace;
    }
    return PlanCandidateScanOutcome::Success;
}

// =====================================================================
// BeginDiskCandidateScan
// =====================================================================

DiskScanOutcome BeginDiskCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                        const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                        CandidateValueType type, const std::filesystem::path& sessionDir,
                                        const ScanSessionIdentity& identity, std::size_t memoryBudgetBytes,
                                        DiskScanStats& outStats, const std::function<void(const DiskScanStats&)>& onProgress) {
    DiskScanStats stats;
    stats.configuredMemoryBudgetBytes = memoryBudgetBytes;

    std::vector<ProcessMemoryRegion> allRegions;
    MemoryMapResult mapResult = memoryMap.EnumerateReadableRegions(allRegions);
    if (mapResult != MemoryMapResult::Success) {
        outStats = stats;
        return MapMemoryMapFailure(mapResult);
    }

    std::vector<ProcessMemoryRegion> targetRegions;
    CandidateScanResult scopeResult = ResolveScanScopeRegions(inspector, allRegions, scope, targetRegions);
    if (scopeResult != CandidateScanResult::Success) {
        outStats = stats;
        return scopeResult == CandidateScanResult::ModuleNotFound ? DiskScanOutcome::ModuleNotFound
                                                                    : DiskScanOutcome::MemoryMapUnavailable;
    }

    std::size_t valueSize = CandidateValueTypeSize(type);
    CandidateScanPlanTotals totals = ComputeCandidateScanPlanTotals(targetRegions, type);
    stats.regionsTotal = totals.regionCount;

    std::error_code dirError;
    std::filesystem::create_directories(sessionDir, dirError);
    if (dirError) {
        outStats = stats;
        return DiskScanOutcome::WriteFailed;
    }

    std::filesystem::path manifestPath = sessionDir / "scan-manifest.json";
    std::filesystem::path baselineTmpPath = sessionDir / "baseline.tmp";
    std::filesystem::path baselinePath = sessionDir / "baseline.bin";

    ScanManifest manifest;
    manifest.identity = identity;
    manifest.alignment = valueSize;
    manifest.regions.reserve(targetRegions.size());
    for (const ProcessMemoryRegion& region : targetRegions) {
        manifest.regions.push_back(ScanManifestRegion{region.baseAddress, region.sizeBytes, region.kind});
    }
    manifest.totalScopeBytes = totals.totalScopeBytes;
    manifest.totalValueCount = totals.comparableValueCount;
    manifest.memoryBudgetBytes = memoryBudgetBytes;
    manifest.generation = 0;
    manifest.state = ScanManifestState::WritingBaseline;
    manifest.startMonotonicUs = NowMonotonicUs();

    auto Fail = [&](DiskScanOutcome outcome, ScanManifestState manifestState, const std::string& reason) -> DiskScanOutcome {
        manifest.state = manifestState;
        manifest.failureReason = reason;
        WriteScanManifest(manifestPath, manifest);
        outStats = stats;
        return outcome;
    };

    if (!WriteScanManifest(manifestPath, manifest)) {
        outStats = stats;
        return DiskScanOutcome::WriteFailed;
    }

    BaselineWriter writer(baselineTmpPath, type, totals.totalScopeBytes, totals.comparableValueCount);
    CandidateStorageResult openResult = writer.Open();
    if (openResult != CandidateStorageResult::Success) {
        return Fail(DiskScanOutcome::WriteFailed, ScanManifestState::Failed,
                    "baseline open failed: " + std::string(ToString(openResult)));
    }

    std::vector<std::uint8_t> chunkBuffer;
    std::vector<std::uint8_t> valueOutputBuffer;
    std::size_t overlap = valueSize - 1;
    std::size_t regionsProcessed = 0;

    for (const ProcessMemoryRegion& region : targetRegions) {
        std::uint64_t regionValueCount = ComputeRegionAlignedValueCount(region, valueSize);
        CandidateStorageResult headerResult = writer.WriteRegionHeader(region, regionValueCount);
        if (headerResult != CandidateStorageResult::Success) {
            return Fail(DiskScanOutcome::WriteFailed, ScanManifestState::Failed,
                        "baseline region header write failed: " + std::string(ToString(headerResult)));
        }

        std::size_t regionOffset = 0;
        while (regionOffset < region.sizeBytes) {
            std::size_t remaining = region.sizeBytes - regionOffset;
            std::size_t ownedChunkSize = std::min(kCandidateScanChunkBytes, remaining);
            std::size_t readSize = std::min(ownedChunkSize + overlap, remaining);

            std::uintptr_t chunkAddress = region.baseAddress + regionOffset;
            if (chunkAddress < region.baseAddress) {
                return Fail(DiskScanOutcome::AddressOverflow, ScanManifestState::Failed, "chunk address overflow");
            }
            std::uintptr_t chunkEnd = chunkAddress + readSize;
            if (chunkEnd < chunkAddress) {
                return Fail(DiskScanOutcome::AddressOverflow, ScanManifestState::Failed, "chunk end address overflow");
            }

            chunkBuffer.assign(readSize, 0);
            ProcessReaderResult readResult = reader.ReadBytes(chunkAddress, chunkBuffer.data(), readSize);
            if (readResult != ProcessReaderResult::Success) {
                DiskScanOutcome outcome = MapReaderFailure(readResult);
                ScanManifestState failState =
                    (outcome == DiskScanOutcome::NotAttached || outcome == DiskScanOutcome::ProcessExited)
                        ? ScanManifestState::Interrupted
                        : ScanManifestState::Failed;
                return Fail(outcome, failState, "baseline chunk read failed: " + std::string(ToString(readResult)));
            }

            std::size_t chunkAddrRemainder = chunkAddress % valueSize;
            std::size_t chunkAlignedStart = (chunkAddrRemainder == 0) ? 0 : (valueSize - chunkAddrRemainder);

            valueOutputBuffer.clear();
            for (std::size_t i = chunkAlignedStart; i + valueSize <= readSize && i < ownedChunkSize; i += valueSize) {
                valueOutputBuffer.insert(valueOutputBuffer.end(), chunkBuffer.begin() + static_cast<std::ptrdiff_t>(i),
                                          chunkBuffer.begin() + static_cast<std::ptrdiff_t>(i + valueSize));
            }
            if (!valueOutputBuffer.empty()) {
                CandidateStorageResult writeResult = writer.WriteRegionValues(valueOutputBuffer.data(), valueOutputBuffer.size());
                if (writeResult != CandidateStorageResult::Success) {
                    return Fail(DiskScanOutcome::WriteFailed, ScanManifestState::Failed,
                                "baseline value write failed: " + std::string(ToString(writeResult)));
                }
            }

            stats.processedBytes += ownedChunkSize;
            stats.peakBufferedBytes =
                std::max(stats.peakBufferedBytes, chunkBuffer.capacity() + valueOutputBuffer.capacity());

            regionOffset += ownedChunkSize;
        }

        ++regionsProcessed;
        stats.regionsProcessed = regionsProcessed;
        stats.coveragePercent =
            totals.totalScopeBytes > 0 ? (100.0 * static_cast<double>(stats.processedBytes) / static_cast<double>(totals.totalScopeBytes))
                                        : 100.0;
        if (onProgress) {
            onProgress(stats);
        }

        // Bounded by region COUNT, not scope size -- cheap enough to persist
        // after every region so an interrupted run leaves a manifest with
        // real (if approximate) progress, not just "WritingBaseline, 0%".
        manifest.processedBytes = stats.processedBytes;
        manifest.coveragePercent = stats.coveragePercent;
        WriteScanManifest(manifestPath, manifest);
    }

    CandidateStorageResult finishResult = writer.Finish();
    if (finishResult != CandidateStorageResult::Success) {
        return Fail(finishResult == CandidateStorageResult::FlushFailed ? DiskScanOutcome::FlushFailed : DiskScanOutcome::WriteFailed,
                    ScanManifestState::Failed, "baseline finish failed: " + std::string(ToString(finishResult)));
    }

    // Self-verify before publish -- never trust the writer's own bookkeeping alone.
    {
        BaselineReader verifyReader(baselineTmpPath);
        CandidateStorageResult verifyResult = verifyReader.Open(totals.comparableValueCount, type);
        if (verifyResult != CandidateStorageResult::Success) {
            return Fail(DiskScanOutcome::CorruptFile, ScanManifestState::Failed,
                        "baseline self-verification failed: " + std::string(ToString(verifyResult)));
        }
    }

    std::error_code renameError;
    std::filesystem::rename(baselineTmpPath, baselinePath, renameError);
    if (renameError) {
        return Fail(DiskScanOutcome::WriteFailed, ScanManifestState::Failed, "baseline publish (rename) failed");
    }

    manifest.state = ScanManifestState::BaselineComplete;
    manifest.processedBytes = totals.totalScopeBytes;
    manifest.coveragePercent = 100.0;
    manifest.completeMonotonicUs = NowMonotonicUs();
    manifest.completedNormally = true;
    manifest.failureReason.clear();
    // The data file is already published and valid at this point; a
    // manifest write failure here is the one self-healing gap the
    // crash-safety design documents (see ResumeDiskCandidateSession) --
    // the next resume rewrites the manifest to match the validated file
    // rather than losing the completed baseline.
    WriteScanManifest(manifestPath, manifest);

    stats.processedBytes = totals.totalScopeBytes;
    stats.coveragePercent = 100.0;
    stats.regionsProcessed = totals.regionCount;
    outStats = stats;
    return DiskScanOutcome::CompleteCoverage;
}

// =====================================================================
// FilterDiskCandidates
// =====================================================================

namespace {

struct FilterPassResult {
    DiskScanOutcome outcome = DiskScanOutcome::ReadFailed;
    std::uint64_t survivorCount = 0;
    std::uint64_t droppedCount = 0;
    std::uint64_t processedCount = 0;
};

/// The first filter after a fresh baseline: baseline.bin is dense (every
/// aligned address in every region has a value), so this walks it region
/// by region, chunk by chunk, coalescing one live-process read per chunk
/// -- never one ReadBytes() call per candidate.
FilterPassResult RunDenseFirstFilter(IProcessReader& reader, BaselineReader& baselineReader, GenerationWriter& genWriter,
                                      CandidateValueType type, std::size_t valueSize, CandidateFilterKind kind,
                                      const CandidateValue* exactTarget, DiskScanStats& stats) {
    FilterPassResult result;
    std::size_t valuesPerChunk = std::max<std::size_t>(1, kCandidateScanChunkBytes / valueSize);
    std::vector<std::uint8_t> baselineBuf(valuesPerChunk * valueSize);
    std::vector<std::uint8_t> liveBuf(valuesPerChunk * valueSize);
    std::vector<std::uint8_t> singleBuf(valueSize);

    ProcessMemoryRegion region;
    std::uint64_t regionValueCount = 0;
    while (baselineReader.NextRegion(region, regionValueCount)) {
        std::size_t addressRemainder = region.baseAddress % valueSize;
        std::size_t alignedStart = (addressRemainder == 0) ? 0 : (valueSize - addressRemainder);

        std::uint64_t valueIndex = 0;
        while (valueIndex < regionValueCount) {
            std::size_t wantValues =
                static_cast<std::size_t>(std::min<std::uint64_t>(valuesPerChunk, regionValueCount - valueIndex));
            std::size_t got = baselineReader.ReadRegionValueChunk(valueIndex, wantValues, baselineBuf.data());
            if (got == 0) {
                result.outcome = DiskScanOutcome::CorruptFile;
                return result;
            }

            std::uintptr_t chunkStartAddress = region.baseAddress + alignedStart + valueIndex * valueSize;
            std::size_t chunkByteLength = got * valueSize;

            ProcessReaderResult readResult = reader.ReadBytes(chunkStartAddress, liveBuf.data(), chunkByteLength);
            if (readResult == ProcessReaderResult::Success) {
                for (std::size_t i = 0; i < got; ++i) {
                    CandidateValue prevVal = DecodeCandidateValue(type, &baselineBuf[i * valueSize]);
                    CandidateValue curVal = DecodeCandidateValue(type, &liveBuf[i * valueSize]);
                    if (EvaluateCandidateFilter(kind, prevVal, curVal, exactTarget)) {
                        std::uintptr_t address = chunkStartAddress + i * valueSize;
                        if (genWriter.WriteRecord(address, &liveBuf[i * valueSize]) != CandidateStorageResult::Success) {
                            result.outcome = DiskScanOutcome::WriteFailed;
                            return result;
                        }
                        ++result.survivorCount;
                    }
                }
            } else if (readResult == ProcessReaderResult::NotAttached || readResult == ProcessReaderResult::ProcessExited) {
                result.outcome =
                    readResult == ProcessReaderResult::NotAttached ? DiskScanOutcome::NotAttached : DiskScanOutcome::ProcessExited;
                return result;
            } else {
                // The whole chunk's coalesced read failed -- fall back to
                // one address at a time within just this chunk, matching
                // the in-memory FilterCandidates' per-address drop policy.
                for (std::size_t i = 0; i < got; ++i) {
                    std::uintptr_t address = chunkStartAddress + i * valueSize;
                    ProcessReaderResult singleResult = reader.ReadBytes(address, singleBuf.data(), valueSize);
                    if (singleResult == ProcessReaderResult::NotAttached || singleResult == ProcessReaderResult::ProcessExited) {
                        result.outcome = singleResult == ProcessReaderResult::NotAttached ? DiskScanOutcome::NotAttached
                                                                                            : DiskScanOutcome::ProcessExited;
                        return result;
                    }
                    if (singleResult != ProcessReaderResult::Success) {
                        ++result.droppedCount;
                        continue;
                    }
                    CandidateValue prevVal = DecodeCandidateValue(type, &baselineBuf[i * valueSize]);
                    CandidateValue curVal = DecodeCandidateValue(type, singleBuf.data());
                    if (EvaluateCandidateFilter(kind, prevVal, curVal, exactTarget)) {
                        if (genWriter.WriteRecord(address, singleBuf.data()) != CandidateStorageResult::Success) {
                            result.outcome = DiskScanOutcome::WriteFailed;
                            return result;
                        }
                        ++result.survivorCount;
                    }
                }
            }

            stats.peakBufferedBytes = std::max(stats.peakBufferedBytes, baselineBuf.capacity() + liveBuf.capacity());
            result.processedCount += got;
            valueIndex += got;
        }
    }

    result.outcome = DiskScanOutcome::CompleteCoverage;
    return result;
}

/// A later filter over a sparse, address-sorted generation file: groups
/// consecutive survivors into address windows of at most
/// kGenerationFilterCoalesceWindowBytes and issues one coalesced read per
/// window -- worst case (scattered survivors) degrades to one read per
/// candidate, never worse than the in-memory FilterCandidates().
FilterPassResult RunSparseGenerationFilter(IProcessReader& reader, GenerationReader& priorReader, GenerationWriter& genWriter,
                                            CandidateValueType type, std::size_t valueSize, CandidateFilterKind kind,
                                            const CandidateValue* exactTarget, DiskScanStats& stats) {
    FilterPassResult result;

    struct PendingRecord {
        std::uint64_t address;
        std::vector<std::uint8_t> value;
    };
    std::vector<PendingRecord> window;
    std::uint64_t windowStart = 0;
    bool aborted = false;

    auto FlushWindow = [&]() {
        if (window.empty()) {
            return;
        }
        std::uint64_t windowEndExclusive = window.back().address + valueSize;
        std::size_t windowLength = static_cast<std::size_t>(windowEndExclusive - windowStart);
        std::vector<std::uint8_t> buf(windowLength);

        ProcessReaderResult readResult = reader.ReadBytes(windowStart, buf.data(), windowLength);
        stats.peakBufferedBytes = std::max(stats.peakBufferedBytes, buf.capacity());

        if (readResult == ProcessReaderResult::Success) {
            for (const PendingRecord& record : window) {
                std::size_t offset = static_cast<std::size_t>(record.address - windowStart);
                CandidateValue prevVal = DecodeCandidateValue(type, record.value.data());
                CandidateValue curVal = DecodeCandidateValue(type, &buf[offset]);
                if (EvaluateCandidateFilter(kind, prevVal, curVal, exactTarget)) {
                    if (genWriter.WriteRecord(record.address, &buf[offset]) != CandidateStorageResult::Success) {
                        result.outcome = DiskScanOutcome::WriteFailed;
                        aborted = true;
                        return;
                    }
                    ++result.survivorCount;
                }
            }
        } else if (readResult == ProcessReaderResult::NotAttached || readResult == ProcessReaderResult::ProcessExited) {
            result.outcome =
                readResult == ProcessReaderResult::NotAttached ? DiskScanOutcome::NotAttached : DiskScanOutcome::ProcessExited;
            aborted = true;
            return;
        } else {
            for (const PendingRecord& record : window) {
                std::vector<std::uint8_t> singleBuf(valueSize);
                ProcessReaderResult singleResult = reader.ReadBytes(record.address, singleBuf.data(), valueSize);
                if (singleResult == ProcessReaderResult::NotAttached || singleResult == ProcessReaderResult::ProcessExited) {
                    result.outcome = singleResult == ProcessReaderResult::NotAttached ? DiskScanOutcome::NotAttached
                                                                                        : DiskScanOutcome::ProcessExited;
                    aborted = true;
                    return;
                }
                if (singleResult != ProcessReaderResult::Success) {
                    ++result.droppedCount;
                    continue;
                }
                CandidateValue prevVal = DecodeCandidateValue(type, record.value.data());
                CandidateValue curVal = DecodeCandidateValue(type, singleBuf.data());
                if (EvaluateCandidateFilter(kind, prevVal, curVal, exactTarget)) {
                    if (genWriter.WriteRecord(record.address, singleBuf.data()) != CandidateStorageResult::Success) {
                        result.outcome = DiskScanOutcome::WriteFailed;
                        aborted = true;
                        return;
                    }
                    ++result.survivorCount;
                }
            }
        }

        result.processedCount += window.size();
        window.clear();
    };

    std::uint64_t address = 0;
    std::vector<std::uint8_t> valueBuf(valueSize);
    while (!aborted && priorReader.NextRecord(address, valueBuf.data())) {
        if (!window.empty() && (address - windowStart) >= kGenerationFilterCoalesceWindowBytes) {
            FlushWindow();
            if (aborted) {
                return result;
            }
        }
        if (window.empty()) {
            windowStart = address;
        }
        window.push_back(PendingRecord{address, valueBuf});
    }
    if (!aborted) {
        FlushWindow();
    }
    if (aborted) {
        return result;
    }

    result.outcome = DiskScanOutcome::CompleteCoverage;
    return result;
}

} // namespace

DiskScanOutcome FilterDiskCandidates(IProcessReader& reader, const std::filesystem::path& sessionDir,
                                      CandidateFilterKind kind, const CandidateValue* exactTarget,
                                      std::size_t memoryBudgetBytes, DiskScanStats& outStats) {
    DiskScanStats stats;
    stats.configuredMemoryBudgetBytes = memoryBudgetBytes;

    if (kind == CandidateFilterKind::ExactValue && exactTarget == nullptr) {
        outStats = stats;
        return DiskScanOutcome::InvalidTarget;
    }

    std::filesystem::path manifestPath = sessionDir / "scan-manifest.json";
    ScanManifest manifest;
    std::string readError;
    if (!ReadScanManifest(manifestPath, manifest, readError)) {
        outStats = stats;
        return DiskScanOutcome::CorruptFile;
    }

    CandidateValueType type;
    if (!ParseCandidateValueType(manifest.identity.valueType, type)) {
        outStats = stats;
        return DiskScanOutcome::CorruptFile;
    }
    std::size_t valueSize = CandidateValueTypeSize(type);

    bool isFirstFilter = (manifest.generation == 0);
    if (isFirstFilter && kind == CandidateFilterKind::Unchanged) {
        outStats = stats;
        return DiskScanOutcome::InitialFilterTooBroad;
    }
    if (isFirstFilter && manifest.state != ScanManifestState::BaselineComplete) {
        outStats = stats;
        return DiskScanOutcome::CorruptFile;
    }
    if (!isFirstFilter && manifest.state != ScanManifestState::CandidatesComplete) {
        outStats = stats;
        return DiskScanOutcome::CorruptFile;
    }

    int newGeneration = manifest.generation + 1;
    std::filesystem::path newGenTmpPath = sessionDir / (GenerationFileBaseName(newGeneration) + ".tmp");
    std::filesystem::path newGenBinPath = sessionDir / (GenerationFileBaseName(newGeneration) + ".bin");

    GenerationWriter genWriter(newGenTmpPath, type, newGeneration);
    CandidateStorageResult openResult = genWriter.Open();
    if (openResult != CandidateStorageResult::Success) {
        outStats = stats;
        return DiskScanOutcome::WriteFailed;
    }

    FilterPassResult passResult;
    if (isFirstFilter) {
        BaselineReader baselineReader(sessionDir / "baseline.bin");
        CandidateStorageResult baselineOpenResult = baselineReader.Open(manifest.totalValueCount, type);
        if (baselineOpenResult != CandidateStorageResult::Success) {
            outStats = stats;
            return DiskScanOutcome::CorruptFile;
        }
        passResult = RunDenseFirstFilter(reader, baselineReader, genWriter, type, valueSize, kind, exactTarget, stats);
    } else {
        std::filesystem::path priorGenPath = sessionDir / (GenerationFileBaseName(manifest.generation) + ".bin");
        GenerationReader priorReader(priorGenPath);
        CandidateStorageResult priorOpenResult = priorReader.Open(manifest.candidateCount, type, manifest.generation);
        if (priorOpenResult != CandidateStorageResult::Success) {
            outStats = stats;
            return DiskScanOutcome::CorruptFile;
        }
        passResult = RunSparseGenerationFilter(reader, priorReader, genWriter, type, valueSize, kind, exactTarget, stats);
    }

    if (passResult.outcome != DiskScanOutcome::CompleteCoverage) {
        // Filter aborted -- the previous complete generation/baseline is
        // left untouched; the new .tmp is never published or treated as
        // complete.
        ScanManifestState failState = (passResult.outcome == DiskScanOutcome::NotAttached ||
                                        passResult.outcome == DiskScanOutcome::ProcessExited)
                                           ? ScanManifestState::Interrupted
                                           : ScanManifestState::Failed;
        manifest.state = failState;
        manifest.failureReason = "filter pass failed: " + std::string(ToString(passResult.outcome));
        WriteScanManifest(manifestPath, manifest);
        stats.survivingCandidateCount = passResult.survivorCount;
        stats.droppedCandidateCount = passResult.droppedCount;
        outStats = stats;
        return passResult.outcome;
    }

    CandidateStorageResult finishResult = genWriter.Finish();
    if (finishResult != CandidateStorageResult::Success) {
        manifest.state = ScanManifestState::Failed;
        manifest.failureReason = "generation finish failed: " + std::string(ToString(finishResult));
        WriteScanManifest(manifestPath, manifest);
        outStats = stats;
        return finishResult == CandidateStorageResult::FlushFailed ? DiskScanOutcome::FlushFailed : DiskScanOutcome::WriteFailed;
    }

    // Self-verify before publish -- never trust the writer's own bookkeeping alone.
    {
        GenerationReader verifyReader(newGenTmpPath);
        CandidateStorageResult verifyResult = verifyReader.Open(passResult.survivorCount, type, newGeneration);
        if (verifyResult != CandidateStorageResult::Success) {
            manifest.state = ScanManifestState::Failed;
            manifest.failureReason = "generation self-verification failed: " + std::string(ToString(verifyResult));
            WriteScanManifest(manifestPath, manifest);
            outStats = stats;
            return DiskScanOutcome::CorruptFile;
        }
    }

    std::error_code renameError;
    std::filesystem::rename(newGenTmpPath, newGenBinPath, renameError);
    if (renameError) {
        manifest.state = ScanManifestState::Failed;
        manifest.failureReason = "generation publish (rename) failed";
        WriteScanManifest(manifestPath, manifest);
        outStats = stats;
        return DiskScanOutcome::WriteFailed;
    }

    manifest.generation = newGeneration;
    manifest.candidateCount = passResult.survivorCount;
    manifest.state = ScanManifestState::CandidatesComplete;
    manifest.processedBytes = passResult.processedCount * valueSize;
    manifest.coveragePercent = 100.0;
    manifest.completeMonotonicUs = NowMonotonicUs();
    manifest.completedNormally = true;
    manifest.failureReason.clear();
    WriteScanManifest(manifestPath, manifest);

    stats.survivingCandidateCount = passResult.survivorCount;
    stats.droppedCandidateCount = passResult.droppedCount;
    if (passResult.droppedCount > 0) {
        stats.droppedCandidateReasonSummary =
            std::to_string(passResult.droppedCount) + " candidate(s) dropped due to per-address read failure";
    }
    stats.processedBytes = passResult.processedCount * valueSize;
    stats.coveragePercent = 100.0;
    outStats = stats;
    return DiskScanOutcome::CompleteCoverage;
}

// =====================================================================
// ResumeDiskCandidateSession
// =====================================================================

DiskScanOutcome ResumeDiskCandidateSession(const std::filesystem::path& sessionDir, const ScanSessionIdentity& currentIdentity,
                                            IProcessReader& reader, ScanManifest& outManifest) {
    std::filesystem::path manifestPath = sessionDir / "scan-manifest.json";
    ScanManifest manifest;
    std::string error;
    if (!ReadScanManifest(manifestPath, manifest, error)) {
        return DiskScanOutcome::CorruptFile;
    }

    if (manifest.identity != currentIdentity) {
        return DiskScanOutcome::SessionMismatch;
    }

    if (!reader.IsAlive()) {
        return DiskScanOutcome::ProcessExited;
    }

    CandidateValueType type;
    if (!ParseCandidateValueType(manifest.identity.valueType, type)) {
        return DiskScanOutcome::CorruptFile;
    }

    if (manifest.generation == 0) {
        if (manifest.state != ScanManifestState::BaselineComplete) {
            return DiskScanOutcome::CorruptFile;
        }
        BaselineReader baselineReader(sessionDir / "baseline.bin");
        if (baselineReader.Open(manifest.totalValueCount, type) != CandidateStorageResult::Success) {
            return DiskScanOutcome::CorruptFile;
        }
    } else {
        if (manifest.state != ScanManifestState::CandidatesComplete) {
            return DiskScanOutcome::CorruptFile;
        }
        GenerationReader genReader(sessionDir / (GenerationFileBaseName(manifest.generation) + ".bin"));
        if (genReader.Open(manifest.candidateCount, type, manifest.generation) != CandidateStorageResult::Success) {
            return DiskScanOutcome::CorruptFile;
        }
    }

    outManifest = manifest;
    return DiskScanOutcome::CompleteCoverage;
}

} // namespace sekiro_haptics::process
