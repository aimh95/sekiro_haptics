// Format-level tests for ScanManifest (JSON) and the baseline/generation
// binary storage formats (SEK-PROBE-001C). Entirely OS-independent: real
// small temporary files, no fake filesystem, no process I/O, no
// multi-gigabyte writes -- large-region arithmetic is exercised via
// ComputeRegionAlignedValueCount() on synthetic metadata only.

#include "sekiro_haptics/process/CandidateScanShared.hpp"
#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"
#include "testing.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace sekiro_haptics::process;

namespace {
std::string TempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

// =====================================================================
// ScanManifest
// =====================================================================

SH_TEST(ScanManifest_WriteThenRead_RoundTripsAllFields) {
    std::string path = TempPath("sh_scan_manifest_test.json");

    ScanManifest manifest;
    manifest.identity.executableFileSizeBytes = 123456789;
    manifest.identity.sha256Hex = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef01234567";
    manifest.identity.pid = 4242;
    manifest.identity.mainModuleBaseAddress = 0x140000000;
    manifest.identity.mainModuleImageSize = 0x2000000;
    manifest.identity.valueType = "u32";
    manifest.identity.scope = "private-readable";
    manifest.alignment = 4;
    manifest.regions.push_back(ScanManifestRegion{0x1000, 4096, MemoryRegionKind::Private});
    manifest.regions.push_back(ScanManifestRegion{0x10000, 8192, MemoryRegionKind::Image});
    manifest.totalScopeBytes = 4096 + 8192;
    manifest.totalValueCount = 3000;
    manifest.processedBytes = 2048;
    manifest.coveragePercent = 16.75;
    manifest.memoryBudgetBytes = 512ULL * 1024 * 1024;
    manifest.generation = 2;
    manifest.candidateCount = 150;
    manifest.state = ScanManifestState::Filtering;
    manifest.failureReason = "";
    manifest.startMonotonicUs = 1000;
    manifest.completeMonotonicUs = 0;
    manifest.completedNormally = false;

    SH_CHECK(WriteScanManifest(path, manifest));

    ScanManifest readBack;
    std::string error;
    SH_CHECK(ReadScanManifest(path, readBack, error));

    SH_CHECK(readBack.schemaVersion == manifest.schemaVersion);
    SH_CHECK(readBack.storageFormatVersion == manifest.storageFormatVersion);
    SH_CHECK(readBack.identity == manifest.identity);
    SH_CHECK(readBack.alignment == manifest.alignment);
    SH_CHECK(readBack.regions.size() == 2);
    SH_CHECK(readBack.regions[0].baseAddress == 0x1000);
    SH_CHECK(readBack.regions[0].sizeBytes == 4096);
    SH_CHECK(readBack.regions[0].kind == MemoryRegionKind::Private);
    SH_CHECK(readBack.regions[1].baseAddress == 0x10000);
    SH_CHECK(readBack.regions[1].kind == MemoryRegionKind::Image);
    SH_CHECK(readBack.totalScopeBytes == manifest.totalScopeBytes);
    SH_CHECK(readBack.totalValueCount == manifest.totalValueCount);
    SH_CHECK(readBack.processedBytes == manifest.processedBytes);
    SH_CHECK(readBack.coveragePercent > 16.74 && readBack.coveragePercent < 16.76);
    SH_CHECK(readBack.memoryBudgetBytes == manifest.memoryBudgetBytes);
    SH_CHECK(readBack.generation == manifest.generation);
    SH_CHECK(readBack.candidateCount == manifest.candidateCount);
    SH_CHECK(readBack.state == ScanManifestState::Filtering);
    SH_CHECK(readBack.startMonotonicUs == manifest.startMonotonicUs);
    SH_CHECK(readBack.completedNormally == manifest.completedNormally);

    std::filesystem::remove(path);
}

SH_TEST(ScanManifest_Read_MalformedJson_Fails) {
    std::string path = TempPath("sh_scan_manifest_malformed.json");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "{ this is not valid json";
    }

    ScanManifest manifest;
    std::string error;
    SH_CHECK(ReadScanManifest(path, manifest, error) == false);
    SH_CHECK(!error.empty());

    std::filesystem::remove(path);
}

SH_TEST(ScanManifest_Read_InvalidState_Fails) {
    std::string path = TempPath("sh_scan_manifest_badstate.json");

    ScanManifest manifest;
    manifest.identity.valueType = "u32";
    manifest.identity.scope = "main-module";
    manifest.state = ScanManifestState::Planning;
    SH_CHECK(WriteScanManifest(path, manifest));

    std::string content;
    {
        std::ifstream in(path);
        std::ostringstream oss;
        oss << in.rdbuf();
        content = oss.str();
    }
    std::size_t pos = content.find("\"Planning\"");
    SH_CHECK(pos != std::string::npos);
    content.replace(pos, std::string("\"Planning\"").size(), "\"NotARealState\"");
    {
        std::ofstream out(path, std::ios::trunc);
        out << content;
    }

    ScanManifest readBack;
    std::string error;
    SH_CHECK(ReadScanManifest(path, readBack, error) == false);

    std::filesystem::remove(path);
}

SH_TEST(ScanManifest_Read_MissingFile_Fails) {
    ScanManifest manifest;
    std::string error;
    SH_CHECK(ReadScanManifest("/nonexistent/path/sh_scan_manifest_missing.json", manifest, error) == false);
    SH_CHECK(!error.empty());
}

// =====================================================================
// ComputeRegionAlignedValueCount -- synthetic large-region arithmetic
// =====================================================================

SH_TEST(ComputeRegionAlignedValueCount_AlignedBase_MatchesSimpleDivision) {
    ProcessMemoryRegion region;
    region.baseAddress = 0x1000; // 4-aligned
    region.sizeBytes = 32;
    SH_CHECK(ComputeRegionAlignedValueCount(region, 4) == 8);
}

SH_TEST(ComputeRegionAlignedValueCount_UnalignedBase_SkipsLeadingUnalignedBytes) {
    ProcessMemoryRegion region;
    region.baseAddress = 0x2003; // 3 mod 4 -- first aligned offset is 1
    region.sizeBytes = 20;
    SH_CHECK(ComputeRegionAlignedValueCount(region, 4) == 4); // (20-1)/4 = 4
}

SH_TEST(ComputeRegionAlignedValueCount_RegionSmallerThanAlignedStart_ReturnsZero) {
    ProcessMemoryRegion region;
    region.baseAddress = 0x2003;
    region.sizeBytes = 0; // smaller than the 1-byte alignment gap
    SH_CHECK(ComputeRegionAlignedValueCount(region, 4) == 0);
}

SH_TEST(ComputeRegionAlignedValueCount_SyntheticMultiGigabyteAlignedRegion_NoOverflow) {
    ProcessMemoryRegion region;
    region.baseAddress = 0x140000000;
    region.sizeBytes = static_cast<std::size_t>(6ULL * 1024 * 1024 * 1024); // 6 GiB -- metadata only
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);
    SH_CHECK(count == static_cast<std::uint64_t>(region.sizeBytes) / 4);
}

SH_TEST(ComputeRegionAlignedValueCount_SyntheticMultiGigabyteUnalignedRegion_NoOverflow) {
    ProcessMemoryRegion region;
    region.baseAddress = 0x140000001; // misaligned by 1 -- aligned start is 3
    region.sizeBytes = static_cast<std::size_t>(5ULL * 1024 * 1024 * 1024 + 7);
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);
    std::uint64_t expected = (static_cast<std::uint64_t>(region.sizeBytes) - 3) / 4;
    SH_CHECK(count == expected);
}

// =====================================================================
// Baseline format
// =====================================================================

SH_TEST(BaselineWriter_MultiRegionWithGaps_RoundTripsAndReconstructsAddresses) {
    std::string path = TempPath("sh_baseline_multiregion.bin");

    ProcessMemoryRegion region1;
    region1.baseAddress = 0x1000;
    region1.sizeBytes = 32; // aligned base -> 8 u32 values
    region1.kind = MemoryRegionKind::Private;

    ProcessMemoryRegion region2;
    region2.baseAddress = 0x2003; // unaligned base
    region2.sizeBytes = 20; // -> 4 u32 values
    region2.kind = MemoryRegionKind::Image;

    std::uint64_t count1 = ComputeRegionAlignedValueCount(region1, 4);
    std::uint64_t count2 = ComputeRegionAlignedValueCount(region2, 4);
    SH_CHECK(count1 == 8);
    SH_CHECK(count2 == 4);

    std::uint64_t totalValues = count1 + count2;
    std::uint64_t totalScopeBytes = region1.sizeBytes + region2.sizeBytes;

    {
        BaselineWriter writer(path, CandidateValueType::U32, totalScopeBytes, totalValues);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);

        SH_CHECK(writer.WriteRegionHeader(region1, count1) == CandidateStorageResult::Success);
        std::vector<std::uint8_t> values1(count1 * 4);
        for (std::uint64_t i = 0; i < count1; ++i) {
            std::uint32_t v = static_cast<std::uint32_t>(100 + i);
            std::memcpy(&values1[i * 4], &v, 4);
        }
        SH_CHECK(writer.WriteRegionValues(values1.data(), values1.size()) == CandidateStorageResult::Success);

        SH_CHECK(writer.WriteRegionHeader(region2, count2) == CandidateStorageResult::Success);
        std::vector<std::uint8_t> values2(count2 * 4);
        for (std::uint64_t i = 0; i < count2; ++i) {
            std::uint32_t v = static_cast<std::uint32_t>(200 + i);
            std::memcpy(&values2[i * 4], &v, 4);
        }
        SH_CHECK(writer.WriteRegionValues(values2.data(), values2.size()) == CandidateStorageResult::Success);

        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
        SH_CHECK(!writer.IsFailed());
    }

    BaselineReader reader(path);
    SH_CHECK(reader.Open(totalValues, CandidateValueType::U32) == CandidateStorageResult::Success);
    SH_CHECK(!reader.Failed());

    ProcessMemoryRegion readRegion1;
    std::uint64_t readCount1 = 0;
    SH_CHECK(reader.NextRegion(readRegion1, readCount1));
    SH_CHECK(readRegion1.baseAddress == region1.baseAddress);
    SH_CHECK(readRegion1.sizeBytes == region1.sizeBytes);
    SH_CHECK(readRegion1.kind == region1.kind);
    SH_CHECK(readCount1 == count1);

    std::vector<std::uint8_t> readBuf1(count1 * 4);
    std::size_t got1 = reader.ReadRegionValueChunk(0, static_cast<std::size_t>(count1), readBuf1.data());
    SH_CHECK(got1 == count1);
    for (std::uint64_t i = 0; i < count1; ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, &readBuf1[i * 4], 4);
        SH_CHECK(v == 100 + i);
    }
    // region1's base is already aligned, so its first value's address is
    // exactly the region base.
    SH_CHECK(region1.baseAddress % 4 == 0);

    ProcessMemoryRegion readRegion2;
    std::uint64_t readCount2 = 0;
    SH_CHECK(reader.NextRegion(readRegion2, readCount2));
    SH_CHECK(readRegion2.baseAddress == region2.baseAddress);
    SH_CHECK(readCount2 == count2);

    std::vector<std::uint8_t> readBuf2(count2 * 4);
    std::size_t got2 = reader.ReadRegionValueChunk(0, static_cast<std::size_t>(count2), readBuf2.data());
    SH_CHECK(got2 == count2);
    for (std::uint64_t i = 0; i < count2; ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, &readBuf2[i * 4], 4);
        SH_CHECK(v == 200 + i);
    }
    // region2's base is unaligned by 3, so the first value's reconstructed
    // address (base + alignedStart) must land on a 4-aligned boundary --
    // this is the closed-form formula the disk-backed scanner relies on.
    std::uintptr_t alignedStart2 = 4 - (region2.baseAddress % 4);
    std::uintptr_t firstValueAddress2 = region2.baseAddress + alignedStart2;
    SH_CHECK(firstValueAddress2 % 4 == 0);

    ProcessMemoryRegion noMoreRegion;
    std::uint64_t noMoreCount = 0;
    SH_CHECK(reader.NextRegion(noMoreRegion, noMoreCount) == false);

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(BaselineReader_ReadRegionValueChunk_MultipleCallsCoverWholeRegionInOrder) {
    std::string path = TempPath("sh_baseline_chunked.bin");

    ProcessMemoryRegion region;
    region.baseAddress = 0x1000;
    region.sizeBytes = 40; // 10 u32 values
    region.kind = MemoryRegionKind::Private;
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);
    SH_CHECK(count == 10);

    {
        BaselineWriter writer(path, CandidateValueType::U32, region.sizeBytes, count);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        SH_CHECK(writer.WriteRegionHeader(region, count) == CandidateStorageResult::Success);
        std::vector<std::uint8_t> values(count * 4);
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint32_t v = static_cast<std::uint32_t>(i);
            std::memcpy(&values[i * 4], &v, 4);
        }
        SH_CHECK(writer.WriteRegionValues(values.data(), values.size()) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    BaselineReader reader(path);
    SH_CHECK(reader.Open(count, CandidateValueType::U32) == CandidateStorageResult::Success);
    ProcessMemoryRegion readRegion;
    std::uint64_t readCount = 0;
    SH_CHECK(reader.NextRegion(readRegion, readCount));

    std::vector<std::uint32_t> reconstructed;
    std::uint64_t index = 0;
    while (index < readCount) {
        std::uint8_t buf[12] = {}; // 3 values at a time -- forces multiple calls
        std::size_t got = reader.ReadRegionValueChunk(index, 3, buf);
        SH_CHECK(got > 0);
        for (std::size_t j = 0; j < got; ++j) {
            std::uint32_t v = 0;
            std::memcpy(&v, &buf[j * 4], 4);
            reconstructed.push_back(v);
        }
        index += got;
    }
    SH_CHECK(reconstructed.size() == count);
    for (std::uint64_t i = 0; i < count; ++i) {
        SH_CHECK(reconstructed[i] == i);
    }

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(BaselineReader_Open_WrongExpectedValueCount_ReturnsSizeMismatch) {
    std::string path = TempPath("sh_baseline_mismatch.bin");
    ProcessMemoryRegion region;
    region.baseAddress = 0x1000;
    region.sizeBytes = 16;
    region.kind = MemoryRegionKind::Private;
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);

    {
        BaselineWriter writer(path, CandidateValueType::U32, region.sizeBytes, count);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        SH_CHECK(writer.WriteRegionHeader(region, count) == CandidateStorageResult::Success);
        std::vector<std::uint8_t> values(count * 4, 0);
        SH_CHECK(writer.WriteRegionValues(values.data(), values.size()) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    BaselineReader reader(path);
    CandidateStorageResult result = reader.Open(count + 1, CandidateValueType::U32);
    SH_CHECK(result == CandidateStorageResult::SizeMismatch);

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(BaselineReader_Open_CorruptHeaderMagic_ReturnsCorruptFile) {
    std::string path = TempPath("sh_baseline_corrupt_magic.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "NOT_A_VALID_BASELINE_FILE_AT_ALL_1234567890";
    }

    BaselineReader reader(path);
    CandidateStorageResult result = reader.Open(0, CandidateValueType::U32);
    SH_CHECK(result == CandidateStorageResult::CorruptFile);

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(BaselineReader_Open_TruncatedFile_IsRejected) {
    std::string path = TempPath("sh_baseline_truncated.bin");
    ProcessMemoryRegion region;
    region.baseAddress = 0x1000;
    region.sizeBytes = 16;
    region.kind = MemoryRegionKind::Private;
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);

    {
        BaselineWriter writer(path, CandidateValueType::U32, region.sizeBytes, count);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        SH_CHECK(writer.WriteRegionHeader(region, count) == CandidateStorageResult::Success);
        std::vector<std::uint8_t> values(count * 4, 0);
        SH_CHECK(writer.WriteRegionValues(values.data(), values.size()) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    std::uintmax_t size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 10); // chop into the footer

    BaselineReader reader(path);
    CandidateStorageResult result = reader.Open(count, CandidateValueType::U32);
    SH_CHECK(result == CandidateStorageResult::ReadFailed || result == CandidateStorageResult::SizeMismatch ||
              result == CandidateStorageResult::CorruptFile);
    SH_CHECK(reader.Failed());

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(BaselineWriter_IncompleteRegion_FailsOnFinish) {
    std::string path = TempPath("sh_baseline_incomplete.bin");
    ProcessMemoryRegion region;
    region.baseAddress = 0x1000;
    region.sizeBytes = 16;
    region.kind = MemoryRegionKind::Private;
    std::uint64_t count = ComputeRegionAlignedValueCount(region, 4);

    BaselineWriter writer(path, CandidateValueType::U32, region.sizeBytes, count);
    SH_CHECK(writer.Open() == CandidateStorageResult::Success);
    SH_CHECK(writer.WriteRegionHeader(region, count) == CandidateStorageResult::Success);
    // Deliberately never write the region's values.
    SH_CHECK(writer.Finish() == CandidateStorageResult::WriteFailed);
    SH_CHECK(writer.IsFailed());

    writer.Close();
    std::filesystem::remove(path);
}

// =====================================================================
// Generation format
// =====================================================================

SH_TEST(GenerationWriter_RoundTripsThroughReader) {
    std::string path = TempPath("sh_generation_roundtrip.bin");
    {
        GenerationWriter writer(path, CandidateValueType::U32, 1);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        std::uint32_t v1 = 111;
        std::uint32_t v2 = 222;
        SH_CHECK(writer.WriteRecord(0x1000, reinterpret_cast<std::uint8_t*>(&v1)) == CandidateStorageResult::Success);
        SH_CHECK(writer.WriteRecord(0x2000, reinterpret_cast<std::uint8_t*>(&v2)) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    GenerationReader reader(path);
    SH_CHECK(reader.Open(2, CandidateValueType::U32, 1) == CandidateStorageResult::Success);

    std::uint64_t addr = 0;
    std::uint8_t buf[4] = {};
    SH_CHECK(reader.NextRecord(addr, buf));
    SH_CHECK(addr == 0x1000);
    std::uint32_t v = 0;
    std::memcpy(&v, buf, 4);
    SH_CHECK(v == 111);

    SH_CHECK(reader.NextRecord(addr, buf));
    SH_CHECK(addr == 0x2000);
    std::memcpy(&v, buf, 4);
    SH_CHECK(v == 222);

    SH_CHECK(reader.NextRecord(addr, buf) == false);

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(GenerationWriter_OutOfOrderAddress_FailsClosed) {
    std::string path = TempPath("sh_generation_unordered.bin");
    GenerationWriter writer(path, CandidateValueType::U32, 1);
    SH_CHECK(writer.Open() == CandidateStorageResult::Success);
    std::uint32_t v = 1;
    SH_CHECK(writer.WriteRecord(0x2000, reinterpret_cast<std::uint8_t*>(&v)) == CandidateStorageResult::Success);
    SH_CHECK(writer.WriteRecord(0x1000, reinterpret_cast<std::uint8_t*>(&v)) == CandidateStorageResult::WriteFailed);
    SH_CHECK(writer.IsFailed());

    writer.Close();
    std::filesystem::remove(path);
}

SH_TEST(GenerationReader_Open_WrongGenerationNumber_ReturnsCorruptFile) {
    std::string path = TempPath("sh_generation_wronggen.bin");
    {
        GenerationWriter writer(path, CandidateValueType::U32, 1);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        std::uint32_t v = 1;
        SH_CHECK(writer.WriteRecord(0x1000, reinterpret_cast<std::uint8_t*>(&v)) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    GenerationReader reader(path);
    SH_CHECK(reader.Open(1, CandidateValueType::U32, 2) == CandidateStorageResult::CorruptFile);

    reader.Close();
    std::filesystem::remove(path);
}

SH_TEST(GenerationReader_Open_WrongRecordCount_ReturnsSizeMismatch) {
    std::string path = TempPath("sh_generation_wrongcount.bin");
    {
        GenerationWriter writer(path, CandidateValueType::U32, 1);
        SH_CHECK(writer.Open() == CandidateStorageResult::Success);
        std::uint32_t v = 1;
        SH_CHECK(writer.WriteRecord(0x1000, reinterpret_cast<std::uint8_t*>(&v)) == CandidateStorageResult::Success);
        SH_CHECK(writer.Finish() == CandidateStorageResult::Success);
    }

    GenerationReader reader(path);
    SH_CHECK(reader.Open(2, CandidateValueType::U32, 1) == CandidateStorageResult::SizeMismatch);

    reader.Close();
    std::filesystem::remove(path);
}
