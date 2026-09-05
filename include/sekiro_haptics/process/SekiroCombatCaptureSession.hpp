#pragma once

// SEK-PROBE-001D Stage C: bounded-window, delta-only memory capture around
// an already-resolved combat object (e.g. PlayerGameData), plus user
// markers -- the raw material Block/PerfectDeflect candidate offsets get
// found in later (see SekiroCombatCaptureAnalyzer.hpp), never a detector
// itself. Never scans for anything and never touches more than one bounded
// region: the region's live base address/generation is supplied by the
// caller every Tick() (normally SekiroRawCombatReader's already-resolved
// PlayerGameData address), so this class has no AOB/resolver dependency of
// its own. See docs/07-combat-signal-reader.md.
//
// "Delta-only" is the whole point: the full raw byte block is never written
// to disk on every sample (see Section 7's ticket text) -- only which
// 4-byte cells changed, their before/after bytes, the timestamp, and the
// region's generation. A full raw block, if ever wanted for deep manual
// inspection, is a separate, explicit opt-in this class does not provide.

#include "sekiro_haptics/process/IProcessReader.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Which known object a capture is scoped to. Only PlayerGameData is wired
/// up so far -- PlayerInfo/validated action-state/current-target objects
/// aren't resolved by anything yet (see SekiroKnownRootResolver.hpp's
/// current root set), and a user-specified address window is future work.
enum class CombatCaptureScope {
    PlayerGameData,
};
const char* ToString(CombatCaptureScope scope);

/// Hard per-scope byte cap (see the ticket's Section 7) -- a
/// caller-requested window is silently clamped down to this, then rounded
/// down to a multiple of kCombatCaptureCellSizeBytes.
inline constexpr std::size_t kPlayerGameDataMaxCaptureBytes = 0xA10;

/// Cells are diffed 4 bytes at a time (matching the i32 field size every
/// known hypothesis in this project uses) rather than per-byte -- a single
/// multi-byte field changing produces one delta record, not up to four.
inline constexpr std::size_t kCombatCaptureCellSizeBytes = 4;

inline constexpr std::chrono::milliseconds kDefaultCombatCaptureIntervalMs{10};
inline constexpr std::chrono::milliseconds kMinCombatCaptureIntervalMs{5};
inline constexpr std::chrono::milliseconds kMaxCombatCaptureIntervalMs{200};

struct CombatCaptureConfig {
    CombatCaptureScope scope = CombatCaptureScope::PlayerGameData;
    /// Clamped down to the scope's own max, then rounded down to a
    /// multiple of kCombatCaptureCellSizeBytes -- see EffectiveWindowSizeBytes().
    std::size_t requestedWindowSizeBytes = kPlayerGameDataMaxCaptureBytes;
    /// Clamped into [kMinCombatCaptureIntervalMs, kMaxCombatCaptureIntervalMs].
    std::chrono::milliseconds samplingInterval = kDefaultCombatCaptureIntervalMs;
};

enum class CombatCaptureStartResult {
    Started,
    AlreadyRunning,
    /// The clamped/rounded window size came out to 0.
    InvalidConfig,
    OpenOutputFailed,
    /// The very first (baseline) read failed -- nothing was opened/started.
    InitialReadFailed,
};
const char* ToString(CombatCaptureStartResult result);

struct CombatCaptureStats {
    std::uint64_t samplesTaken = 0;
    std::uint64_t deltaRecordsWritten = 0;
    std::uint64_t markersWritten = 0;
    /// A Tick() whose caller-supplied `nowMonotonicUs` arrived more than
    /// 2x the configured sampling interval after the previous sample --
    /// the caller's own loop fell behind, not a process-read problem.
    std::uint64_t lateSamples = 0;
    /// A Tick() where the region was unavailable (address 0) or the read
    /// itself failed -- no delta record was possible for that tick.
    std::uint64_t droppedSamples = 0;
    /// A Tick() whose `regionGeneration` differed from the previous one --
    /// the diff baseline was reset rather than diffed across instances.
    std::uint64_t discontinuities = 0;
};

/// Owns one capture-to-file session. Not copyable (holds an open file and a
/// reference to the process reader).
class SekiroCombatCaptureSession {
public:
    explicit SekiroCombatCaptureSession(IProcessReader& reader);
    ~SekiroCombatCaptureSession();

    SekiroCombatCaptureSession(const SekiroCombatCaptureSession&) = delete;
    SekiroCombatCaptureSession& operator=(const SekiroCombatCaptureSession&) = delete;

    /// Opens `outputPath` (truncating any existing content) and takes the
    /// initial baseline read at `regionBaseAddress` -- that first read is
    /// never itself written as a delta record (there is nothing to diff it
    /// against yet). Fails without opening anything if `regionBaseAddress
    /// == 0` or the read fails.
    CombatCaptureStartResult Start(const CombatCaptureConfig& config, std::uintptr_t regionBaseAddress,
                                    std::uint64_t regionGeneration, const std::string& outputPath,
                                    std::int64_t nowMonotonicUs);

    /// One read+diff+write cycle, driven entirely by caller-supplied time
    /// (never sleeps or reads a clock itself -- see kDefaultCombatCaptureIntervalMs).
    /// A call arriving before the configured interval has elapsed since the
    /// last sample is a silent no-op (not counted in any stat).
    /// `regionBaseAddress == 0` means "not currently resolved" and is
    /// recorded as a dropped sample, never treated as a real all-zero read.
    /// Returns false only if the session isn't running.
    bool Tick(std::uintptr_t regionBaseAddress, std::uint64_t regionGeneration, std::int64_t nowMonotonicUs);

    /// Records a marker at the current moment. Returns false if the
    /// session isn't running or the write fails.
    bool Mark(const std::string& label, std::int64_t nowMonotonicUs);

    /// Flushes and closes the output file. Safe to call when not running.
    void Stop();

    bool IsRunning() const { return running_; }
    CombatCaptureStats Stats() const { return stats_; }
    std::size_t EffectiveWindowSizeBytes() const { return windowSizeBytes_; }
    std::chrono::milliseconds SamplingInterval() const { return samplingInterval_; }

private:
    void WriteDeltaRecord(std::int64_t timestampUs, std::uint64_t generation, std::size_t offset,
                           const std::uint8_t* previousCell, const std::uint8_t* currentCell);
    void WriteMarkerRecord(std::int64_t timestampUs, const std::string& label);
    void WriteDiscontinuityRecord(std::int64_t timestampUs, std::uint64_t oldGeneration, std::uint64_t newGeneration);
    void WriteDroppedRecord(std::int64_t timestampUs, const std::string& reason);

    IProcessReader& reader_;
    std::ofstream stream_;
    bool running_ = false;
    std::size_t windowSizeBytes_ = 0;
    std::chrono::milliseconds samplingInterval_{kDefaultCombatCaptureIntervalMs};
    std::uint64_t lastGeneration_ = 0;
    std::vector<std::uint8_t> previousBytes_;
    std::int64_t lastSampleMonotonicUs_ = 0;
    bool hasLastSample_ = false;
    CombatCaptureStats stats_;
};

} // namespace sekiro_haptics::process
