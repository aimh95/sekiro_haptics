#pragma once

#include "sekiro_haptics/events/GameEvent.hpp"
#include "sekiro_haptics/replay/ExpectedEvent.hpp"

#include <chrono>
#include <vector>

namespace sekiro_haptics::replay {

/// Result of CompareEvents(): how a detector's actual output measured up
/// against a hand-authored set of expectations for the same replay.
struct ReplayComparisonResult {
    std::size_t expectedCount = 0;
    std::size_t detectedCount = 0;
    std::size_t matchedCount = 0;

    /// Expected events with no matching actual event.
    std::vector<ExpectedEvent> missed;
    /// Actual events matching no (remaining) expectation.
    std::vector<GameEvent> falsePositives;
    /// Actual events that would otherwise match an expectation already
    /// claimed by an earlier actual event.
    std::vector<GameEvent> duplicates;
    /// actual.timestamp - expected.timestamp for each matched pair, in
    /// match order. Never negative -- see CompareEvents' matching rule.
    std::vector<std::chrono::microseconds> latencies;
};

/// Compares `actual` (a detector's real output) against `expected` (hand
/// authored ground truth) for the same replay run.
///
/// Matching rule: an actual event matches an expected one if `gameId` and
/// `eventId` are equal AND `actual.timestamp` falls in
/// [`expected.timestamp`, `expected.timestamp + maxLatency`] --
/// `actual.timestamp` must not precede `expected.timestamp` (a detector
/// reacting before the event it claims to detect has even happened is a
/// correctness bug, not just latency). Expected events are matched in the
/// order `actual` is given, against expectations in the order `expected`
/// is given; each expected event can be matched at most once. Any further
/// actual event that would otherwise match an already-matched expectation
/// is recorded in `duplicates`, not counted as a second match. An actual
/// event matching no expectation's window at all is a false positive.
ReplayComparisonResult CompareEvents(const std::vector<ExpectedEvent>& expected, const std::vector<GameEvent>& actual,
                                      std::chrono::microseconds maxLatency);

} // namespace sekiro_haptics::replay
