#include "sekiro_haptics/process/SekiroCombatCaptureSession.hpp"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace sekiro_haptics::process {

namespace {

constexpr int kCaptureSchemaVersion = 3;

// Use one exclusive-create operation; an exists() + truncating open races
// other writers and can destroy an existing capture or symlink target.
std::FILE* OpenExclusive(const std::string& path) {
#ifdef _WIN32
    int fd = -1;
    const auto error = _wsopen_s(&fd, std::filesystem::path(path).c_str(),
                                _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                _SH_DENYWR, _S_IREAD | _S_IWRITE);
    if (error != 0) { errno = error; return nullptr; }
    auto* file = _fdopen(fd, "wb");
    if (!file) _close(fd);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return nullptr;
    auto* file = ::fdopen(fd, "wb");
    if (!file) ::close(fd);
#endif
    return file;
}

void WriteJsonString(std::ostream& out, const std::string& text) {
    out << '"';
    for (char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    const auto value = static_cast<unsigned char>(c);
                    out << "\\u00" << hex[value >> 4] << hex[value & 15];
                } else {
                    out << c;
                }
                break;
        }
    }
    out << '"';
}

std::string ToHexBytes(const std::uint8_t* bytes, std::size_t length) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return oss.str();
}

std::size_t ClampWindowSizeBytes(CombatCaptureScope scope, std::size_t requested) {
    std::size_t scopeMax = 0;
    switch (scope) {
        case CombatCaptureScope::PlayerGameData:
            scopeMax = kPlayerGameDataMaxCaptureBytes;
            break;
        case CombatCaptureScope::CustomAddress:
            scopeMax = kCustomAddressMaxCaptureBytes;
            break;
    }
    std::size_t clamped = std::min(requested, scopeMax);
    clamped -= clamped % kCombatCaptureCellSizeBytes;
    return clamped;
}

std::chrono::milliseconds ClampInterval(std::chrono::milliseconds interval) {
    if (interval < kMinCombatCaptureIntervalMs) {
        return kMinCombatCaptureIntervalMs;
    }
    if (interval > kMaxCombatCaptureIntervalMs) {
        return kMaxCombatCaptureIntervalMs;
    }
    return interval;
}

std::int64_t Percentile(const std::vector<std::int64_t>& sorted, int pct) {
    if (sorted.empty()) {
        return 0;
    }
    std::size_t idx = std::min(sorted.size() - 1, sorted.size() * static_cast<std::size_t>(pct) / 100);
    return sorted[idx];
}

} // namespace

const char* ToString(CombatCaptureScope scope) {
    switch (scope) {
        case CombatCaptureScope::PlayerGameData:
            return "PlayerGameData";
        case CombatCaptureScope::CustomAddress:
            return "CustomAddress";
    }
    return "Unknown";
}

const char* ToString(CombatCaptureStartResult result) {
    switch (result) {
        case CombatCaptureStartResult::Started:
            return "Started";
        case CombatCaptureStartResult::AlreadyRunning:
            return "AlreadyRunning";
        case CombatCaptureStartResult::InvalidConfig:
            return "InvalidConfig";
        case CombatCaptureStartResult::OpenOutputFailed:
            return "OpenOutputFailed";
        case CombatCaptureStartResult::OutputExists:
            return "OutputExists";
        case CombatCaptureStartResult::WriteOutputFailed:
            return "WriteOutputFailed";
        case CombatCaptureStartResult::InitialReadFailed:
            return "InitialReadFailed";
    }
    return "Unknown";
}

SekiroCombatCaptureSession::SekiroCombatCaptureSession(IProcessReader& reader, ValidateRegion validateRegion)
    : reader_(reader), validateRegion_(std::move(validateRegion)) {}

SekiroCombatCaptureSession::~SekiroCombatCaptureSession() {
    Stop();
}

CombatCaptureStartResult SekiroCombatCaptureSession::Start(const CombatCaptureConfig& config,
                                                             std::uintptr_t regionBaseAddress,
                                                             std::uint64_t regionGeneration,
                                                             const std::string& outputPath,
                                                             std::int64_t nowMonotonicUs) {
    if (running_) {
        return CombatCaptureStartResult::AlreadyRunning;
    }

    std::size_t windowSize = ClampWindowSizeBytes(config.scope, config.requestedWindowSizeBytes);
    if (windowSize == 0 || regionBaseAddress == 0 || nowMonotonicUs < 0 ||
        regionBaseAddress > std::numeric_limits<std::uintptr_t>::max() - windowSize ||
        nowMonotonicUs > std::numeric_limits<std::int64_t>::max() - 2'000'000) {
        return CombatCaptureStartResult::InvalidConfig;
    }

    std::vector<std::uint8_t> baseline(windowSize);
    ProcessReaderResult readResult = reader_.ReadBytes(regionBaseAddress, baseline.data(), windowSize);
    if (readResult != ProcessReaderResult::Success) {
        return CombatCaptureStartResult::InitialReadFailed;
    }
    if (validateRegion_ && !validateRegion_(regionBaseAddress, regionGeneration)) {
        return CombatCaptureStartResult::InitialReadFailed;
    }

    Stop();
    file_ = OpenExclusive(outputPath);
    if (!file_) {
        return errno == EEXIST ? CombatCaptureStartResult::OutputExists : CombatCaptureStartResult::OpenOutputFailed;
    }
    stream_.str("");
    stream_.clear();

    windowSizeBytes_ = windowSize;
    samplingInterval_ = ClampInterval(config.samplingInterval);
    lateToleranceUs_ = config.lateToleranceUs >= 0 ? config.lateToleranceUs : samplingInterval_.count() * 1000;
    lastGeneration_ = regionGeneration;
    lastBaseAddress_ = regionBaseAddress;
    previousBytes_ = std::move(baseline);
    baselineValid_ = true;
    nextSequence_ = 0;
    nextScheduledUs_ = nowMonotonicUs + samplingInterval_.count() * 1000;
    lastAcceptedActualUs_ = nowMonotonicUs;
    hasLastSample_ = false;
    stats_ = CombatCaptureStats{};
    sampleIntervalsUs_.clear();
    sampleLatenessUs_.clear();
    running_ = true;
    stream_ << "{\"schemaVersion\":3,\"recordKind\":\"capture_start\",\"timestampUs\":" << nowMonotonicUs
            << ",\"scope\":\"" << ToString(config.scope) << "\",\"windowSizeBytes\":" << windowSizeBytes_
            << ",\"intervalUs\":" << samplingInterval_.count() * 1000
            << ",\"interpretation\":\"unvalidated_raw_memory\"}\n";
    WriteBaselineRecord(nowMonotonicUs, "initial");
    return FlushRecords() ? CombatCaptureStartResult::Started : CombatCaptureStartResult::WriteOutputFailed;
}

bool SekiroCombatCaptureSession::Tick(std::uintptr_t regionBaseAddress, std::uint64_t regionGeneration,
                                       std::int64_t nowMonotonicUs) {
    if (!running_) {
        return false;
    }

    if (nowMonotonicUs < nextScheduledUs_) {
        return true; // not due yet -- silent no-op, not counted anywhere
    }

    std::int64_t intervalUs = samplingInterval_.count() * 1000;
    std::int64_t scheduledUs = nextScheduledUs_;
    std::int64_t latenessUs = nowMonotonicUs - scheduledUs;
    std::int64_t missedCycles = latenessUs / intervalUs; // >= 0
    if (nowMonotonicUs > std::numeric_limits<std::int64_t>::max() - intervalUs) {
        baselineValid_ = false;
        Stop();
        return false;
    }

    // Advance the grid past every slot we're about to skip -- landing on
    // the next slot strictly after `now` -- rather than resetting relative
    // to `now` itself, which would let drift accumulate and would let a
    // caller "catch up" by bursting reads. Skipped slots are counted, not
    // replayed.
    nextScheduledUs_ = scheduledUs + intervalUs * (missedCycles + 1);

    if (latenessUs >= lateToleranceUs_) {
        ++stats_.lateSamples;
    }
    if (missedCycles > 0) {
        stats_.missedScheduleSamples += static_cast<std::uint64_t>(missedCycles);
        stats_.droppedSamplesTotal += static_cast<std::uint64_t>(missedCycles);
        baselineValid_ = false;
        stream_ << "{\"schemaVersion\":3,\"recordKind\":\"gap\",\"timestampUs\":" << nowMonotonicUs
                << ",\"fromTimestampUs\":" << scheduledUs << ",\"missedCycles\":" << missedCycles << "}\n";
    }

    sampleLatenessUs_.push_back(latenessUs);
    if (hasLastSample_) {
        sampleIntervalsUs_.push_back(nowMonotonicUs - lastAcceptedActualUs_);
    }
    lastAcceptedActualUs_ = nowMonotonicUs;
    hasLastSample_ = true;
    std::uint64_t sequence = nextSequence_++;
    ++stats_.samplesTaken;

    if (regionBaseAddress == 0) {
        ++stats_.unresolvedSamples;
        ++stats_.droppedSamplesTotal;
        baselineValid_ = false;
        previousBytes_.clear();
        WriteDroppedRecord(nowMonotonicUs, "not resolved");
        return FlushRecords();
    }

    if (regionGeneration != lastGeneration_ || regionBaseAddress != lastBaseAddress_) {
        ++stats_.discontinuities;
        WriteDiscontinuityRecord(nowMonotonicUs, lastGeneration_, regionGeneration);
        lastGeneration_ = regionGeneration;
        lastBaseAddress_ = regionBaseAddress;
        baselineValid_ = false;
        previousBytes_.clear(); // Clear BEFORE a possibly failed first read of the new owner.
    }

    std::vector<std::uint8_t> current(windowSizeBytes_);
    ProcessReaderResult readResult = ProcessReaderResult::ReadFailed;
    if (regionBaseAddress <= std::numeric_limits<std::uintptr_t>::max() - windowSizeBytes_) {
        readResult = reader_.ReadBytes(regionBaseAddress, current.data(), windowSizeBytes_);
    }
    if (readResult != ProcessReaderResult::Success) {
        ++stats_.readFailedSamples;
        ++stats_.droppedSamplesTotal;
        baselineValid_ = false;
        previousBytes_.clear();
        WriteDroppedRecord(nowMonotonicUs, std::string("read failed: ") + ToString(readResult));
        return FlushRecords();
    }
    if (validateRegion_ && !validateRegion_(regionBaseAddress, regionGeneration)) {
        ++stats_.inconsistentSamples;
        ++stats_.droppedSamplesTotal;
        baselineValid_ = false;
        previousBytes_.clear();
        WriteDroppedRecord(nowMonotonicUs, "object changed during read");
        return FlushRecords();
    }
    if (!baselineValid_) {
        previousBytes_ = std::move(current);
        baselineValid_ = true;
        WriteBaselineRecord(nowMonotonicUs, "rebaseline");
        WriteSampleRecord(sequence, scheduledUs, nowMonotonicUs, "baseline");
        return FlushRecords();
    }

    for (std::size_t offset = 0; offset < windowSizeBytes_; offset += kCombatCaptureCellSizeBytes) {
        bool changed = false;
        for (std::size_t i = 0; i < kCombatCaptureCellSizeBytes; ++i) {
            if (previousBytes_[offset + i] != current[offset + i]) {
                changed = true;
                break;
            }
        }
        if (changed) {
            WriteDeltaRecord(sequence, scheduledUs, nowMonotonicUs, regionGeneration, offset, &previousBytes_[offset],
                              &current[offset]);
            ++stats_.deltaRecordsWritten;
        }
    }
    previousBytes_ = std::move(current);
    WriteSampleRecord(sequence, scheduledUs, nowMonotonicUs, "observed");
    return FlushRecords();
}

bool SekiroCombatCaptureSession::Mark(const std::string& label, std::int64_t inputTimestampUs,
                                       std::int64_t processedTimestampUs) {
    if (!running_) {
        ++stats_.markersRejectedNotRunning;
        return false;
    }
    WriteMarkerRecord(inputTimestampUs, processedTimestampUs, label);
    if (!FlushRecords()) return false;
    ++stats_.markersWritten;
    return true;
}

CombatCaptureStats SekiroCombatCaptureSession::Stats() const {
    CombatCaptureStats result = stats_;
    result.configuredIntervalUs = samplingInterval_.count() * 1000;
    result.lateToleranceUs = lateToleranceUs_;

    if (!sampleIntervalsUs_.empty()) {
        std::vector<std::int64_t> sorted = sampleIntervalsUs_;
        std::sort(sorted.begin(), sorted.end());
        result.minIntervalUs = sorted.front();
        result.p50IntervalUs = Percentile(sorted, 50);
        result.p95IntervalUs = Percentile(sorted, 95);
        result.p99IntervalUs = Percentile(sorted, 99);
        result.maxIntervalUs = sorted.back();
    }
    if (!sampleLatenessUs_.empty()) {
        std::vector<std::int64_t> sorted = sampleLatenessUs_;
        std::sort(sorted.begin(), sorted.end());
        result.p50LatenessUs = Percentile(sorted, 50);
        result.p95LatenessUs = Percentile(sorted, 95);
        result.maxLatenessUs = sorted.back();
    }
    return result;
}

bool SekiroCombatCaptureSession::Stop() {
    if (file_) {
        if (running_ && !stats_.outputFailed) {
            stream_ << "{\"schemaVersion\":3,\"recordKind\":\"capture_end\",\"timestampUs\":"
                    << lastAcceptedActualUs_ << ",\"samplesTaken\":" << stats_.samplesTaken << "}\n";
            FlushRecords();
        }
        if (std::fclose(file_) != 0) stats_.outputFailed = true;
        file_ = nullptr;
    }
    running_ = false;
    return !stats_.outputFailed;
}

bool SekiroCombatCaptureSession::FlushRecords() {
    const std::string pending = stream_.str();
    if (!file_ || !stream_ ||
        std::fwrite(pending.data(), 1, pending.size(), file_) != pending.size() || std::fflush(file_) != 0) {
        stats_.outputFailed = true;
        running_ = false;
        baselineValid_ = false;
        return false;
    }
    stream_.str("");
    stream_.clear();
    return true;
}

void SekiroCombatCaptureSession::WriteBaselineRecord(std::int64_t timestampUs, const char* reason) {
    stream_ << "{\"schemaVersion\":3,\"recordKind\":\"baseline\",\"timestampUs\":" << timestampUs
            << ",\"generation\":" << lastGeneration_ << ",\"baseAddressHex\":\"0x"
            << std::hex << lastBaseAddress_ << std::dec << "\",\"reason\":\"" << reason
            << "\",\"bytesHex\":\"" << ToHexBytes(previousBytes_.data(), previousBytes_.size()) << "\"}\n";
    ++stats_.baselineRecordsWritten;
}

void SekiroCombatCaptureSession::WriteSampleRecord(std::uint64_t sequence, std::int64_t scheduledUs,
                                                   std::int64_t actualUs, const char* status) {
    stream_ << "{\"schemaVersion\":3,\"recordKind\":\"sample\",\"timestampUs\":" << actualUs
            << ",\"sequence\":" << sequence << ",\"scheduledTimestampUs\":" << scheduledUs
            << ",\"generation\":" << lastGeneration_ << ",\"status\":\"" << status << "\"}\n";
    ++stats_.sampleRecordsWritten;
}

void SekiroCombatCaptureSession::WriteDeltaRecord(std::uint64_t sequence, std::int64_t scheduledUs,
                                                    std::int64_t actualUs, std::uint64_t generation,
                                                    std::size_t offset, const std::uint8_t* previousCell,
                                                    const std::uint8_t* currentCell) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"sequence\":" << sequence;
    stream_ << ",\"scheduledTimestampUs\":" << scheduledUs;
    stream_ << ",\"timestampUs\":" << actualUs; // "actual" -- kept as timestampUs for analyzer compatibility
    stream_ << ",\"recordKind\":\"delta\"";
    stream_ << ",\"generation\":" << generation;
    stream_ << ",\"offset\":" << offset;
    stream_ << ",\"cellSizeBytes\":" << kCombatCaptureCellSizeBytes;
    stream_ << ",\"previousBytesHex\":";
    WriteJsonString(stream_, ToHexBytes(previousCell, kCombatCaptureCellSizeBytes));
    stream_ << ",\"currentBytesHex\":";
    WriteJsonString(stream_, ToHexBytes(currentCell, kCombatCaptureCellSizeBytes));
    stream_ << "}\n";
}

void SekiroCombatCaptureSession::WriteMarkerRecord(std::int64_t inputTimestampUs, std::int64_t processedTimestampUs,
                                                    const std::string& label) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"timestampUs\":" << inputTimestampUs; // the precise input moment -- see the header comment
    stream_ << ",\"processedTimestampUs\":" << processedTimestampUs;
    stream_ << ",\"recordKind\":\"marker\"";
    stream_ << ",\"source\":\"manual_or_input_annotation\"";
    stream_ << ",\"label\":";
    WriteJsonString(stream_, label);
    stream_ << "}\n";
}

void SekiroCombatCaptureSession::WriteDiscontinuityRecord(std::int64_t timestampUs, std::uint64_t oldGeneration,
                                                            std::uint64_t newGeneration) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
    stream_ << ",\"recordKind\":\"discontinuity\"";
    stream_ << ",\"oldGeneration\":" << oldGeneration;
    stream_ << ",\"newGeneration\":" << newGeneration;
    stream_ << "}\n";
}

void SekiroCombatCaptureSession::WriteDroppedRecord(std::int64_t timestampUs, const std::string& reason) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
    stream_ << ",\"recordKind\":\"dropped\"";
    stream_ << ",\"sequence\":" << nextSequence_ - 1;
    stream_ << ",\"generation\":" << lastGeneration_;
    stream_ << ",\"reason\":";
    WriteJsonString(stream_, reason);
    stream_ << "}\n";
}

} // namespace sekiro_haptics::process
