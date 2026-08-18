#pragma once

// Generic unknown-value candidate scanner/filter -- the memory-scanning
// technique (find every plausible value, then narrow by how it changes)
// used by SEK-PROBE-001A's signal-discovery CLI. Deliberately NOT built on
// AobScanner.hpp: that scanner searches for a known byte *pattern*: this
// module has no pattern, only a value type and a change policy, so it
// gets its own minimal implementation rather than forcing an unrelated
// shape into AOB scanning. No game meaning of any kind lives here -- see
// docs/05-process-access.md.

#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace sekiro_haptics::process {

/// Supported candidate value interpretations. Raw memory bytes are never
/// treated as a meaningful value on their own -- only at an address the
/// chosen type's natural alignment permits, decoded as exactly this type.
enum class CandidateValueType {
    U8,
    U16,
    U32,
    I32,
    F32,
};

/// Byte size of `type` (1, 2, or 4).
std::size_t CandidateValueTypeSize(CandidateValueType type);

/// Short lowercase name for `type` ("u8", "u16", "u32", "i32", "f32") --
/// used by the CLI and by session/watch record serialization.
const char* ToString(CandidateValueType type);

/// A decoded candidate value, tagged by which alternative is active via
/// the owning Candidate's `type` -- callers always know which alternative
/// to expect and never need to probe it.
using CandidateValue = std::variant<std::uint8_t, std::uint16_t, std::uint32_t, std::int32_t, float>;

/// Decodes a little-endian `type`-shaped value from `bytes`, which must
/// point at least `CandidateValueTypeSize(type)` readable bytes. Pure
/// decode, no bounds/alignment checking of its own -- BeginCandidateScan()
/// and any single-address read (e.g. a watch sample) are both expected to
/// have already validated the read before calling this.
CandidateValue DecodeCandidateValue(CandidateValueType type, const std::uint8_t* bytes);

/// One surviving candidate address: where it lives, what type it's being
/// interpreted as, and the most recently observed value at that address
/// (the filter baseline for the *next* Filter() call).
struct Candidate {
    std::uintptr_t address = 0;
    CandidateValueType type = CandidateValueType::U32;
    CandidateValue value;
};

/// Which memory regions BeginCandidateScan() considers.
enum class CandidateScanScope {
    /// Only the attached process's own main module's image range.
    MainModule,
    /// Every readable region NOT backed by a loaded image (anonymous
    /// heap/stack/VirtualAlloc'd memory) -- typically where dynamic game
    /// state lives.
    PrivateReadable,
    /// Every readable region regardless of kind.
    AllReadable,
};

/// Returns a human-readable name for a CandidateScanScope ("main-module",
/// "private-readable", "all-readable" -- matching the CLI's own verb
/// spelling), e.g. for logging or manifest serialization.
const char* ToString(CandidateScanScope scope);

/// Outcome of BeginCandidateScan().
enum class CandidateScanResult {
    Success,
    NotAttached,
    ProcessExited,
    ReadFailed,
    PartialRead,
    /// CandidateScanScope::MainModule was requested but the main module
    /// couldn't be identified.
    ModuleNotFound,
    /// The underlying IProcessMemoryMap::EnumerateReadableRegions() call
    /// itself failed (enumeration failure, overflow, or its own region
    /// count limit).
    MemoryMapUnavailable,
    /// The number of candidates (or total bytes read) would exceed this
    /// call's bound -- never silently truncated, see
    /// docs/05-process-access.md.
    CandidateLimitExceeded,
    /// A chunk address computation overflowed the address space.
    AddressOverflow,
};

/// Returns a human-readable name for a CandidateScanResult, e.g. for logging.
const char* ToString(CandidateScanResult result);

/// Default cap on the number of candidates a single BeginCandidateScan()
/// call may produce -- a public, named bound rather than an implicit,
/// undocumented limit. Exceeding it fails the whole scan closed rather
/// than returning a silently-truncated candidate set.
inline constexpr std::size_t kDefaultMaxCandidates = 2'000'000;

/// Default cap on total bytes a single BeginCandidateScan() call may read
/// across every region in scope.
inline constexpr std::uint64_t kDefaultMaxScanBytes = 2ULL * 1024 * 1024 * 1024;

/// The chunk size regions are read in -- never one unbounded allocation
/// per region, matching AobScanner's kAobScanChunkBytes convention (a
/// smaller, separate constant here since candidate scanning and AOB
/// scanning are independent, unrelated responsibilities).
inline constexpr std::size_t kCandidateScanChunkBytes = 64 * 1024;

/// Performs the first pass of a candidate search: reads every readable
/// region in `scope`, decodes a candidate of `type` at every address the
/// type's own size naturally aligns to (its alignment policy -- e.g. a
/// U32 candidate only at 4-byte-aligned addresses), and records each
/// one's address and initial value. `outCandidates` is only written to on
/// Success -- left untouched on any failure.
///
/// Regions are read in fixed kCandidateScanChunkBytes chunks, each
/// overlapping the next by `CandidateValueTypeSize(type) - 1` bytes so a
/// candidate straddling a chunk boundary is still found exactly once
/// (same technique as AobScanner's ScanProcessRange) -- never one
/// unbounded per-region allocation.
///
/// Any single chunk read failing aborts the whole scan -- a partial
/// candidate set from some, but not all, regions is never returned as if
/// it were complete.
CandidateScanResult BeginCandidateScan(IProcessReader& reader, IProcessInspector& inspector,
                                        const IProcessMemoryMap& memoryMap, CandidateScanScope scope,
                                        CandidateValueType type, std::vector<Candidate>& outCandidates,
                                        std::size_t maxCandidates = kDefaultMaxCandidates,
                                        std::uint64_t maxScanBytes = kDefaultMaxScanBytes);

/// Which comparison a Filter() call applies between each candidate's
/// stored baseline value and its freshly re-read current value.
enum class CandidateFilterKind {
    /// current != baseline (float: not approximately equal, see the
    /// epsilon/NaN policy below).
    Changed,
    /// current == baseline (float: approximately equal).
    Unchanged,
    /// current > baseline (float: strictly greater, beyond epsilon; NaN
    /// on either side never passes).
    Increased,
    /// current < baseline (float: strictly less, beyond epsilon; NaN on
    /// either side never passes).
    Decreased,
    /// current == a caller-supplied target value (float: approximately
    /// equal to the target).
    ExactValue,
};

/// Absolute epsilon used for every float comparison in this module.
/// Two floats within this distance of each other are treated as equal;
/// a NaN is never considered equal (or ordered relative) to anything,
/// including another NaN -- matching IEEE-754 semantics rather than
/// silently picking an arbitrary tie-break.
inline constexpr float kCandidateFloatEpsilon = 0.0001f;

/// Outcome of FilterCandidates().
enum class CandidateFilterResult {
    Success,
    /// No process attached -- `candidates` is left completely untouched.
    NotAttached,
    /// The attached process is no longer running -- `candidates` is left
    /// completely untouched.
    ProcessExited,
    /// CandidateFilterKind::ExactValue was requested without a target
    /// value -- `candidates` is left completely untouched.
    InvalidTarget,
};

/// Returns a human-readable name for a CandidateFilterResult, e.g. for logging.
const char* ToString(CandidateFilterResult result);

/// Re-reads every candidate's current value and applies `kind`,
/// in-place: a candidate that no longer passes is removed; a surviving
/// candidate's stored value becomes its freshly-read current value (the
/// new baseline for the *next* Filter() call).
///
/// A candidate whose individual re-read fails (a hard read failure or a
/// partial read, while the process remains attached and alive) is simply
/// removed -- it does not fail the whole command, matching this being a
/// per-address condition rather than a process-wide one. If instead the
/// process itself is not attached or has exited, the *entire* command
/// fails and `candidates` is left completely unchanged -- those are
/// process-wide conditions no partial per-candidate handling can paper
/// over.
CandidateFilterResult FilterCandidates(IProcessReader& reader, std::vector<Candidate>& candidates,
                                        CandidateFilterKind kind, const CandidateValue* exactTarget = nullptr);

} // namespace sekiro_haptics::process
