#pragma once

// SEK-PROBE-001D Stage C: offline analysis of a capture file written by
// SekiroCombatCaptureSession -- correlates which memory offsets changed
// near which user markers. Deliberately kept separate from the sampler
// (SekiroCombatCaptureSession never reads its own output back, and this
// class never touches a live process) per the ticket's "sampler와
// 분석기를 분리" requirement.
//
// This is a first-pass correlation view, not the full statistical rigor a
// production Block/PerfectDeflect detector requires (cross-trial
// reproducibility rate, false-positive rate against *other* markers,
// generalization across enemy types -- see docs/07-combat-signal-reader.md's
// "Detector gate" section). It exists to help a human eyeball candidate
// offsets, nothing more -- never treat its output as a validated signal.

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// How many times delta records at `offset` fell within the analysis
/// window of an occurrence of `label`, out of `markerOccurrences` total
/// occurrences of that label in the file. A ratio near 1.0
/// (changeCount / markerOccurrences) means "this offset reliably changes
/// whenever this marker was recorded" -- promising, but not sufficient on
/// its own (see the header comment above: still needs a false-positive
/// check against *other* markers before it's trustworthy).
struct CombatCaptureOffsetMarkerStat {
    std::size_t offset = 0;
    std::string label;
    std::uint64_t changeCount = 0;
    std::uint64_t markerOccurrences = 0;
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

    /// One entry per (offset, label) pair with at least one correlated
    /// change, sorted by descending changeCount.
    std::vector<CombatCaptureOffsetMarkerStat> offsetMarkerStats;
};

/// Reads the capture JSONL file at `path` and, for every marker occurrence,
/// counts delta records at each offset whose timestamp falls within
/// `windowUs` microseconds either side of that marker. Pure file analysis
/// -- never opens `path` for writing, never touches a live process.
CombatCaptureAnalysisReport AnalyzeCombatCaptureFile(const std::string& path, std::int64_t windowUs);

} // namespace sekiro_haptics::process
