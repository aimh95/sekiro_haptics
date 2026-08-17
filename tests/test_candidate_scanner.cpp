// Unit tests for BeginCandidateScan()/FilterCandidates() -- the generic
// unknown-value candidate scanner used by SEK-PROBE-001A's signal-discovery
// CLI. No real process, no game meaning of any kind -- every scenario here
// is driven through Fake doubles.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessMemoryMap.hpp"
#include "FakeProcessReader.hpp"

#include <cmath>
#include <cstring>
#include <limits>

using namespace sekiro_haptics::process;

namespace {

ProcessMemoryRegion MakePrivateRegion(std::uintptr_t base, std::size_t size) {
    ProcessMemoryRegion r;
    r.baseAddress = base;
    r.sizeBytes = size;
    r.kind = MemoryRegionKind::Private;
    r.committed = true;
    r.readable = true;
    return r;
}

std::vector<std::uint8_t> FloatBytes(float f) {
    std::vector<std::uint8_t> bytes(4);
    std::memcpy(bytes.data(), &f, 4);
    return bytes;
}

} // namespace

// ===========================================================================
// BeginCandidateScan: decoding, alignment
// ===========================================================================

SH_TEST(BeginCandidateScan_U8_DecodesEveryByte) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});
    std::uint8_t data[4] = {0xAB, 0xCD, 0xEF, 0x12};
    reader.PokeBytes(0x10000, data, sizeof(data));

    std::vector<Candidate> candidates;
    CandidateScanResult result =
        BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U8,
                            candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 4);
    SH_CHECK(candidates[0].address == 0x10000);
    SH_CHECK(std::get<std::uint8_t>(candidates[0].value) == 0xAB);
    SH_CHECK(std::get<std::uint8_t>(candidates[3].value) == 0x12);
}

SH_TEST(BeginCandidateScan_U16_DecodesLittleEndianAtTwoByteAlignment) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});
    std::uint8_t data[4] = {0x34, 0x12, 0x78, 0x56}; // 0x1234, 0x5678
    reader.PokeBytes(0x10000, data, sizeof(data));

    std::vector<Candidate> candidates;
    CandidateScanResult result =
        BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                            CandidateValueType::U16, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 2);
    SH_CHECK(candidates[0].address == 0x10000);
    SH_CHECK(std::get<std::uint16_t>(candidates[0].value) == 0x1234);
    SH_CHECK(candidates[1].address == 0x10002);
    SH_CHECK(std::get<std::uint16_t>(candidates[1].value) == 0x5678);
}

SH_TEST(BeginCandidateScan_U32_DecodesLittleEndian) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});
    std::uint8_t data[4] = {0x78, 0x56, 0x34, 0x12}; // 0x12345678
    reader.PokeBytes(0x10000, data, sizeof(data));

    std::vector<Candidate> candidates;
    CandidateScanResult result =
        BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                            CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(std::get<std::uint32_t>(candidates[0].value) == 0x12345678u);
}

SH_TEST(BeginCandidateScan_I32_DecodesNegativeValueCorrectly) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});
    std::uint8_t data[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // -1
    reader.PokeBytes(0x10000, data, sizeof(data));

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::I32, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(std::get<std::int32_t>(candidates[0].value) == -1);
}

SH_TEST(BeginCandidateScan_F32_DecodesKnownFloatValue) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});
    reader.PokeBytes(0x10000, FloatBytes(1.5f).data(), 4);

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::F32, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(std::get<float>(candidates[0].value) == 1.5f);
}

SH_TEST(BeginCandidateScan_AlignmentPolicy_OnlyNaturallyAlignedAddressesBecomeCandidates) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    // Base is not 4-byte aligned; region covers [0x10001, 0x10009).
    memoryMap.SetRegions({MakePrivateRegion(0x10001, 8)});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    // Only 0x10004 is 4-byte aligned and has a full 4 bytes before the
    // region ends at 0x10009 -- 0x10008 would need bytes through 0x1000C.
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10004);
}

SH_TEST(BeginCandidateScan_RegionBoundary_NeverProducesCandidateExtendingPastRegionEnd) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 10)}); // [0x10000, 0x1000A)

    std::vector<Candidate> candidates;
    SH_CHECK(BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                                 CandidateValueType::U32, candidates) == CandidateScanResult::Success);
    for (const Candidate& c : candidates) {
        SH_CHECK(c.address + 4 <= 0x1000A);
    }
    SH_CHECK(candidates.size() == 2); // 0x10000 and 0x10004 -- 0x10008 would need bytes through 0x1000C
}

// ===========================================================================
// BeginCandidateScan: scope
// ===========================================================================

SH_TEST(BeginCandidateScan_MainModuleScope_ClipsToModuleRange) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    ModuleInfo mainModule;
    mainModule.baseAddress = 0x10010;
    mainModule.imageSize = 0x10;
    inspector.SetMainModule(mainModule);

    FakeProcessMemoryMap memoryMap;
    // An Image region wider than the module on both sides (as if it also
    // covered an adjacent module) -- candidates must only come from the
    // module's own [0x10010, 0x10020) range.
    ProcessMemoryRegion imageRegion;
    imageRegion.baseAddress = 0x10000;
    imageRegion.sizeBytes = 0x30;
    imageRegion.kind = MemoryRegionKind::Image;
    imageRegion.committed = true;
    imageRegion.readable = true;
    memoryMap.SetRegions({imageRegion});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::MainModule, CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 4); // 0x10010, 0x10014, 0x10018, 0x1001C -- all within the module's own range
    for (const Candidate& c : candidates) {
        SH_CHECK(c.address >= mainModule.baseAddress);
        SH_CHECK(c.address + 4 <= mainModule.baseAddress + mainModule.imageSize);
    }
}

SH_TEST(BeginCandidateScan_MainModuleScope_ModuleNotFound_ReturnsModuleNotFound) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    inspector.ForceGetMainModuleResult(ProcessInspectionResult::MainModuleNotFound);
    FakeProcessMemoryMap memoryMap;

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::MainModule, CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::ModuleNotFound);
}

SH_TEST(BeginCandidateScan_PrivateReadableScope_ExcludesImageRegions) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    ProcessMemoryRegion image = MakePrivateRegion(0x10000, 4);
    image.kind = MemoryRegionKind::Image;
    ProcessMemoryRegion priv = MakePrivateRegion(0x20000, 4);
    memoryMap.SetRegions({image, priv});
    reader.PokeBytes(0x20000, FloatBytes(0.0f).data(), 4);

    std::vector<Candidate> candidates;
    SH_CHECK(BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                                 CandidateValueType::U32, candidates) == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x20000);
}

SH_TEST(BeginCandidateScan_AllReadableScope_IncludesEveryKind) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    ProcessMemoryRegion image = MakePrivateRegion(0x10000, 4);
    image.kind = MemoryRegionKind::Image;
    ProcessMemoryRegion priv = MakePrivateRegion(0x20000, 4);
    memoryMap.SetRegions({image, priv});

    std::vector<Candidate> candidates;
    SH_CHECK(BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::AllReadable,
                                 CandidateValueType::U32, candidates) == CandidateScanResult::Success);
    SH_CHECK(candidates.size() == 2);
}

// ===========================================================================
// BeginCandidateScan: failures
// ===========================================================================

SH_TEST(BeginCandidateScan_MemoryMapNotAttached_ReturnsNotAttached) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.ForceResult(MemoryMapResult::NotAttached);

    std::vector<Candidate> sentinel(3);
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::AllReadable, CandidateValueType::U32, sentinel);

    SH_CHECK(result == CandidateScanResult::NotAttached);
    SH_CHECK(sentinel.size() == 3); // untouched
}

SH_TEST(BeginCandidateScan_ChunkReadFails_ReturnsReadFailed) {
    FakeProcessReader reader;
    reader.FailReadAtCall(0);
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});

    std::vector<Candidate> sentinel(2);
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32, sentinel);

    SH_CHECK(result == CandidateScanResult::ReadFailed);
    SH_CHECK(sentinel.size() == 2);
}

SH_TEST(BeginCandidateScan_ChunkPartialRead_ReturnsPartialRead) {
    FakeProcessReader reader;
    reader.ForcePartialReadAtCall(0, 2);
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::PartialRead);
}

SH_TEST(BeginCandidateScan_ProcessExited_ReturnsProcessExited) {
    FakeProcessReader reader;
    reader.SetAlive(false);
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 4)});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32, candidates);

    SH_CHECK(result == CandidateScanResult::ProcessExited);
}

SH_TEST(BeginCandidateScan_CandidateLimitExceeded_FailsClosed) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 16)}); // 4 u32 candidates

    std::vector<Candidate> sentinel(1);
    CandidateScanResult result =
        BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U32,
                            sentinel, /*maxCandidates=*/2, kDefaultMaxScanBytes);

    SH_CHECK(result == CandidateScanResult::CandidateLimitExceeded);
    SH_CHECK(sentinel.size() == 1); // untouched
}

SH_TEST(BeginCandidateScan_ScanByteLimitExceeded_FailsClosed) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakePrivateRegion(0x10000, 16)});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                                                      CandidateValueType::U32, candidates, kDefaultMaxCandidates,
                                                      /*maxScanBytes=*/4);

    SH_CHECK(result == CandidateScanResult::CandidateLimitExceeded);
}

SH_TEST(BeginCandidateScan_RegionAddressOverflow_ReturnsAddressOverflow) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    std::uintptr_t nearMax = std::numeric_limits<std::uintptr_t>::max() - 500;
    ProcessMemoryRegion overflowing = MakePrivateRegion(nearMax, 1000); // base+size overflows
    memoryMap.SetRegions({overflowing});

    std::vector<Candidate> candidates;
    CandidateScanResult result = BeginCandidateScan(
        reader, inspector, memoryMap, CandidateScanScope::PrivateReadable, CandidateValueType::U8, candidates);

    SH_CHECK(result == CandidateScanResult::AddressOverflow);
}

// ===========================================================================
// FilterCandidates: changed / unchanged / increased / decreased / exact
// ===========================================================================

SH_TEST(FilterCandidates_Changed_KeepsOnlyModifiedCandidates) {
    FakeProcessReader reader;
    reader.PokeBytes(0x10000, FloatBytes(0.0f).data(), 4); // placeholder, overwritten below as u32
    std::uint32_t v1 = 100, v2 = 200;
    reader.PokeBytes(0x10000, &v1, 4);
    reader.PokeBytes(0x10004, &v2, 4);

    std::vector<Candidate> candidates;
    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{200};
    candidates = {a, b};

    std::uint32_t newV1 = 999; // changed
    reader.PokeBytes(0x10000, &newV1, 4);
    // v2 unchanged at 0x10004.

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Changed);

    SH_CHECK(result == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
    SH_CHECK(std::get<std::uint32_t>(candidates[0].value) == 999u); // baseline advanced
}

SH_TEST(FilterCandidates_Unchanged_KeepsOnlyStableCandidates) {
    FakeProcessReader reader;
    std::uint32_t v1 = 100, v2 = 200;
    reader.PokeBytes(0x10000, &v1, 4);
    reader.PokeBytes(0x10004, &v2, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{999}; // stale baseline, will not match current 200
    std::vector<Candidate> candidates = {a, b};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Unchanged);

    SH_CHECK(result == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
}

SH_TEST(FilterCandidates_Increased_KeepsOnlyLargerCandidates) {
    FakeProcessReader reader;
    std::uint32_t up = 150, down = 50;
    reader.PokeBytes(0x10000, &up, 4);
    reader.PokeBytes(0x10004, &down, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{100};
    std::vector<Candidate> candidates = {a, b};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Increased) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
    SH_CHECK(std::get<std::uint32_t>(candidates[0].value) == 150u);
}

SH_TEST(FilterCandidates_Decreased_KeepsOnlySmallerCandidates) {
    FakeProcessReader reader;
    std::uint32_t up = 150, down = 50;
    reader.PokeBytes(0x10000, &up, 4);
    reader.PokeBytes(0x10004, &down, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{100};
    std::vector<Candidate> candidates = {a, b};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Decreased) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10004);
}

SH_TEST(FilterCandidates_ExactValue_KeepsOnlyMatchingCandidates) {
    FakeProcessReader reader;
    std::uint32_t match = 77, noMatch = 88;
    reader.PokeBytes(0x10000, &match, 4);
    reader.PokeBytes(0x10004, &noMatch, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{0};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{0};
    std::vector<Candidate> candidates = {a, b};

    CandidateValue target = std::uint32_t{77};
    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::ExactValue, &target) ==
             CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
}

SH_TEST(FilterCandidates_ExactValue_NoTargetProvided_ReturnsInvalidTargetAndLeavesCandidatesUnchanged) {
    FakeProcessReader reader;
    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{0};
    std::vector<Candidate> candidates = {a};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::ExactValue, nullptr);

    SH_CHECK(result == CandidateFilterResult::InvalidTarget);
    SH_CHECK(candidates.size() == 1);
}

SH_TEST(FilterCandidates_SignedValues_IncreasedDecreasedRespectSign) {
    FakeProcessReader reader;
    std::int32_t lessNegative = -10; // increased relative to -50
    std::int32_t moreNegative = -100; // decreased relative to -50
    reader.PokeBytes(0x10000, &lessNegative, 4);
    reader.PokeBytes(0x10004, &moreNegative, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::I32;
    a.value = std::int32_t{-50};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::I32;
    b.value = std::int32_t{-50};
    std::vector<Candidate> candidates = {a, b};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Increased) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
}

// ===========================================================================
// FilterCandidates: float epsilon / NaN / Infinity
// ===========================================================================

SH_TEST(FilterCandidates_FloatWithinEpsilon_TreatedAsUnchanged) {
    FakeProcessReader reader;
    reader.PokeBytes(0x10000, FloatBytes(1.00005f).data(), 4); // delta 0.00005 < epsilon 0.0001

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::F32;
    a.value = 1.0f;
    std::vector<Candidate> candidates = {a};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Unchanged) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
}

SH_TEST(FilterCandidates_FloatBeyondEpsilon_TreatedAsChanged) {
    FakeProcessReader reader;
    reader.PokeBytes(0x10000, FloatBytes(1.01f).data(), 4); // delta 0.01 > epsilon

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::F32;
    a.value = 1.0f;
    std::vector<Candidate> candidates = {a};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Changed) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
}

SH_TEST(FilterCandidates_FloatNaN_NeverPassesIncreasedOrDecreased) {
    FakeProcessReader reader;
    float nanValue = std::numeric_limits<float>::quiet_NaN();
    reader.PokeBytes(0x10000, FloatBytes(nanValue).data(), 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::F32;
    a.value = 5.0f;
    std::vector<Candidate> increasedCandidates = {a};
    std::vector<Candidate> decreasedCandidates = {a};

    SH_CHECK(FilterCandidates(reader, increasedCandidates, CandidateFilterKind::Increased) ==
             CandidateFilterResult::Success);
    SH_CHECK(increasedCandidates.empty());
    SH_CHECK(FilterCandidates(reader, decreasedCandidates, CandidateFilterKind::Decreased) ==
             CandidateFilterResult::Success);
    SH_CHECK(decreasedCandidates.empty());
}

SH_TEST(FilterCandidates_FloatNaN_TreatedAsChangedNeverUnchanged) {
    FakeProcessReader reader;
    float nanValue = std::numeric_limits<float>::quiet_NaN();
    reader.PokeBytes(0x10000, FloatBytes(nanValue).data(), 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::F32;
    a.value = 5.0f;
    std::vector<Candidate> changedCandidates = {a};
    std::vector<Candidate> unchangedCandidates = {a};

    SH_CHECK(FilterCandidates(reader, changedCandidates, CandidateFilterKind::Changed) ==
             CandidateFilterResult::Success);
    SH_CHECK(changedCandidates.size() == 1); // NaN counts as "changed" from a finite baseline

    SH_CHECK(FilterCandidates(reader, unchangedCandidates, CandidateFilterKind::Unchanged) ==
             CandidateFilterResult::Success);
    SH_CHECK(unchangedCandidates.empty());
}

SH_TEST(FilterCandidates_FloatInfinity_EqualInfinitiesAreUnchanged) {
    FakeProcessReader reader;
    float posInf = std::numeric_limits<float>::infinity();
    reader.PokeBytes(0x10000, FloatBytes(posInf).data(), 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::F32;
    a.value = posInf;
    std::vector<Candidate> candidates = {a};

    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Unchanged) == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
}

// ===========================================================================
// FilterCandidates: read failure handling
// ===========================================================================

SH_TEST(FilterCandidates_PartialReadForOneCandidate_RemovesOnlyThatCandidate) {
    FakeProcessReader reader;
    std::uint32_t v1 = 100, v2 = 200;
    reader.PokeBytes(0x10000, &v1, 4);
    reader.PokeBytes(0x10004, &v2, 4);
    reader.ForcePartialReadAtCall(0, 2); // only the first candidate's read is partial

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{999}; // will become 200 after re-read, filtered by "changed"
    std::vector<Candidate> candidates = {a, b};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Changed);

    SH_CHECK(result == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10004);
}

SH_TEST(FilterCandidates_CompleteReadFailureForOneCandidate_RemovesOnlyThatCandidate) {
    FakeProcessReader reader;
    std::uint32_t v2 = 200;
    reader.PokeBytes(0x10004, &v2, 4);
    reader.FailReadAtCall(0);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{200};
    std::vector<Candidate> candidates = {a, b};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Unchanged);

    SH_CHECK(result == CandidateFilterResult::Success);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10004);
}

SH_TEST(FilterCandidates_NotAttached_FailsWholeCommandAndLeavesCandidatesUnchanged) {
    FakeProcessReader reader;
    reader.SetAttached(false);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    std::vector<Candidate> candidates = {a};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Changed);

    SH_CHECK(result == CandidateFilterResult::NotAttached);
    SH_CHECK(candidates.size() == 1);
    SH_CHECK(std::get<std::uint32_t>(candidates[0].value) == 100u); // untouched, not partially updated
}

SH_TEST(FilterCandidates_ProcessExited_FailsWholeCommandAndLeavesCandidatesUnchanged) {
    FakeProcessReader reader;
    reader.SetAlive(false);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100};
    std::vector<Candidate> candidates = {a};

    CandidateFilterResult result = FilterCandidates(reader, candidates, CandidateFilterKind::Changed);

    SH_CHECK(result == CandidateFilterResult::ProcessExited);
    SH_CHECK(candidates.size() == 1);
}

SH_TEST(FilterCandidates_UnrelatedNonMatchingCandidate_RemovedByFilter) {
    FakeProcessReader reader;
    std::uint32_t related = 999, unrelated = 42; // unrelated never changes
    reader.PokeBytes(0x10000, &related, 4);
    reader.PokeBytes(0x10004, &unrelated, 4);

    Candidate a;
    a.address = 0x10000;
    a.type = CandidateValueType::U32;
    a.value = std::uint32_t{100}; // will differ from 999 -- "changed"
    Candidate b;
    b.address = 0x10004;
    b.type = CandidateValueType::U32;
    b.value = std::uint32_t{42}; // matches current value -- "unchanged", removed by "changed" filter

    std::vector<Candidate> candidates = {a, b};
    SH_CHECK(FilterCandidates(reader, candidates, CandidateFilterKind::Changed) == CandidateFilterResult::Success);

    SH_CHECK(candidates.size() == 1);
    SH_CHECK(candidates[0].address == 0x10000);
}
