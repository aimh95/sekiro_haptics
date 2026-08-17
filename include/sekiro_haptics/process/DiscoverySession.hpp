#pragma once

// Session metadata and watch-capture (JSONL) I/O for SEK-PROBE-001A's
// developer signal-discovery tool. No game meaning of any kind lives
// here -- a "sample" is just a raw value at an address, a "marker" is
// just a user-entered text label. Timestamps recorded here are
// session-local monotonic microseconds, never wall-clock -- see
// docs/05-process-access.md.

#include "sekiro_haptics/process/CandidateScanner.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace sekiro_haptics::process {

inline constexpr int kDiscoverySessionSchemaVersion = 1;

/// Everything recorded once, at the start of a live discovery session.
/// PID and the module base/image-size fields are only valid for *this*
/// specific run (ASLR changes them every attach) -- only
/// `executableFileSizeBytes` + `sha256Hex` are a stable build identity.
/// This struct (and the file it's written to) is never a production
/// SignatureProfile -- nothing here is auto-promoted into one.
struct DiscoverySessionMetadata {
    int schemaVersion = kDiscoverySessionSchemaVersion;
    std::string processImagePath;
    std::uint64_t executableFileSizeBytes = 0;
    /// Lowercase, 64-character hex SHA-256 -- see ExecutableIdentity.hpp.
    std::string sha256Hex;
    std::string mainModuleName;
    /// Valid only for this run -- ASLR-dependent, see the struct comment.
    std::uintptr_t mainModuleBaseAddress = 0;
    std::size_t mainModuleImageSize = 0;
    /// Valid only for this run.
    std::uint32_t pid = 0;
    /// Human-readable wall-clock marker (e.g. ISO-8601) -- informational
    /// only, never used to order or time watch records.
    std::string sessionStartWallClock;
    /// Text form of the scan scope selected for this session (e.g.
    /// "main-module", "private-readable", "all-readable").
    std::string selectedScope;
    /// Text form of the value type selected for this session (see
    /// CandidateScanner.hpp's ToString(CandidateValueType)).
    std::string selectedValueType;
    /// The watch loop's real sleep_for() interval, if a watch was run --
    /// documented explicitly as real wall-clock sleep, never confused
    /// with deterministic replay pacing (see docs/05-process-access.md).
    std::int64_t samplingIntervalMs = 0;
    std::string toolVersion;
    /// Whether the session ended via an explicit, clean stop (quit /
    /// watch stop) as opposed to a forced/unexpected termination.
    bool endedNormally = false;
    /// e.g. "quit", "process_exited", "ctrl_c", "detached".
    std::string endReason;
};

/// Writes `metadata` as a single JSON object to `path` (truncating any
/// existing content). Returns false on a write error (including failing
/// to open `path`).
bool WriteDiscoverySessionMetadata(const std::string& path, const DiscoverySessionMetadata& metadata);

/// Appends "sample"/"marker" JSONL records for one live discovery
/// session's watch capture. Every record carries a caller-supplied
/// session-local monotonic timestamp (microseconds) -- this class never
/// reads the wall clock itself, so record ordering is exactly whatever
/// order the caller writes them in.
class DiscoveryWatchWriter {
public:
    explicit DiscoveryWatchWriter(const std::string& path);
    ~DiscoveryWatchWriter();

    DiscoveryWatchWriter(const DiscoveryWatchWriter&) = delete;
    DiscoveryWatchWriter& operator=(const DiscoveryWatchWriter&) = delete;

    bool IsOpen() const;

    /// Appends one "sample" record: `signalName` is a user-chosen label
    /// (e.g. "candidate/player_hp") -- never inferred or auto-generated
    /// from any game concept. Returns false on a write error.
    bool WriteSample(std::int64_t timestampUs, const std::string& signalName, std::uintptr_t runtimeAddress,
                      CandidateValueType valueType, const CandidateValue& value);

    /// Appends one "marker" record: a user-entered observation label
    /// (e.g. "take_damage") -- a note the user made, never an automatic
    /// event judgment. Returns false on a write error.
    bool WriteMarker(std::int64_t timestampUs, const std::string& label);

    /// Flushes and closes the underlying file early -- also done safely
    /// by the destructor, but callers use this to confirm data reached
    /// disk before reporting success.
    void Close();

private:
    std::ofstream stream_;
};

} // namespace sekiro_haptics::process
