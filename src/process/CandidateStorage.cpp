#include "sekiro_haptics/process/CandidateStorage.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace sekiro_haptics::process {

namespace {

template <typename T>
void WriteRaw(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadRaw(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool ReadMagic(std::ifstream& in, const char* expected) {
    char buffer[8] = {};
    in.read(buffer, 8);
    if (!in) {
        return false;
    }
    return std::memcmp(buffer, expected, 8) == 0;
}

} // namespace

const char* ToString(CandidateStorageResult result) {
    switch (result) {
        case CandidateStorageResult::Success:
            return "Success";
        case CandidateStorageResult::OpenFailed:
            return "OpenFailed";
        case CandidateStorageResult::WriteFailed:
            return "WriteFailed";
        case CandidateStorageResult::FlushFailed:
            return "FlushFailed";
        case CandidateStorageResult::ReadFailed:
            return "ReadFailed";
        case CandidateStorageResult::CorruptFile:
            return "CorruptFile";
        case CandidateStorageResult::SizeMismatch:
            return "SizeMismatch";
    }
    return "Unknown";
}

// =====================================================================
// BaselineWriter
// =====================================================================

BaselineWriter::BaselineWriter(std::filesystem::path path, CandidateValueType type, std::uint64_t totalScopeBytes,
                                std::uint64_t totalValueCount)
    : path_(std::move(path)),
      type_(type),
      valueSize_(CandidateValueTypeSize(type)),
      totalScopeBytes_(totalScopeBytes),
      totalValueCount_(totalValueCount) {}

BaselineWriter::~BaselineWriter() {
    if (stream_.is_open()) {
        stream_.close();
    }
}

CandidateStorageResult BaselineWriter::Open() {
    stream_.open(path_, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!stream_.is_open()) {
        failed_ = true;
        return CandidateStorageResult::OpenFailed;
    }

    stream_.write(kBaselineHeaderMagic, 8);
    WriteRaw(stream_, static_cast<std::uint32_t>(kScanStorageFormatVersion));
    WriteRaw(stream_, static_cast<std::uint32_t>(type_));
    WriteRaw(stream_, static_cast<std::uint32_t>(valueSize_));
    WriteRaw(stream_, std::uint32_t{0}); // regionCount placeholder, fixed up in Finish()
    WriteRaw(stream_, totalScopeBytes_);
    WriteRaw(stream_, totalValueCount_);
    WriteRaw(stream_, std::uint64_t{0}); // reserved

    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }
    return CandidateStorageResult::Success;
}

CandidateStorageResult BaselineWriter::WriteRegionHeader(const ProcessMemoryRegion& region, std::uint64_t valueCount) {
    if (failed_) {
        return CandidateStorageResult::WriteFailed;
    }
    if (currentRegionValuesRemaining_ != 0) {
        // Previous region wasn't fully written -- fail closed rather than
        // silently producing a directory entry that doesn't match its payload.
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    WriteRaw(stream_, static_cast<std::uint64_t>(region.baseAddress));
    WriteRaw(stream_, static_cast<std::uint64_t>(region.sizeBytes));
    WriteRaw(stream_, static_cast<std::uint32_t>(region.kind));
    WriteRaw(stream_, valueCount);

    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    ++regionCount_;
    currentRegionValuesRemaining_ = valueCount;
    payloadBytesWritten_ += kBaselineRegionEntryBytes;
    return CandidateStorageResult::Success;
}

CandidateStorageResult BaselineWriter::WriteRegionValues(const std::uint8_t* rawValueBytes, std::size_t byteCount) {
    if (failed_) {
        return CandidateStorageResult::WriteFailed;
    }
    if (byteCount % valueSize_ != 0) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    std::uint64_t count = static_cast<std::uint64_t>(byteCount / valueSize_);
    if (count > currentRegionValuesRemaining_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    stream_.write(reinterpret_cast<const char*>(rawValueBytes), static_cast<std::streamsize>(byteCount));
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    currentRegionValuesRemaining_ -= count;
    valuesWritten_ += count;
    payloadBytesWritten_ += byteCount;
    return CandidateStorageResult::Success;
}

CandidateStorageResult BaselineWriter::Finish() {
    if (failed_) {
        return CandidateStorageResult::WriteFailed;
    }
    if (currentRegionValuesRemaining_ != 0 || valuesWritten_ != totalValueCount_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    stream_.write(kBaselineFooterMagic, 8);
    WriteRaw(stream_, valuesWritten_);
    WriteRaw(stream_, payloadBytesWritten_);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    // Fix up the region count now that it's known, then leave the write
    // position wherever it lands -- nothing is written after this.
    stream_.seekp(20, std::ios::beg);
    WriteRaw(stream_, regionCount_);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    stream_.flush();
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::FlushFailed;
    }
    stream_.close();
    return CandidateStorageResult::Success;
}

bool BaselineWriter::IsFailed() const {
    return failed_;
}

// =====================================================================
// BaselineReader
// =====================================================================

BaselineReader::BaselineReader(std::filesystem::path path) : path_(std::move(path)) {}

BaselineReader::~BaselineReader() = default;

CandidateStorageResult BaselineReader::Open(std::uint64_t expectedTotalValueCount, CandidateValueType expectedType) {
    stream_.open(path_, std::ios::binary | std::ios::in);
    if (!stream_.is_open()) {
        failed_ = true;
        return CandidateStorageResult::OpenFailed;
    }

    std::error_code sizeError;
    std::uintmax_t realFileSize = std::filesystem::file_size(path_, sizeError);
    if (sizeError) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }

    if (!ReadMagic(stream_, kBaselineHeaderMagic)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }

    std::uint32_t formatVersion = 0, typeCode = 0, valueSizeBytes = 0, regionCount = 0;
    std::uint64_t totalScopeBytes = 0, totalValueCount = 0, reserved = 0;
    if (!ReadRaw(stream_, formatVersion) || !ReadRaw(stream_, typeCode) || !ReadRaw(stream_, valueSizeBytes) ||
        !ReadRaw(stream_, regionCount) || !ReadRaw(stream_, totalScopeBytes) || !ReadRaw(stream_, totalValueCount) ||
        !ReadRaw(stream_, reserved)) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }

    valueSize_ = CandidateValueTypeSize(expectedType);
    if (typeCode != static_cast<std::uint32_t>(expectedType) || valueSizeBytes != static_cast<std::uint32_t>(valueSize_) ||
        formatVersion != static_cast<std::uint32_t>(kScanStorageFormatVersion)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }
    if (totalValueCount != expectedTotalValueCount) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }
    type_ = expectedType;

    std::streamoff pos = static_cast<std::streamoff>(kBaselineHeaderBytes);
    std::vector<RegionEntry> regions;
    regions.reserve(regionCount);
    std::uint64_t sumValueCounts = 0;

    for (std::uint32_t i = 0; i < regionCount; ++i) {
        std::uint64_t baseAddress = 0, sizeBytes = 0, valueCount = 0;
        std::uint32_t kindCode = 0;
        if (!ReadRaw(stream_, baseAddress) || !ReadRaw(stream_, sizeBytes) || !ReadRaw(stream_, kindCode) ||
            !ReadRaw(stream_, valueCount)) {
            failed_ = true;
            return CandidateStorageResult::ReadFailed;
        }
        if (kindCode > static_cast<std::uint32_t>(MemoryRegionKind::Private)) {
            failed_ = true;
            return CandidateStorageResult::CorruptFile;
        }

        RegionEntry entry;
        entry.region.baseAddress = static_cast<std::uintptr_t>(baseAddress);
        entry.region.sizeBytes = static_cast<std::size_t>(sizeBytes);
        entry.region.kind = static_cast<MemoryRegionKind>(kindCode);
        entry.valueCount = valueCount;

        pos += static_cast<std::streamoff>(kBaselineRegionEntryBytes);
        entry.valuesFileOffset = pos;
        pos += static_cast<std::streamoff>(valueCount * valueSize_);
        sumValueCounts += valueCount;

        regions.push_back(entry);

        // Skip over this region's value payload to reach the next entry.
        stream_.seekg(pos, std::ios::beg);
        if (!stream_) {
            failed_ = true;
            return CandidateStorageResult::ReadFailed;
        }
    }

    if (sumValueCounts != totalValueCount) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }

    if (!ReadMagic(stream_, kBaselineFooterMagic)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }
    std::uint64_t footerValueCount = 0, footerBytesWritten = 0;
    if (!ReadRaw(stream_, footerValueCount) || !ReadRaw(stream_, footerBytesWritten)) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }
    if (footerValueCount != totalValueCount) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }

    std::uintmax_t expectedFileSize = static_cast<std::uintmax_t>(pos) + kBaselineFooterBytes;
    if (realFileSize != expectedFileSize) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }

    regions_ = std::move(regions);
    regionsReturned_ = 0;
    return CandidateStorageResult::Success;
}

bool BaselineReader::NextRegion(ProcessMemoryRegion& outRegion, std::uint64_t& outValueCount) {
    if (failed_ || regionsReturned_ >= regions_.size()) {
        return false;
    }
    const RegionEntry& entry = regions_[regionsReturned_];
    outRegion = entry.region;
    outValueCount = entry.valueCount;
    ++regionsReturned_;
    return true;
}

std::size_t BaselineReader::ReadRegionValueChunk(std::uint64_t valueIndexInRegion, std::size_t maxValues,
                                                  std::uint8_t* outBuffer) {
    if (failed_ || regionsReturned_ == 0 || regionsReturned_ > regions_.size()) {
        return 0;
    }
    const RegionEntry& entry = regions_[regionsReturned_ - 1];
    if (valueIndexInRegion >= entry.valueCount) {
        return 0;
    }

    std::uint64_t available = entry.valueCount - valueIndexInRegion;
    std::uint64_t toRead = std::min<std::uint64_t>(static_cast<std::uint64_t>(maxValues), available);

    std::streamoff offset = entry.valuesFileOffset + static_cast<std::streamoff>(valueIndexInRegion * valueSize_);
    stream_.seekg(offset, std::ios::beg);
    stream_.read(reinterpret_cast<char*>(outBuffer), static_cast<std::streamsize>(toRead * valueSize_));
    if (!stream_) {
        failed_ = true;
        return 0;
    }
    return static_cast<std::size_t>(toRead);
}

CandidateValueType BaselineReader::ValueType() const {
    return type_;
}

std::size_t BaselineReader::ValueSize() const {
    return valueSize_;
}

bool BaselineReader::Failed() const {
    return failed_;
}

// =====================================================================
// GenerationWriter
// =====================================================================

GenerationWriter::GenerationWriter(std::filesystem::path path, CandidateValueType type, int generationNumber)
    : path_(std::move(path)), type_(type), valueSize_(CandidateValueTypeSize(type)), generationNumber_(generationNumber) {}

GenerationWriter::~GenerationWriter() {
    if (stream_.is_open()) {
        stream_.close();
    }
}

CandidateStorageResult GenerationWriter::Open() {
    stream_.open(path_, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!stream_.is_open()) {
        failed_ = true;
        return CandidateStorageResult::OpenFailed;
    }

    stream_.write(kGenerationHeaderMagic, 8);
    WriteRaw(stream_, static_cast<std::uint32_t>(kScanStorageFormatVersion));
    WriteRaw(stream_, static_cast<std::uint32_t>(type_));
    WriteRaw(stream_, static_cast<std::uint32_t>(generationNumber_));
    WriteRaw(stream_, std::uint32_t{0}); // reserved
    WriteRaw(stream_, std::uint64_t{0}); // expectedRecordCount placeholder, fixed up in Finish()

    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }
    return CandidateStorageResult::Success;
}

CandidateStorageResult GenerationWriter::WriteRecord(std::uint64_t address, const std::uint8_t* rawValueBytes) {
    if (failed_) {
        return CandidateStorageResult::WriteFailed;
    }
    if (haveLastAddress_ && address <= lastAddress_) {
        // Records must be strictly ascending -- a caller bug, fail closed
        // rather than writing an unsorted generation a later filter pass
        // couldn't correctly coalesce reads over.
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    WriteRaw(stream_, address);
    stream_.write(reinterpret_cast<const char*>(rawValueBytes), static_cast<std::streamsize>(valueSize_));
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    haveLastAddress_ = true;
    lastAddress_ = address;
    ++recordsWritten_;
    return CandidateStorageResult::Success;
}

CandidateStorageResult GenerationWriter::Finish() {
    if (failed_) {
        return CandidateStorageResult::WriteFailed;
    }

    std::uint64_t totalBytesWritten = recordsWritten_ * (8 + static_cast<std::uint64_t>(valueSize_));
    stream_.write(kGenerationFooterMagic, 8);
    WriteRaw(stream_, recordsWritten_);
    WriteRaw(stream_, totalBytesWritten);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    stream_.seekp(24, std::ios::beg);
    WriteRaw(stream_, recordsWritten_);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::WriteFailed;
    }

    stream_.flush();
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::FlushFailed;
    }
    stream_.close();
    return CandidateStorageResult::Success;
}

bool GenerationWriter::IsFailed() const {
    return failed_;
}

// =====================================================================
// GenerationReader
// =====================================================================

GenerationReader::GenerationReader(std::filesystem::path path) : path_(std::move(path)) {}

GenerationReader::~GenerationReader() = default;

CandidateStorageResult GenerationReader::Open(std::uint64_t expectedRecordCount, CandidateValueType expectedType,
                                               int expectedGeneration) {
    stream_.open(path_, std::ios::binary | std::ios::in);
    if (!stream_.is_open()) {
        failed_ = true;
        return CandidateStorageResult::OpenFailed;
    }

    std::error_code sizeError;
    std::uintmax_t realFileSize = std::filesystem::file_size(path_, sizeError);
    if (sizeError) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }

    if (!ReadMagic(stream_, kGenerationHeaderMagic)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }

    std::uint32_t formatVersion = 0, typeCode = 0, generationNumber = 0, reserved = 0;
    std::uint64_t expectedRecordCountField = 0;
    if (!ReadRaw(stream_, formatVersion) || !ReadRaw(stream_, typeCode) || !ReadRaw(stream_, generationNumber) ||
        !ReadRaw(stream_, reserved) || !ReadRaw(stream_, expectedRecordCountField)) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }

    valueSize_ = CandidateValueTypeSize(expectedType);
    if (formatVersion != static_cast<std::uint32_t>(kScanStorageFormatVersion) ||
        typeCode != static_cast<std::uint32_t>(expectedType) ||
        generationNumber != static_cast<std::uint32_t>(expectedGeneration)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }
    if (expectedRecordCountField != expectedRecordCount) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }
    type_ = expectedType;

    std::uint64_t payloadBytes = expectedRecordCount * (8 + static_cast<std::uint64_t>(valueSize_));
    std::streamoff footerPos = static_cast<std::streamoff>(kGenerationHeaderBytes + payloadBytes);
    stream_.seekg(footerPos, std::ios::beg);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }

    if (!ReadMagic(stream_, kGenerationFooterMagic)) {
        failed_ = true;
        return CandidateStorageResult::CorruptFile;
    }
    std::uint64_t footerRecordCount = 0, footerBytesWritten = 0;
    if (!ReadRaw(stream_, footerRecordCount) || !ReadRaw(stream_, footerBytesWritten)) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }
    if (footerRecordCount != expectedRecordCount || footerBytesWritten != payloadBytes) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }

    std::uintmax_t expectedFileSize = kGenerationHeaderBytes + payloadBytes + kGenerationFooterBytes;
    if (realFileSize != expectedFileSize) {
        failed_ = true;
        return CandidateStorageResult::SizeMismatch;
    }

    recordCount_ = expectedRecordCount;
    recordsReturned_ = 0;
    stream_.seekg(static_cast<std::streamoff>(kGenerationHeaderBytes), std::ios::beg);
    if (!stream_) {
        failed_ = true;
        return CandidateStorageResult::ReadFailed;
    }
    return CandidateStorageResult::Success;
}

bool GenerationReader::NextRecord(std::uint64_t& outAddress, std::uint8_t* outValueBuffer) {
    if (failed_ || recordsReturned_ >= recordCount_) {
        return false;
    }
    if (!ReadRaw(stream_, outAddress)) {
        failed_ = true;
        return false;
    }
    stream_.read(reinterpret_cast<char*>(outValueBuffer), static_cast<std::streamsize>(valueSize_));
    if (!stream_) {
        failed_ = true;
        return false;
    }
    ++recordsReturned_;
    return true;
}

CandidateValueType GenerationReader::ValueType() const {
    return type_;
}

std::size_t GenerationReader::ValueSize() const {
    return valueSize_;
}

bool GenerationReader::Failed() const {
    return failed_;
}

} // namespace sekiro_haptics::process
