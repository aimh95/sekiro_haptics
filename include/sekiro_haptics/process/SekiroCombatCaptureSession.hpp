#pragma once

// Bounded-window memory capture around an
// already-resolved combat object (e.g. PlayerGameData), plus user markers --
// the raw material Block/PerfectDeflect candidate offsets get found in
// later (see SekiroCombatCaptureAnalyzer.hpp), never a detector itself.
// Never scans for anything and never touches more than one bounded region:
// the region's live base address/generation is supplied by the caller
// every Tick() (normally SekiroRawCombatReader's already-resolved
// PlayerGameData address), so this class has no AOB/resolver dependency of
// its own. See docs/07-combat-signal-reader.md.
//
// Schema v3 stores a full baseline initially and after every discontinuity
// or read gap, then 4-byte deltas and a sample commit record on EVERY
// successful tick (including unchanged samples). A reader can reconstruct
// exact raw bytes without interpreting them as verified game fields.
//
// Sampling V2 (SEK-PROBE-001E Section 6): this class never reads a clock
// itself -- every timestamp is caller-supplied, so it's fully deterministic
// in tests. Ticks are scheduled on a fixed grid (nextScheduledUs_ always
// advances by whole multiples of the configured interval, never by
// "however late the last call happened to be") so a caller that falls
// behind never bursts through several catch-up reads in a row -- it just
// skips the missed slots and reports them as dropped/late.

#include "sekiro_haptics/process/IProcessReader.hpp"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Which known object a capture is scoped to. PlayerGameData is the
/// AOB-resolved player object (see SekiroRawCombatReader.hpp).
/// CustomAddress is any runtime address the caller already has evidence
/// for -- the window starts *at* that address and extends forward, never
/// resolved or re-scanned by this class itself.
enum class CombatCaptureScope {
    PlayerGameData,
    CustomAddress,
};
const char* ToString(CombatCaptureScope scope);

/// Hard per-scope byte caps -- a caller-requested window is silently
/// clamped down to the active scope's own cap, then rounded down to a
/// multiple of kCombatCaptureCellSizeBytes.
inline constexpr std::size_t kPlayerGameDataMaxCaptureBytes = 0xA10;
inline constexpr std::size_t kCustomAddressMaxCaptureBytes = 64 * 1024;

/// Cells are diffed 4 bytes at a time (matching the i32 field size every
/// known hypothesis in this project uses) rather than per-byte -- a single
/// multi-byte field changing produces one delta record, not up to four.
inline constexpr std::size_t kCombatCaptureCellSizeBytes = 4;

inline constexpr std::chrono::milliseconds kDefaultCombatCaptureIntervalMs{5};
inline constexpr std::chrono::milliseconds kMinCombatCaptureIntervalMs{5};
inline constexpr std::chrono::milliseconds kMaxCombatCaptureIntervalMs{200};

struct CombatCaptureConfig {
    CombatCaptureScope scope = CombatCaptureScope::PlayerGameData;
    /// Clamped down to the scope's own max, then rounded down to a
    /// multiple of kCombatCaptureCellSizeBytes -- see EffectiveWindowSizeBytes().
    std::size_t requestedWindowSizeBytes = kPlayerGameDataMaxCaptureBytes;
    /// Clamped into [kMinCombatCaptureIntervalMs, kMaxCombatCaptureIntervalMs].
    std::chrono::milliseconds samplingInterval = kDefaultCombatCaptureIntervalMs;
    /// CombatCaptureScope::CustomAddress only: the fixed runtime address the
    /// capture window starts at. Ignored for every other scope.
    std::uintptr_t customBaseAddress = 0;
    /// How late (relative to the scheduled slot) a sample may arrive before
    /// counting as "late" -- see CombatCaptureStats::lateSamples. Defaults
    /// to one full interval (i.e. "missed at least one whole cycle"), never
    /// a handful of microseconds of jitter -- see the ticket's own
    /// "단순히 예정 시각보다 몇 ns 늦었다는 이유로 late 처리하지 마라" rule.
    std::int64_t lateToleranceUs = -1; // -1 sentinel: Start() defaults this to one interval
};

enum class CombatCaptureStartResult {
    Started,
    AlreadyRunning,
    /// The clamped/rounded window size came out to 0.
    InvalidConfig,
    OpenOutputFailed,
    OutputExists,
    WriteOutputFailed,
    /// The very first (baseline) read failed -- nothing was opened/started.
    InitialReadFailed,
};
const char* ToString(CombatCaptureStartResult result);

struct CombatCaptureStats {
    std::uint64_t samplesTaken = 0;
    std::uint64_t deltaRecordsWritten = 0;
    std::uint64_t markersWritten = 0;
    /// Mark() calls rejected because the session wasn't running (before
    /// Start() or after Stop()) -- never silently dropped without a count.
    std::uint64_t markersRejectedNotRunning = 0;
    /// A sample whose lateness (actual arrival vs. its scheduled grid slot)
    /// was at or past `lateToleranceUs` -- i.e.
    /// `(nowMonotonicUs - scheduledSlotUs) >= lateToleranceUs`. With the
    /// default `lateToleranceUs` (exactly one configured interval), this is
    /// precisely "missed at least one whole cycle" (equivalent to
    /// `missedCycles >= 1` for that tick), never merely "a few microseconds
    /// of scheduler jitter." The comparison is `>=`, not `>` -- a sample
    /// arriving at exactly one interval late has, by definition, already
    /// missed that one whole cycle.
    std::uint64_t lateSamples = 0;
    /// Scheduled cycles skipped outright because the caller's Tick() calls
    /// fell behind (see the class comment on never bursting catch-up
    /// reads) -- a slot for which no read was even attempted. Distinct from
    /// `readFailedSamples`/`unresolvedSamples` below, which are slots that
    /// *were* attempted but didn't yield a real sample.
    std::uint64_t missedScheduleSamples = 0;
    /// Tick() attempted a process read (a normal tick, or the re-baseline
    /// read taken right after a generation change) and the reader reported
    /// failure.
    std::uint64_t readFailedSamples = 0;
    /// Tick() was called with `regionBaseAddress == 0` -- "not currently
    /// resolved," never treated as a real all-zero read.
    std::uint64_t unresolvedSamples = 0;
    /// `missedScheduleSamples + readFailedSamples + unresolvedSamples` --
    /// every sample that should have happened but didn't, for any reason.
    std::uint64_t droppedSamplesTotal = 0;
    /// A Tick() whose `regionGeneration` differed from the previous one --
    /// the diff baseline was reset rather than diffed across instances.
    std::uint64_t discontinuities = 0;
    std::uint64_t inconsistentSamples = 0;
    std::uint64_t baselineRecordsWritten = 0;
    std::uint64_t sampleRecordsWritten = 0;
    bool outputFailed = false;

    std::int64_t configuredIntervalUs = 0;
    std::int64_t lateToleranceUs = 0;

    /// Distribution of actual accepted-sample-to-sample gaps, in
    /// microseconds. All zero if fewer than 2 samples were taken.
    std::int64_t minIntervalUs = 0;
    std::int64_t p50IntervalUs = 0;
    std::int64_t p95IntervalUs = 0;
    std::int64_t p99IntervalUs = 0;
    std::int64_t maxIntervalUs = 0;

    /// Distribution of lateness (actual arrival time minus that sample's
    /// scheduled grid slot), in microseconds -- distinct from the interval
    /// stats above: this measures drift against the fixed schedule, not
    /// just gap-to-gap variance. All zero if no samples were taken.
    std::int64_t p50LatenessUs = 0;
    std::int64_t p95LatenessUs = 0;
    std::int64_t maxLatenessUs = 0;
};

/// Owns one capture-to-file session. Not copyable (holds an open file and a
/// reference to the process reader). Not internally thread-safe -- a caller
/// driving Tick() from one thread and Mark() from another (see
/// SekiroCombatSessionController, which does exactly this) must serialize
/// access itself.
class SekiroCombatCaptureSession {
public:
    using ValidateRegion = std::function<bool(std::uintptr_t, std::uint64_t)>;
    explicit SekiroCombatCaptureSession(IProcessReader& reader, ValidateRegion validateRegion = {});
    ~SekiroCombatCaptureSession();

    SekiroCombatCaptureSession(const SekiroCombatCaptureSession&) = delete;
    SekiroCombatCaptureSession& operator=(const SekiroCombatCaptureSession&) = delete;

    /// Atomically creates `outputPath`, refusing every existing path, and
    /// stores the initial baseline. Fails without opening anything if `regionBaseAddress
    /// == 0` or the read fails. The first scheduled sample slot is set to
    /// `nowMonotonicUs + interval`.
    CombatCaptureStartResult Start(const CombatCaptureConfig& config, std::uintptr_t regionBaseAddress,
                                    std::uint64_t regionGeneration, const std::string& outputPath,
                                    std::int64_t nowMonotonicUs);

    /// One read+diff+write cycle, driven entirely by caller-supplied time
    /// (never sleeps or reads a clock itself). A call arriving before the
    /// next scheduled slot is a silent no-op (not counted in any stat). A
    /// call arriving after one or more slots have passed advances the
    /// schedule grid past all of them at once (see the class comment on
    /// never bursting catch-up reads) and counts the skipped slots as
    /// `missedScheduleSamples`/`droppedSamplesTotal`. `regionBaseAddress ==
    /// 0` means "not currently resolved" and is recorded as one
    /// `unresolvedSamples`/`droppedSamplesTotal` sample, never treated as a
    /// real all-zero read. Returns false if stopped or output fails.
    bool Tick(std::uintptr_t regionBaseAddress, std::uint64_t regionGeneration, std::int64_t nowMonotonicUs);

    /// Records a marker. `inputTimestampUs` is the precise moment the
    /// underlying input happened (e.g. a controller button press, captured
    /// at the source thread -- see RunControllerGuardWatchThread() in
    /// main.cpp) and is what every timing/trial computation uses;
    /// `processedTimestampUs` is when this call actually executed, recorded
    /// alongside it purely for diagnosing queue/processing lag, never used
    /// for correlation. Returns false (and counts
    /// CombatCaptureStats::markersRejectedNotRunning) if the session isn't
    /// running.
    bool Mark(const std::string& label, std::int64_t inputTimestampUs, std::int64_t processedTimestampUs);

    /// Flushes and closes the output file. Safe to call when not running.
    bool Stop();

    bool IsRunning() const { return running_; }
    bool IsDue(std::int64_t nowUs) const { return running_ && nowUs >= nextScheduledUs_; }
    /// Computes the min/p50/p95/p99/max interval and p50/p95/max lateness
    /// fields fresh from every accepted sample recorded so far -- not free
    /// for very long sessions (sorts copies of the whole history) but this
    /// is a manual dev tool, never called per-sample.
    CombatCaptureStats Stats() const;
    std::size_t EffectiveWindowSizeBytes() const { return windowSizeBytes_; }
    std::chrono::milliseconds SamplingInterval() const { return samplingInterval_; }

private:
    void WriteDeltaRecord(std::uint64_t sequence, std::int64_t scheduledUs, std::int64_t actualUs,
                           std::uint64_t generation, std::size_t offset, const std::uint8_t* previousCell,
                           const std::uint8_t* currentCell);
    void WriteMarkerRecord(std::int64_t inputTimestampUs, std::int64_t processedTimestampUs, const std::string& label);
    void WriteDiscontinuityRecord(std::int64_t actualUs, std::uint64_t oldGeneration, std::uint64_t newGeneration);
    void WriteDroppedRecord(std::int64_t actualUs, const std::string& reason);
    void WriteBaselineRecord(std::int64_t timestampUs, const char* reason);
    void WriteSampleRecord(std::uint64_t sequence, std::int64_t scheduledUs, std::int64_t actualUs,
                           const char* status);
    bool FlushRecords();

    IProcessReader& reader_;
    ValidateRegion validateRegion_;
    std::FILE* file_ = nullptr;
    std::ostringstream stream_;
    bool running_ = false;
    std::size_t windowSizeBytes_ = 0;
    std::chrono::milliseconds samplingInterval_{kDefaultCombatCaptureIntervalMs};
    std::int64_t lateToleranceUs_ = 0;
    std::uint64_t lastGeneration_ = 0;
    std::uintptr_t lastBaseAddress_ = 0;
    std::vector<std::uint8_t> previousBytes_;
    bool baselineValid_ = false;

    std::uint64_t nextSequence_ = 0;
    std::int64_t nextScheduledUs_ = 0;
    std::int64_t lastAcceptedActualUs_ = 0;
    bool hasLastSample_ = false;

    CombatCaptureStats stats_;
    /// Every accepted sample's actual gap since the previous one, in
    /// microseconds -- the raw material Stats() sorts to report interval
    /// percentiles.
    std::vector<std::int64_t> sampleIntervalsUs_;
    /// Every accepted sample's lateness (actual - scheduled), in
    /// microseconds -- the raw material Stats() sorts to report lateness
    /// percentiles.
    std::vector<std::int64_t> sampleLatenessUs_;
};

} // namespace sekiro_haptics::process
