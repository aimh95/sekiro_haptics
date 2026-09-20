#pragma once

// SEK-PROBE-001D Stage C: offline, trial-centric analysis of a capture file
// written by SekiroCombatCaptureSession -- correlates which memory offsets
// changed around which labeled combat outcomes. Deliberately kept separate
// from the sampler (SekiroCombatCaptureSession never reads its own output
// back, and this class never touches a live process) per the ticket's
// "sampler와 분석기를 분리" requirement.
//
// A "trial" is one guard-button press (an auto-recorded "guard_input"
// marker -- see RunControllerGuardWatchThread() in
// apps/sekiro_signal_probe/main.cpp) paired with whatever outcome label
// (normal_block/perfect_deflect/take_damage/...) the human recorded shortly
// after it. Each guard_input is consumed by at most one outcome label (the
// unique unclaimed candidate within `guardInputLookbackUs`). Overlapping
// analysis windows and trials crossing recorded gaps are excluded separately.
// This legacy analyzer accepts v1/v2 only. Use tools/deflect_capture.py for v3.
//
// SEK-PROBE-001E Section 7: an outcome marker with *no* unclaimed guard_input
// candidate in range is an orphan (death/respawn/idle/loading/rest, or a
// guard_input the human simply never got around to labeling) and an outcome
// marker with *more than one* unclaimed candidate in range is ambiguous (we
// cannot tell which press it belongs to) -- both are excluded from scoring
// entirely rather than guessed at (see `orphanMarkersExcluded`/
// `ambiguousMarkersExcluded` below). A guard_input that no outcome marker
// ever claims still becomes its own "guard_input"-labeled trial.
//
// This is a first-pass correlation view, not the full statistical rigor a
// production Block/PerfectDeflect detector requires (generalization across
// enemy types, independent replay) -- see docs/07-combat-signal-reader.md's
// "Detector gate" section. It exists to help a human eyeball candidate
// offsets, nothing more -- never treat its output as a validated signal.

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// One bit that differed between a trial's before/after value at some offset.
struct CombatCaptureBitFlip {
    int bitIndex = 0; // 0-31
    bool becameOne = false; // true: 0->1, false: 1->0
};

/// Per-(offset, label) statistics computed over every trial. `SupportRate()`
/// is how often this offset actually changed during a trial of this label;
/// `FalsePositiveRate()` is how often it *also* changed during trials of
/// every *other* label -- a real candidate signal needs high support and
/// low false-positive rate, not just a high raw change count (see the
/// header comment above and this ticket's Section 9/10).
struct CombatCaptureOffsetTrialStat {
    std::size_t offset = 0;
    std::string label;

    std::uint64_t trialsForLabel = 0;
    std::uint64_t trialsChangedForLabel = 0;
    std::uint64_t trialsForOtherLabels = 0;
    std::uint64_t trialsChangedForOtherLabels = 0;

    /// One representative example: the first trial (of this label) where
    /// this offset changed within the analysis window.
    std::uint32_t exampleBeforeU32 = 0;
    std::uint32_t exampleAfterU32 = 0;
    /// Time from the trial's anchor (the guard_input, if one was linked) to
    /// this offset's first change within the window, in microseconds.
    std::int64_t exampleFirstChangeUs = 0;
    /// How long the "after" value persisted before the next change to this
    /// offset anywhere in the file, in microseconds -- -1 if it never
    /// changed again before the capture ended (still holding).
    std::int64_t exampleDurationHeldUs = -1;
    std::vector<CombatCaptureBitFlip> exampleBitFlips;

    double SupportRate() const {
        return trialsForLabel > 0 ? static_cast<double>(trialsChangedForLabel) / static_cast<double>(trialsForLabel)
                                   : 0.0;
    }
    double FalsePositiveRate() const {
        return trialsForOtherLabels > 0
                   ? static_cast<double>(trialsChangedForOtherLabels) / static_cast<double>(trialsForOtherLabels)
                   : 0.0;
    }
};

struct CombatCaptureAnalysisReport {
    bool ok = false;
    /// Set only when ok == false (file missing, unreadable, or a
    /// malformed line -- the whole file is rejected rather than silently
    /// analyzing a partial/corrupt capture).
    std::string error;

    std::uint64_t totalDeltaRecords = 0;
    std::uint64_t totalMarkers = 0;
    std::uint64_t totalDiscontinuities = 0;
    std::uint64_t totalDropped = 0;
    /// Total trials constructed (guard_input+label pairs, plus any
    /// unclaimed guard_input-only trials) -- see the header comment above.
    std::uint64_t totalTrials = 0;
    /// Outcome markers excluded from scoring because no unclaimed
    /// guard_input candidate existed within the lookback window.
    std::uint64_t orphanMarkersExcluded = 0;
    /// Outcome markers excluded from scoring because more than one
    /// unclaimed guard_input candidate existed within the lookback window
    /// (which one it belongs to is genuinely unknown).
    std::uint64_t ambiguousMarkersExcluded = 0;
    std::uint64_t overlappingTrialsExcluded = 0;
    std::uint64_t discontinuousTrialsExcluded = 0;

    /// One entry per (offset, label) with at least one trial where that
    /// offset changed, sorted by descending SupportRate() then ascending
    /// FalsePositiveRate() -- the most promising candidates (changes often
    /// for this label, rarely for others) sort first.
    std::vector<CombatCaptureOffsetTrialStat> offsetStats;
};

/// Default guard_input-to-outcome-marker lookback (SEK-PROBE-001E Section 7:
/// "default 1s, configurable" -- narrowed from the original 3s constant now
/// that ambiguous/orphan markers are excluded rather than guessed at, so a
/// smaller default window produces fewer ambiguous matches).
constexpr std::int64_t kDefaultGuardInputLookbackUs = 1'000'000;

/// Reads the capture JSONL file at `path`, builds the trial list described
/// above (linking guard_input markers to outcome markers within
/// `guardInputLookbackUs` of each other), and computes per-(offset, label)
/// support/false-positive statistics over a `windowUs`-microsecond window
/// starting at each trial's anchor. Pure file analysis -- never opens `path`
/// for writing, never touches a live process.
CombatCaptureAnalysisReport AnalyzeCombatCaptureFile(const std::string& path, std::int64_t windowUs,
                                                      std::int64_t guardInputLookbackUs = kDefaultGuardInputLookbackUs);

} // namespace sekiro_haptics::process
