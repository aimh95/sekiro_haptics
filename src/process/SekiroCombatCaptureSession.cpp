#include "sekiro_haptics/process/SekiroCombatCaptureSession.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace sekiro_haptics::process {

namespace {

constexpr int kCaptureSchemaVersion = 1;

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
                out << c;
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

} // namespace

const char* ToString(CombatCaptureScope scope) {
    switch (scope) {
        case CombatCaptureScope::PlayerGameData:
            return "PlayerGameData";
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
        case CombatCaptureStartResult::InitialReadFailed:
            return "InitialReadFailed";
    }
    return "Unknown";
}

SekiroCombatCaptureSession::SekiroCombatCaptureSession(IProcessReader& reader) : reader_(reader) {}

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
    if (windowSize == 0 || regionBaseAddress == 0) {
        return CombatCaptureStartResult::InvalidConfig;
    }

    std::vector<std::uint8_t> baseline(windowSize);
    ProcessReaderResult readResult = reader_.ReadBytes(regionBaseAddress, baseline.data(), windowSize);
    if (readResult != ProcessReaderResult::Success) {
        return CombatCaptureStartResult::InitialReadFailed;
    }

    stream_.open(outputPath, std::ios::trunc);
    if (!stream_.is_open()) {
        return CombatCaptureStartResult::OpenOutputFailed;
    }

    windowSizeBytes_ = windowSize;
    samplingInterval_ = ClampInterval(config.samplingInterval);
    lastGeneration_ = regionGeneration;
    previousBytes_ = std::move(baseline);
    lastSampleMonotonicUs_ = nowMonotonicUs;
    hasLastSample_ = true;
    stats_ = CombatCaptureStats{};
    running_ = true;
    return CombatCaptureStartResult::Started;
}

bool SekiroCombatCaptureSession::Tick(std::uintptr_t regionBaseAddress, std::uint64_t regionGeneration,
                                       std::int64_t nowMonotonicUs) {
    if (!running_) {
        return false;
    }

    if (hasLastSample_) {
        std::int64_t elapsedMs = (nowMonotonicUs - lastSampleMonotonicUs_) / 1000;
        if (elapsedMs < samplingInterval_.count()) {
            return true; // not due yet -- silent no-op, not counted anywhere
        }
        if (elapsedMs > samplingInterval_.count() * 2) {
            ++stats_.lateSamples;
        }
    }
    lastSampleMonotonicUs_ = nowMonotonicUs;
    hasLastSample_ = true;
    ++stats_.samplesTaken;

    if (regionBaseAddress == 0) {
        ++stats_.droppedSamples;
        WriteDroppedRecord(nowMonotonicUs, "not resolved");
        return true;
    }

    if (regionGeneration != lastGeneration_) {
        ++stats_.discontinuities;
        WriteDiscontinuityRecord(nowMonotonicUs, lastGeneration_, regionGeneration);
        lastGeneration_ = regionGeneration;

        // Re-baseline against the new instance -- never diff its bytes
        // against the previous (different) instance's data.
        std::vector<std::uint8_t> fresh(windowSizeBytes_);
        ProcessReaderResult readResult = reader_.ReadBytes(regionBaseAddress, fresh.data(), windowSizeBytes_);
        if (readResult != ProcessReaderResult::Success) {
            ++stats_.droppedSamples;
            WriteDroppedRecord(nowMonotonicUs, "read failed after discontinuity");
            return true;
        }
        previousBytes_ = std::move(fresh);
        return true;
    }

    std::vector<std::uint8_t> current(windowSizeBytes_);
    ProcessReaderResult readResult = reader_.ReadBytes(regionBaseAddress, current.data(), windowSizeBytes_);
    if (readResult != ProcessReaderResult::Success) {
        ++stats_.droppedSamples;
        WriteDroppedRecord(nowMonotonicUs, "read failed");
        return true;
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
            WriteDeltaRecord(nowMonotonicUs, regionGeneration, offset, &previousBytes_[offset], &current[offset]);
            ++stats_.deltaRecordsWritten;
        }
    }
    previousBytes_ = std::move(current);
    return true;
}

bool SekiroCombatCaptureSession::Mark(const std::string& label, std::int64_t nowMonotonicUs) {
    if (!running_) {
        return false;
    }
    WriteMarkerRecord(nowMonotonicUs, label);
    ++stats_.markersWritten;
    return static_cast<bool>(stream_);
}

void SekiroCombatCaptureSession::Stop() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    running_ = false;
}

void SekiroCombatCaptureSession::WriteDeltaRecord(std::int64_t timestampUs, std::uint64_t generation,
                                                    std::size_t offset, const std::uint8_t* previousCell,
                                                    const std::uint8_t* currentCell) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
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

void SekiroCombatCaptureSession::WriteMarkerRecord(std::int64_t timestampUs, const std::string& label) {
    stream_ << "{\"schemaVersion\":" << kCaptureSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
    stream_ << ",\"recordKind\":\"marker\"";
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
    stream_ << ",\"reason\":";
    WriteJsonString(stream_, reason);
    stream_ << "}\n";
}

} // namespace sekiro_haptics::process
