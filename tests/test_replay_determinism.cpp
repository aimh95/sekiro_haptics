// Proves the replay path is deterministic: replaying the same trace
// through the same detector many times in a row always produces the same
// event sequence, in the same order, with the same logical timestamps.
//
// This holds specifically for this fast/synchronous path:
// ReplaySignalSource/ManualLabelEventDetector never call sleep_for() or
// read the wall clock -- they only consume GameSignal::timestamp values
// already present in the trace, at whatever speed the CPU processes them.
// This is NOT a claim about apps/replay_cli's real-time playback mode,
// which paces itself with std::this_thread::sleep_for() between signals
// (computed from trace timestamp deltas, but still an actual OS sleep --
// its wake time is subject to OS scheduling, not deterministic down to the
// microsecond). This test, and RunReplayLoop generally, deliberately never
// exercises that mode.

#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "sekiro_haptics/signals/ReplaySignalSource.hpp"
#include "testing.hpp"

#include <string>
#include <tuple>
#include <vector>

using namespace sekiro_haptics;

namespace {

std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}

using EventSummary = std::tuple<std::string, std::string, std::chrono::microseconds::rep>;

std::vector<EventSummary> ReplayOnce(const std::string& tracePath) {
    ReplaySignalSource source(tracePath);
    ManualLabelEventDetector detector;

    std::vector<EventSummary> summaries;
    GameSignal signal;
    while (source.Next(signal) == SignalSourceResult::Success) {
        std::vector<GameEvent> events;
        detector.OnSignal(signal, events);
        for (const GameEvent& event : events) {
            summaries.emplace_back(event.gameId, event.eventId, event.timestamp.count());
        }
    }

    std::vector<GameEvent> flushed;
    detector.Flush(flushed);
    for (const GameEvent& event : flushed) {
        summaries.emplace_back(event.gameId, event.eventId, event.timestamp.count());
    }

    return summaries;
}

} // namespace

SH_TEST(Replay_SameTraceRunOneHundredTimes_ProducesIdenticalEventSequenceEveryTime) {
    const std::string tracePath = Fixture("perfect_deflect.jsonl");

    std::vector<EventSummary> first = ReplayOnce(tracePath);
    SH_CHECK(!first.empty()); // sanity: the trace actually produces an event

    for (int run = 0; run < 100; ++run) {
        std::vector<EventSummary> repeated = ReplayOnce(tracePath);
        SH_CHECK(repeated == first);
    }
}
