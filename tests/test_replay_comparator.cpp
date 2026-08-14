// Unit tests for ReplayComparator's matching logic. Pure hand-built
// ExpectedEvent/GameEvent vectors -- no trace files, no detector, no
// hardware.

#include "sekiro_haptics/replay/ReplayComparator.hpp"
#include "testing.hpp"

#include <chrono>

using namespace sekiro_haptics;
using namespace sekiro_haptics::replay;
using namespace std::chrono_literals;

namespace {

ExpectedEvent Expected(const std::string& eventId, std::chrono::microseconds timestamp) {
    ExpectedEvent event;
    event.gameId = "sekiro";
    event.eventId = eventId;
    event.timestamp = timestamp;
    return event;
}

GameEvent Actual(const std::string& eventId, std::chrono::microseconds timestamp) {
    GameEvent event;
    event.gameId = "sekiro";
    event.eventId = eventId;
    event.timestamp = timestamp;
    return event;
}

} // namespace

SH_TEST(CompareEvents_ExactMatch_CountsAsMatchedWithZeroLatency) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {Actual("combat.perfect_deflect", 1000us)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 1);
    SH_CHECK(result.missed.empty());
    SH_CHECK(result.falsePositives.empty());
    SH_CHECK(result.duplicates.empty());
    SH_CHECK(result.latencies.size() == 1);
    SH_CHECK(result.latencies[0].count() == 0);
}

SH_TEST(CompareEvents_ActualWithinMaxLatency_MatchesWithPositiveLatency) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {Actual("combat.perfect_deflect", 1030us)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50us);

    SH_CHECK(result.matchedCount == 1);
    SH_CHECK(result.latencies[0].count() == 30);
}

SH_TEST(CompareEvents_ExpectedEventNeverDetected_IsMissed) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 0);
    SH_CHECK(result.missed.size() == 1);
    SH_CHECK(result.missed[0].eventId == "combat.perfect_deflect");
}

SH_TEST(CompareEvents_ActualWithNoExpectation_IsFalsePositive) {
    std::vector<ExpectedEvent> expected = {};
    std::vector<GameEvent> actual = {Actual("combat.perfect_deflect", 1000us)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 0);
    SH_CHECK(result.falsePositives.size() == 1);
}

SH_TEST(CompareEvents_ActualBeforeExpectedTimestamp_IsFalsePositiveNotAMatch) {
    // A detector reacting before the event it claims to detect has even
    // happened is a correctness bug, not "negative latency."
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {Actual("combat.perfect_deflect", 900us)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 0);
    SH_CHECK(result.falsePositives.size() == 1);
    SH_CHECK(result.missed.size() == 1);
}

SH_TEST(CompareEvents_ActualBeyondMaxLatency_IsFalsePositive) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {Actual("combat.perfect_deflect", 1000us + 100ms)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 0);
    SH_CHECK(result.falsePositives.size() == 1);
}

SH_TEST(CompareEvents_SecondActualMatchingClaimedExpectation_IsDuplicate) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {
        Actual("combat.perfect_deflect", 1000us),
        Actual("combat.perfect_deflect", 1010us),
    };

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 1);
    SH_CHECK(result.duplicates.size() == 1);
    SH_CHECK(result.falsePositives.empty());
    SH_CHECK(result.missed.empty());
}

SH_TEST(CompareEvents_MismatchedEventId_DoesNotMatch) {
    std::vector<ExpectedEvent> expected = {Expected("combat.perfect_deflect", 1000us)};
    std::vector<GameEvent> actual = {Actual("combat.take_damage", 1000us)};

    ReplayComparisonResult result = CompareEvents(expected, actual, 50ms);

    SH_CHECK(result.matchedCount == 0);
    SH_CHECK(result.falsePositives.size() == 1);
    SH_CHECK(result.missed.size() == 1);
}
