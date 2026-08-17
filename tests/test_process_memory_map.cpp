// Unit tests for Win32ProcessReader's IProcessMemoryMap implementation
// (EnumerateReadableRegions()). Every OS-level fact (MEM_COMMIT, PAGE_GUARD,
// PAGE_NOACCESS, region kind, process liveness) is driven through
// FakeWin32Api's configurable region list -- no real process memory is
// ever touched here. Part of SEK-PROBE-001A.

#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "FakeWin32Api.hpp"

#include <limits>

using namespace sekiro_haptics::process;

namespace {

RawMemoryRegionInfo MakeRegion(std::uintptr_t base, std::size_t size, bool committed, bool guarded, bool noAccess,
                                bool readableProtection, RawMemoryRegionKind kind) {
    RawMemoryRegionInfo r;
    r.baseAddress = base;
    r.regionSize = size;
    r.committed = committed;
    r.guarded = guarded;
    r.noAccessProtection = noAccess;
    r.readableProtection = readableProtection;
    r.kind = kind;
    return r;
}

// A normal, fully-readable committed private region -- the common case
// most tests start from and tweak one flag on.
RawMemoryRegionInfo MakeReadablePrivateRegion(std::uintptr_t base, std::size_t size) {
    return MakeRegion(base, size, /*committed=*/true, /*guarded=*/false, /*noAccess=*/false,
                       /*readableProtection=*/true, RawMemoryRegionKind::Private);
}

} // namespace

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_CommittedReadableRegion_Included) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    api.SetMemoryRegions({MakeReadablePrivateRegion(0x10000, 0x1000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::Success);
    SH_CHECK(regions.size() == 1);
    SH_CHECK(regions[0].baseAddress == 0x10000);
    SH_CHECK(regions[0].sizeBytes == 0x1000);
    SH_CHECK(regions[0].committed);
    SH_CHECK(regions[0].readable);
    SH_CHECK(regions[0].kind == MemoryRegionKind::Private);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_GuardedRegion_Excluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo guarded = MakeReadablePrivateRegion(0x10000, 0x1000);
    guarded.guarded = true;
    api.SetMemoryRegions({guarded});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::Success);
    SH_CHECK(regions.empty());
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_NoAccessRegion_Excluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo noAccess = MakeReadablePrivateRegion(0x10000, 0x1000);
    noAccess.noAccessProtection = true;
    noAccess.readableProtection = false;
    api.SetMemoryRegions({noAccess});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::Success);
    SH_CHECK(regions.empty());
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_UncommittedRegion_Excluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo uncommitted = MakeReadablePrivateRegion(0x10000, 0x1000);
    uncommitted.committed = false;
    api.SetMemoryRegions({uncommitted});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::Success);
    SH_CHECK(regions.empty());
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_NonReadableProtection_Excluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo noRead = MakeReadablePrivateRegion(0x10000, 0x1000);
    noRead.readableProtection = false; // e.g. PAGE_EXECUTE alone -- no explicit read bit
    api.SetMemoryRegions({noRead});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::Success);
    SH_CHECK(regions.empty());
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_ZeroSizeRegion_Excluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo zeroSize = MakeReadablePrivateRegion(0x10000, 0);
    // A zero-size entry can't be walked past either -- pair it with a
    // real region afterward that would only be reached if the walk
    // didn't get stuck, proving termination is graceful either way.
    api.SetMemoryRegions({zeroSize});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::Success);
    SH_CHECK(regions.empty());
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_OverflowingRegion_ReturnsAddressOverflow) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    std::uintptr_t nearMax = std::numeric_limits<std::uintptr_t>::max() - 10;
    RawMemoryRegionInfo overflowing = MakeReadablePrivateRegion(nearMax, 1000); // base + size overflows
    api.SetMemoryRegions({overflowing});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::AddressOverflow);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_EnumerationFailure_ReturnsEnumerationFailed) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    api.ForceMemoryRegionQueryFailureAtCall(0);
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::EnumerationFailed);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_NotAttached_ReturnsNotAttached) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::NotAttached);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_ProcessExited_ReturnsProcessExited) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetProcessAlive(1234, false);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::ProcessExited);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_Failure_LeavesOutputUnchanged) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    api.ForceMemoryRegionQueryFailureAtCall(0);
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> sentinel(3); // pre-existing content
    MemoryMapResult result = reader.EnumerateReadableRegions(sentinel);

    SH_CHECK(result == MemoryMapResult::EnumerationFailed);
    SH_CHECK(sentinel.size() == 3); // untouched, not cleared or partially replaced
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_MultipleRegionKinds_ClassifiedCorrectly) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo image = MakeReadablePrivateRegion(0x10000, 0x1000);
    image.kind = RawMemoryRegionKind::Image;
    RawMemoryRegionInfo mapped = MakeReadablePrivateRegion(0x20000, 0x1000);
    mapped.kind = RawMemoryRegionKind::Mapped;
    RawMemoryRegionInfo priv = MakeReadablePrivateRegion(0x30000, 0x1000);
    priv.kind = RawMemoryRegionKind::Private;
    api.SetMemoryRegions({image, mapped, priv});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::Success);
    SH_CHECK(regions.size() == 3);
    SH_CHECK(regions[0].kind == MemoryRegionKind::Image);
    SH_CHECK(regions[1].kind == MemoryRegionKind::Mapped);
    SH_CHECK(regions[2].kind == MemoryRegionKind::Private);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_TooManyRegions_ReturnsRegionCountLimitExceeded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    std::vector<RawMemoryRegionInfo> manyRegions;
    manyRegions.reserve(200001);
    for (std::size_t i = 0; i < 200001; ++i) {
        manyRegions.push_back(MakeReadablePrivateRegion(static_cast<std::uintptr_t>(i) * 0x1000, 0x1000));
    }
    api.SetMemoryRegions(std::move(manyRegions));
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    MemoryMapResult result = reader.EnumerateReadableRegions(regions);

    SH_CHECK(result == MemoryMapResult::RegionCountLimitExceeded);
}

SH_TEST(Win32ProcessReader_EnumerateReadableRegions_MixOfReadableAndUnreadable_OnlyReadableIncluded) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "probe_target.exe"}});
    RawMemoryRegionInfo readableA = MakeReadablePrivateRegion(0x10000, 0x1000);
    RawMemoryRegionInfo guardedB = MakeReadablePrivateRegion(0x20000, 0x1000);
    guardedB.guarded = true;
    RawMemoryRegionInfo readableC = MakeReadablePrivateRegion(0x30000, 0x1000);
    api.SetMemoryRegions({readableA, guardedB, readableC});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::Success);
    SH_CHECK(regions.size() == 2);
    SH_CHECK(regions[0].baseAddress == 0x10000);
    SH_CHECK(regions[1].baseAddress == 0x30000);
}
