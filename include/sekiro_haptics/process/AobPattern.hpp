#pragma once

// Strict AOB (array-of-bytes) pattern parser. Pure text -> byte/mask data,
// no scanning, no process access -- see AobScanner.hpp for what consumes
// the parsed AobPattern. Part of SEK-READ-001C; see
// docs/05-process-access.md for the full pattern grammar and how this
// fits together with the scanner and RIP-relative resolver.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Outcome of a pattern-parse, buffer/process scan, or RIP-relative
/// resolve operation -- one shared enum across all three (AobPattern.hpp,
/// AobScanner.hpp, RipRelative.hpp), matching this project's existing
/// TransportResult/ProcessReaderResult/ProcessInspectionResult convention.
/// Any single function only produces a subset of these; see each
/// function's doc comment for which.
enum class AobScanResult {
    Success,
    /// ParseAobPattern(): the pattern text itself is malformed (see
    /// ParseAobPattern's doc comment for every rejected form).
    InvalidPattern,
    /// A scan range or RIP-relative source/target range was zero-based,
    /// zero-sized, overflowed the address space, or a computed result
    /// address didn't fully fit inside its declared range.
    InvalidRange,
    /// A scan (buffer or process range) found zero matches.
    NoMatch,
    /// A scan found more than one match -- never picked arbitrarily.
    MultipleMatches,
    /// ScanProcessRange()/ResolveRipRelativeFromProcess(): no process attached.
    NotAttached,
    /// The attached process is no longer running.
    ProcessExited,
    /// The underlying read call itself failed for a still-running process.
    ReadFailed,
    /// The read call reported success but returned fewer bytes than
    /// requested. Never treated as Success.
    PartialRead,
    /// A computed address (instruction start, displacement location, or
    /// RIP-relative target) overflowed or underflowed the address space.
    AddressOverflow,
    /// ResolveRipRelativeAddress(): the declared displacement location
    /// doesn't lie fully inside the declared instruction bounds.
    InvalidDisplacementLayout,
    /// The computed RIP-relative target address is zero, or falls outside
    /// the caller-supplied allowed target range -- never guessed as valid.
    TargetOutOfRange,
};

/// Returns a human-readable name for an AobScanResult, e.g. for logging.
const char* ToString(AobScanResult result);

/// The maximum number of bytes ParseAobPattern() accepts in a single
/// pattern -- a deliberately generous but finite bound (real AOB
/// signatures are typically well under 100 bytes) so a pathologically long
/// pattern string is rejected rather than silently accepted.
inline constexpr std::size_t kMaxAobPatternBytes = 256;

/// A parsed AOB pattern: byte values and their exact/wildcard mask, kept
/// as two parallel arrays of the same length (mask[i] == true means
/// bytes[i] must match exactly at that position; mask[i] == false means
/// any byte value is accepted and bytes[i] is meaningless/unset).
struct AobPattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;

    std::size_t size() const { return bytes.size(); }
};

/// Parses `text` as a whitespace-separated sequence of AOB tokens into
/// `outPattern`. Grammar per token:
/// - Exact byte: exactly two hex digits, case-insensitive (`"48"`, `"8b"`).
/// - Wildcard: `"?"` or `"??"`.
///
/// Rejected (AobScanResult::InvalidPattern), `outPattern` left untouched:
/// - An empty pattern (no tokens at all).
/// - Malformed hex in a two-character token (e.g. `"4G"`, `"$$"`).
/// - A single hex digit alone (e.g. `"4"`) -- exact bytes must be exactly
///   two digits; only wildcards may be a single character.
/// - A partial wildcard mixing one hex digit and one `'?'` (e.g. `"4?"`,
///   `"?F"`).
/// - Any token three characters or longer.
/// - A pattern made entirely of wildcards (no exact byte at all) -- such a
///   pattern carries no anchor to search for and is rejected rather than
///   silently matching everywhere.
/// - A pattern longer than kMaxAobPatternBytes bytes.
AobScanResult ParseAobPattern(const std::string& text, AobPattern& outPattern);

} // namespace sekiro_haptics::process
