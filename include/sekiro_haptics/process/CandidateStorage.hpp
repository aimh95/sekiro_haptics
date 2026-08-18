#pragma once

// Binary on-disk formats for a disk-backed scan's baseline and filter
// generations (SEK-PROBE-001C). Neither format stores a per-value
// address: a baseline's values are implicitly addressed by their position
// within a region (see ComputeRegionAlignedValueCount's doc comment for
// the closed-form reconstruction), and a generation's sparse survivor
// records each carry their own absolute address explicitly (they are no
// longer a dense, position-addressable run after filtering).
//
// Corruption detection relies on magic-string validation (header +
// footer) plus exact on-disk-file-size and record-count cross-validation
// -- no CRC. This catches truncation and garbled headers/footers/sizes,
// which is everything this project's test matrix requires; a checksum can
// be added later as its own independent change if a real corruption
// pattern beyond that is ever observed.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace sekiro_haptics::process {

/// Version of the baseline.bin/candidates-NNNN.bin binary layouts
/// implemented by this file. Recorded in both file headers and in
/// ScanManifest::storageFormatVersion. Bump and document in
/// docs/06-signal-discovery-probe.md if either layout ever changes.
inline constexpr int kScanStorageFormatVersion = 1;

/// Outcome of a CandidateStorage read/write operation.
enum class CandidateStorageResult {
    Success,
    OpenFailed,
    WriteFailed,
    FlushFailed,
    ReadFailed,
    CorruptFile,
    SizeMismatch,
};
const char* ToString(CandidateStorageResult result);

// --- Baseline format --------------------------------------------------
//
// [48-byte header]
//   char[8]   magic = "SHBASE1\0"
//   uint32_t  storageFormatVersion
//   uint32_t  valueTypeCode      (CandidateValueType as int)
//   uint32_t  valueSizeBytes     (CandidateValueTypeSize(type))
//   uint32_t  regionCount
//   uint64_t  totalScopeBytes
//   uint64_t  totalValueCount    (expected -- for read-back validation)
//   uint64_t  reserved = 0
// [repeated, once per region, ascending baseAddress order]
//   [28-byte region directory entry]
//     uint64_t baseAddress
//     uint64_t sizeBytes
//     uint32_t kindCode          (MemoryRegionKind as int)
//     uint64_t valueCount        (ComputeRegionAlignedValueCount() result --
//                                 64-bit so a hypothetical >4-billion-value
//                                 region, e.g. a large mapped file backing
//                                 store scanned as u8, cannot silently wrap)
//   [valueCount * valueSizeBytes raw little-endian value bytes]
// [24-byte footer]
//   char[8]   magic = "SHBEND1\0"
//   uint64_t  totalValueCountWritten
//   uint64_t  totalBytesWritten  (sum of every region directory entry +
//                                 its values, for the file-size cross-check)

inline constexpr char kBaselineHeaderMagic[8] = {'S', 'H', 'B', 'A', 'S', 'E', '1', '\0'};
inline constexpr char kBaselineFooterMagic[8] = {'S', 'H', 'B', 'E', 'N', 'D', '1', '\0'};
inline constexpr std::size_t kBaselineHeaderBytes = 48;
inline constexpr std::size_t kBaselineRegionEntryBytes = 28;
inline constexpr std::size_t kBaselineFooterBytes = 24;

/// Write-side I/O buffering unit -- fixed, independent of scope/candidate
/// count. Also used as the read-side chunk size.
inline constexpr std::size_t kCandidateStorageIoChunkBytes = 1 * 1024 * 1024;

/// Writes a baseline.bin-shaped file. Usage: construct, Open(), then for
/// each region in order call WriteRegionHeader() followed by one or more
/// WriteRegionValues() calls covering exactly that region's value count,
/// then Finish() once at the very end.
class BaselineWriter {
public:
    BaselineWriter(std::filesystem::path path, CandidateValueType type, std::uint64_t totalScopeBytes,
                    std::uint64_t totalValueCount);
    ~BaselineWriter();

    BaselineWriter(const BaselineWriter&) = delete;
    BaselineWriter& operator=(const BaselineWriter&) = delete;

    CandidateStorageResult Open();

    /// Declares the next region and how many aligned values it contains.
    /// Must be followed by exactly `valueCount` values' worth of bytes
    /// across one or more WriteRegionValues() calls before the next
    /// WriteRegionHeader() (or Finish()).
    CandidateStorageResult WriteRegionHeader(const ProcessMemoryRegion& region, std::uint64_t valueCount);

    /// Appends raw little-endian value bytes for the current region.
    /// `byteCount` must be a multiple of the writer's value size.
    CandidateStorageResult WriteRegionValues(const std::uint8_t* rawValueBytes, std::size_t byteCount);

    /// Writes the footer, flushes, and closes. Must be called exactly
    /// once, after every region has been fully written. `IsFailed()` must
    /// be false when this is called.
    CandidateStorageResult Finish();

    bool IsFailed() const;

private:
    std::filesystem::path path_;
    CandidateValueType type_;
    std::size_t valueSize_;
    std::uint64_t totalScopeBytes_;
    std::uint64_t totalValueCount_;
    std::ofstream stream_;
    std::uint32_t regionCount_ = 0;
    std::uint64_t valuesWritten_ = 0;
    std::uint64_t payloadBytesWritten_ = 0;
    std::uint64_t currentRegionValuesRemaining_ = 0;
    bool failed_ = false;
};

/// Reads a baseline.bin-shaped file back, region by region, in the same
/// order it was written.
class BaselineReader {
public:
    explicit BaselineReader(std::filesystem::path path);
    ~BaselineReader();

    BaselineReader(const BaselineReader&) = delete;
    BaselineReader& operator=(const BaselineReader&) = delete;

    /// Validates the header, every region directory entry, and the
    /// footer/file-size cross-checks against `expectedTotalValueCount`
    /// and `expectedType` -- CorruptFile/SizeMismatch/ReadFailed on any
    /// inconsistency. On Success, positions the reader at the first region.
    CandidateStorageResult Open(std::uint64_t expectedTotalValueCount, CandidateValueType expectedType);

    /// Advances to the next region and reports it. Returns false (with no
    /// error -- this is normal end-of-file) once every region has been
    /// consumed.
    bool NextRegion(ProcessMemoryRegion& outRegion, std::uint64_t& outValueCount);

    /// Reads up to `maxValues` raw values (valueSize bytes each) from the
    /// CURRENT region (the one most recently returned by NextRegion()),
    /// starting at `valueIndexInRegion`, into caller-owned `outBuffer`
    /// (must be at least `maxValues * valueSize` bytes). Returns the
    /// number of values actually read (less than `maxValues` only at the
    /// current region's end).
    std::size_t ReadRegionValueChunk(std::uint64_t valueIndexInRegion, std::size_t maxValues, std::uint8_t* outBuffer);

    CandidateValueType ValueType() const;
    std::size_t ValueSize() const;
    bool Failed() const;

private:
    struct RegionEntry {
        ProcessMemoryRegion region;
        std::uint64_t valueCount = 0;
        std::streamoff valuesFileOffset = 0;
    };

    std::filesystem::path path_;
    std::ifstream stream_;
    CandidateValueType type_ = CandidateValueType::U32;
    std::size_t valueSize_ = 4;
    // Region directory only -- bounded by region COUNT (thousands), never
    // by scope size, so holding this in memory does not reintroduce the
    // scope-proportional allocation this ticket exists to eliminate.
    std::vector<RegionEntry> regions_;
    std::size_t regionsReturned_ = 0;
    bool failed_ = false;
};

// --- Generation format --------------------------------------------------
//
// [32-byte header]
//   char[8]   magic = "SHGEN1\0\0"
//   uint32_t  storageFormatVersion
//   uint32_t  valueTypeCode
//   uint32_t  generationNumber
//   uint32_t  reserved = 0
//   uint64_t  expectedRecordCount
// [repeated, once per surviving candidate, strictly ascending address]
//   uint64_t  address
//   [valueSizeBytes raw little-endian current value bytes]
// [24-byte footer]
//   char[8]   magic = "SHGEND1\0"
//   uint64_t  actualRecordCount
//   uint64_t  totalBytesWritten

inline constexpr char kGenerationHeaderMagic[8] = {'S', 'H', 'G', 'E', 'N', '1', '\0', '\0'};
inline constexpr char kGenerationFooterMagic[8] = {'S', 'H', 'G', 'E', 'N', 'D', '1', '\0'};
inline constexpr std::size_t kGenerationHeaderBytes = 32;
inline constexpr std::size_t kGenerationFooterBytes = 24;

class GenerationWriter {
public:
    GenerationWriter(std::filesystem::path path, CandidateValueType type, int generationNumber);
    ~GenerationWriter();

    GenerationWriter(const GenerationWriter&) = delete;
    GenerationWriter& operator=(const GenerationWriter&) = delete;

    CandidateStorageResult Open();

    /// Appends one record. `address` must be strictly greater than the
    /// previously written record's address (enforced) -- records must be
    /// written in ascending order.
    CandidateStorageResult WriteRecord(std::uint64_t address, const std::uint8_t* rawValueBytes);

    CandidateStorageResult Finish();
    bool IsFailed() const;

private:
    std::filesystem::path path_;
    CandidateValueType type_;
    std::size_t valueSize_;
    int generationNumber_;
    std::ofstream stream_;
    std::uint64_t recordsWritten_ = 0;
    bool haveLastAddress_ = false;
    std::uint64_t lastAddress_ = 0;
    bool failed_ = false;
};

class GenerationReader {
public:
    explicit GenerationReader(std::filesystem::path path);
    ~GenerationReader();

    GenerationReader(const GenerationReader&) = delete;
    GenerationReader& operator=(const GenerationReader&) = delete;

    CandidateStorageResult Open(std::uint64_t expectedRecordCount, CandidateValueType expectedType,
                                 int expectedGeneration);

    /// Reads the next record. Returns false (no error -- normal
    /// end-of-file) once every record has been consumed. `outValueBuffer`
    /// must be at least ValueSize() bytes.
    bool NextRecord(std::uint64_t& outAddress, std::uint8_t* outValueBuffer);

    CandidateValueType ValueType() const;
    std::size_t ValueSize() const;
    bool Failed() const;

private:
    std::filesystem::path path_;
    std::ifstream stream_;
    CandidateValueType type_ = CandidateValueType::U32;
    std::size_t valueSize_ = 4;
    std::uint64_t recordCount_ = 0;
    std::uint64_t recordsReturned_ = 0;
    bool failed_ = false;
};

} // namespace sekiro_haptics::process
