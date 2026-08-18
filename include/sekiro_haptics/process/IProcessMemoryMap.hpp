#pragma once

// Read-only enumeration of an attached process's committed, readable
// memory regions -- no game meaning of any kind. Part of SEK-PROBE-001A;
// see docs/05-process-access.md for the classification policy (what
// counts as "readable") and why enumeration success never guarantees a
// later read of that same region will still succeed.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sekiro_haptics::process {

/// Coarse classification of a readable region's origin -- enough to
/// support scope filtering (main-module / private-readable / all-readable)
/// without exposing raw Win32 MEM_* constants.
enum class MemoryRegionKind {
    /// Backed by a loaded PE image (an exe or DLL's mapped sections).
    Image,
    /// A memory-mapped file/section that is not a loaded PE image.
    Mapped,
    /// Anonymous, privately-committed memory (heaps, stacks, VirtualAlloc'd
    /// regions) -- typically where dynamic game state such as HP lives.
    Private,
};

/// Returns a human-readable name for a MemoryRegionKind, e.g. for logging
/// or manifest serialization.
const char* ToString(MemoryRegionKind kind);

/// One committed, readable memory region in an attached process.
/// Everything here is a *fact about the region*, not a guarantee about any
/// specific future read -- the process's memory map can change between
/// this being reported and any later ReadBytes() call.
struct ProcessMemoryRegion {
    std::uintptr_t baseAddress = 0;
    std::size_t sizeBytes = 0;
    MemoryRegionKind kind = MemoryRegionKind::Private;
    /// Always true for MEM_COMMIT for every region IProcessMemoryMap
    /// returns -- kept as an explicit field (rather than implied) so the
    /// type documents its own invariant.
    bool committed = true;
    /// Always true for every region IProcessMemoryMap returns --
    /// EnumerateReadableRegions() only ever appends regions that passed
    /// its full readable-classification policy (committed, a
    /// read-granting protection, not PAGE_GUARD, not PAGE_NOACCESS, a
    /// non-zero and non-overflowing size). Kept as an explicit field for
    /// the same self-documentation reason as `committed`.
    bool readable = true;
};

/// Outcome of IProcessMemoryMap::EnumerateReadableRegions().
enum class MemoryMapResult {
    Success,
    /// No process attached.
    NotAttached,
    /// The attached process is no longer running.
    ProcessExited,
    /// The underlying region-query mechanism itself failed (distinct from
    /// reaching the end of the address space, which is normal successful
    /// termination, not a failure).
    EnumerationFailed,
    /// A region's own base+size, or the walk's next query address,
    /// overflowed the address space -- enumeration cannot safely continue
    /// past that point, so the whole call fails rather than silently
    /// stopping early and reporting a partial map as complete.
    AddressOverflow,
    /// The number of regions found would exceed this call's bound --
    /// never silently truncated, see docs/05-process-access.md.
    RegionCountLimitExceeded,
};

/// Returns a human-readable name for a MemoryMapResult, e.g. for logging.
const char* ToString(MemoryMapResult result);

/// Read-only enumeration of an *already-attached* process's memory
/// layout, filtered to exactly the regions safe to attempt a read from.
/// Implementations query OS metadata only -- nothing here reads region
/// *contents*, and nothing here ever changes a region's protection.
class IProcessMemoryMap {
public:
    virtual ~IProcessMemoryMap() = default;

    /// Walks the entire queryable address space and appends every region
    /// that is MEM_COMMIT, has a read-granting protection, is not
    /// PAGE_GUARD, is not PAGE_NOACCESS, and has a non-zero,
    /// non-overflowing size. `outRegions` is only written to on Success --
    /// left completely untouched on any failure, never partially filled.
    ///
    /// A successful enumeration says nothing about whether a *later*
    /// ReadBytes() call against any of these regions will still succeed --
    /// the process's memory map can change (pages freed, protection
    /// changed, the process itself exiting) at any moment after this
    /// returns. Callers must still handle read failures at read time.
    virtual MemoryMapResult EnumerateReadableRegions(std::vector<ProcessMemoryRegion>& outRegions) const = 0;
};

} // namespace sekiro_haptics::process
