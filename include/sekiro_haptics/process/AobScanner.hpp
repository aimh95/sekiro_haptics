#pragma once

// Byte-pattern (AOB) scanning: a pure in-memory buffer scanner, and a
// bounded IProcessReader-based range scanner built on top of it. Part of
// SEK-READ-001C -- see docs/05-process-access.md for the chunk/overlap
// policy and the unique-match principle this module enforces throughout.

#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"

#include <cstddef>
#include <cstdint>

namespace sekiro_haptics::process {

/// The chunk size ScanProcessRange() reads a process range in -- a range
/// is never read/allocated in one unbounded piece. Exposed here (not just
/// as an implementation-private constant) so tests -- including the real
/// Win32 integration test's helper process -- can deliberately place data
/// straddling a chunk boundary.
inline constexpr std::size_t kAobScanChunkBytes = 64 * 1024;

/// Searches `buffer` (of `bufferSize` bytes) for `pattern`, requiring
/// exactly one match. Pure function -- no process access, no allocation
/// beyond local scan state.
///
/// - `pattern.size() > bufferSize` is AobScanResult::NoMatch (the pattern
///   simply cannot fit, not a malformed-input error).
/// - Every possible start offset in `buffer` is checked, including
///   overlapping candidate matches -- the scan never stops at the first
///   match found, so a second (possibly overlapping) match is still
///   detected and reported as MultipleMatches rather than silently
///   returning the first one.
/// - Zero matches is NoMatch; more than one is MultipleMatches; neither
///   ever writes to `outOffset`.
/// - `pattern.bytes.empty()` (should never happen for a pattern produced
///   by ParseAobPattern(), but checked defensively) is InvalidPattern.
AobScanResult ScanBuffer(const std::uint8_t* buffer, std::size_t bufferSize, const AobPattern& pattern,
                          std::size_t& outOffset);

/// Searches the attached process's memory in `[rangeBase, rangeBase +
/// rangeSize)` -- a range the caller has already validated is readable
/// (e.g. a module's own base/size from IProcessInspector::GetMainModule())
/// -- for `pattern`, requiring exactly one match. This module never
/// assumes an entire module is readable on its own; it only ever scans
/// the exact range it's given.
///
/// Reads the range in fixed kAobScanChunkBytes-sized chunks rather than
/// one unbounded allocation, each chunk overlapping the next by
/// `pattern.size() - 1` bytes so a match straddling a chunk boundary is
/// still found; a match is only counted once, attributed to the chunk
/// whose own (non-overlap) portion it starts in.
///
/// Fails closed:
/// - `pattern.bytes.empty()`: InvalidPattern.
/// - `rangeBase == 0`, `rangeSize == 0`, or `rangeBase + rangeSize`
///   overflowing the address space: InvalidRange.
/// - `pattern.size() > rangeSize`: NoMatch.
/// - No process attached: NotAttached.
/// - Any chunk read failing (NotAttached / ProcessExited / ReadFailed /
///   PartialRead, mapped 1:1 from the underlying ProcessReaderResult)
///   aborts the *entire* scan immediately -- any matches already found in
///   earlier, successfully-read chunks are discarded, never partially
///   reported.
/// - The process's liveness is re-checked once more after the full range
///   has been read, before declaring success -- a process that exited in
///   the instant after the last chunk read but before this function
///   returns is still caught.
/// - Zero matches: NoMatch. More than one: MultipleMatches. Neither ever
///   writes to `outAddress`.
/// - The single match's address (and its full pattern length) is
///   re-verified to fall completely within `[rangeBase, rangeBase +
///   rangeSize)` before being reported -- InvalidRange if that
///   re-verification somehow fails.
///
/// Never caches a result between calls -- every call is a fresh scan.
AobScanResult ScanProcessRange(IProcessReader& reader, std::uintptr_t rangeBase, std::size_t rangeSize,
                                const AobPattern& pattern, std::uintptr_t& outAddress);

} // namespace sekiro_haptics::process
