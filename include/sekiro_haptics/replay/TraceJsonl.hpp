#pragma once

#include "sekiro_haptics/signals/GameSignal.hpp"

#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>

namespace sekiro_haptics::trace {

/// Version of the on-disk JSONL trace schema this reader/writer implements.
/// Bump this and document the change in docs/03-trace-format.md if the
/// required-field set or ordering policy below ever changes.
inline constexpr int kSchemaVersion = 1;

/// Outcome of a single TraceReader::ReadNext() call.
enum class TraceReadResult {
    Success,
    EndOfTrace,
    MalformedJson,
    MissingTimestamp,
    MissingSignal,
    OutOfOrderTimestamp,
    InvalidSignalValidity,
    IoError,
};

/// Returns a human-readable name for a TraceReadResult, e.g. for logging.
const char* ToString(TraceReadResult result);

/// Reads a newline-delimited JSON (JSONL) trace file, one GameSignal per
/// non-blank line. See docs/03-trace-format.md for the full schema.
///
/// Required fields per line: `timestampUs` (JSON number, microseconds) and
/// `signal` (non-empty JSON string). `value` is optional (defaults to an
/// empty string if absent) and is stringified regardless of its JSON type.
/// Any other JSON key on the line is preserved into GameSignal::extra,
/// stringified, so a trace author can attach ad hoc fields a detector may
/// or may not look at.
///
/// Ordering policy: `timestampUs` must be non-decreasing from one non-blank
/// line to the next (ties are allowed). A decrease is reported as
/// OutOfOrderTimestamp rather than silently reordering the trace.
///
/// Signal validity: an optional `validity` field distinguishes a real
/// reading from a signal source that couldn't observe this value right
/// now -- important so "couldn't read this" is never confused with "read
/// this as 0/false". If present, it must be exactly one of `"valid"`
/// (the default when the field is absent), `"unavailable"`, or
/// `"disabled"`; any other value is InvalidSignalValidity. The value ends
/// up in `GameSignal::extra["validity"]` verbatim (only when the field was
/// present at all) -- consumers that care (see ManualLabelEventDetector)
/// check for it there rather than this struct growing a dedicated field.
class TraceReader {
public:
    explicit TraceReader(const std::string& path);

    /// Whether the file was successfully opened. Does not change as
    /// ReadNext() consumes the file (it reflects the initial open, and
    /// Rewind()'s re-open).
    bool IsOpen() const;

    /// Reads the next non-blank line. On any non-Success, non-EndOfTrace
    /// result, `*outError` (if non-null) is set to "line N: <reason>".
    /// A malformed or policy-violating line still advances the read
    /// position -- the caller decides whether to keep reading.
    TraceReadResult ReadNext(GameSignal& outSignal, std::string* outError = nullptr);

    /// Re-opens the trace file and resets the line counter and ordering
    /// state, so the next ReadNext() call starts from the first line
    /// again. Returns false if the file could not be re-opened.
    bool Rewind();

    /// 1-based line number of the line most recently returned by
    /// ReadNext() (whatever the result), or 0 before the first call.
    std::size_t LineNumber() const;

private:
    std::string path_;
    std::ifstream stream_;
    bool opened_ = false;
    std::size_t lineNumber_ = 0;
    bool haveLastTimestamp_ = false;
    std::chrono::microseconds lastTimestamp_{0};
};

/// Writes GameSignal instances as JSONL, one per line, in the same schema
/// TraceReader reads. Intended for tests (round-tripping) and for a future
/// tool that records a live trace.
class TraceWriter {
public:
    explicit TraceWriter(const std::string& path);

    bool IsOpen() const;

    /// Appends one JSONL line for `signal`. Returns false on a write error.
    bool WriteSignal(const GameSignal& signal);

private:
    std::ofstream stream_;
};

} // namespace sekiro_haptics::trace
