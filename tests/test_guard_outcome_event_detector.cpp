// Unit tests for GuardOutcomeEventDetector -- one event per guard *resolution*,
// so repeated identical outcomes (deflect->deflect, block->block) stay distinct.
//
// These are SYNTHETIC observations. No occurrence field has been confirmed in
// the real game yet (see docs/astra/results/DEFLECT_STATUS.md 6.9), so passing
// these tests says nothing about in-game detection accuracy -- they only pin
// down the boundary behaviour the detector must have once a real field exists.

#include "sekiro_haptics/process/GuardOutcomeEventDetector.hpp"
#include "testing.hpp"

#include <vector>

using namespace sekiro_haptics::process;

namespace {

constexpr std::uint64_t kGen = 7;

struct Harness {
    GuardOutcomeEventDetector detector;
    std::vector<GuardEvent> events;
    std::int64_t t = 0;
    std::uint64_t counter = 100;
    std::uint8_t outcome = 0;
    std::uint64_t generation = kGen;

    explicit Harness(GuardDetectorConfig config = {}) : detector(config) {}

    /// One good observation with the current state, advancing the clock.
    void Tick(std::int64_t stepUs = 5000) {
        t += stepUs;
        GuardObservation obs;
        obs.timestampUs = t;
        obs.readOk = true;
        obs.generation = generation;
        obs.occurrence = counter;
        obs.outcome = outcome;
        Feed(obs);
    }

    /// A guard resolved: the occurrence signal advances and the outcome is set.
    void Resolve(std::uint8_t newOutcome, std::uint64_t step = 1) {
        counter += step;
        outcome = newOutcome;
        Tick();
    }

    void Failure() {
        t += 5000;
        GuardObservation obs;
        obs.timestampUs = t;
        obs.readOk = false;
        Feed(obs);
    }

    void Gap() {
        t += 5000;
        GuardObservation obs;
        obs.timestampUs = t;
        obs.readOk = true;
        obs.continuityBreak = true;
        obs.generation = generation;
        obs.occurrence = counter;
        obs.outcome = outcome;
        Feed(obs);
    }

    void Feed(const GuardObservation& obs) {
        for (const GuardEvent& e : detector.Update(obs)) {
            events.push_back(e);
        }
    }

    void Finish() {
        for (const GuardEvent& e : detector.Finish()) {
            events.push_back(e);
        }
    }

    std::size_t CountOf(GuardEventKind kind) const {
        std::size_t n = 0;
        for (const GuardEvent& e : events) {
            if (e.kind == kind) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace

// --- the repeat cases this whole exercise exists for ----------------------

SH_TEST(GuardOutcome_ThreeDeflectsInARow_ProduceThreeSeparateEvents) {
    Harness h;
    h.Tick(); // baseline
    h.Resolve(1);
    h.Resolve(1);
    h.Resolve(1);
    SH_CHECK(h.events.size() == 3);
    SH_CHECK(h.CountOf(GuardEventKind::Deflect) == 3);
    SH_CHECK(h.CountOf(GuardEventKind::Block) == 0);
}

SH_TEST(GuardOutcome_ThreeBlocksInARow_ProduceThreeSeparateEvents) {
    Harness h;
    h.Tick();
    h.Resolve(0);
    h.Resolve(0);
    h.Resolve(0);
    SH_CHECK(h.events.size() == 3);
    SH_CHECK(h.CountOf(GuardEventKind::Block) == 3);
}

SH_TEST(GuardOutcome_MixedSequence_PreservesKindAndOrder) {
    Harness h;
    h.Tick();
    h.Resolve(1);
    h.Resolve(1);
    h.Resolve(0);
    h.Resolve(0);
    h.Resolve(1);
    SH_CHECK(h.events.size() == 5);
    const GuardEventKind expected[] = {GuardEventKind::Deflect, GuardEventKind::Deflect,
                                       GuardEventKind::Block, GuardEventKind::Block,
                                       GuardEventKind::Deflect};
    for (std::size_t i = 0; i < 5; ++i) {
        SH_CHECK(h.events[i].kind == expected[i]);
    }
}

SH_TEST(GuardOutcome_RapidResolutions_AreNotMergedByProximity) {
    // Two hits 20ms apart must stay two events -- no time-based debounce.
    Harness h;
    h.Tick();
    h.counter += 1;
    h.outcome = 1;
    h.Tick(20000);
    h.counter += 1;
    h.outcome = 1;
    h.Tick(20000);
    SH_CHECK(h.CountOf(GuardEventKind::Deflect) == 2);
}

// --- a held value must never invent events --------------------------------

SH_TEST(GuardOutcome_HeldOutcomeWithoutOccurrenceChange_EmitsNothing) {
    Harness h;
    h.Tick();
    h.Resolve(1);
    SH_CHECK(h.events.size() == 1);
    for (int i = 0; i < 50; ++i) {
        h.Tick(); // outcome stays 1, counter stays put
    }
    SH_CHECK(h.events.size() == 1);
}

SH_TEST(GuardOutcome_IdleAtZero_NeverEmitsABlock) {
    // Standing still / holding guard with no hit: outcome reads 0 forever.
    Harness h;
    for (int i = 0; i < 100; ++i) {
        h.Tick();
    }
    SH_CHECK(h.events.empty());
}

// --- counter discipline ---------------------------------------------------

SH_TEST(GuardOutcome_CounterJump_ReportsOneUnresolvedNotTwoEvents) {
    Harness h;
    h.Tick();
    h.Resolve(1, /*step=*/2);
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Unresolved);
    SH_CHECK(h.events[0].reason == UnresolvedReason::CounterJump);
    SH_CHECK(h.events[0].occurrenceDelta == 2);
    SH_CHECK(h.CountOf(GuardEventKind::Deflect) == 0);
}

SH_TEST(GuardOutcome_CounterWrap_CountsAsOneStepNotAHugeJump) {
    GuardDetectorConfig config;
    config.counterBits = 32;
    Harness h(config);
    h.counter = 0xFFFFFFFFull;
    h.Tick();
    h.counter = 0; // wrapped
    h.outcome = 1;
    h.Tick();
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Deflect);
    SH_CHECK(h.events[0].occurrenceDelta == 1);
}

SH_TEST(GuardOutcome_OutcomeOutsideZeroOne_IsUnresolvedNotADeflect) {
    Harness h;
    h.Tick();
    h.Resolve(7);
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Unresolved);
    SH_CHECK(h.events[0].reason == UnresolvedReason::OutcomeOutOfRange);
}

// --- continuity: attach, object swap, read failure, gap -------------------

SH_TEST(GuardOutcome_FirstObservation_IsBaselineOnly) {
    Harness h;
    h.counter = 5000;
    h.outcome = 1; // already 1 when we attach -- must NOT fire
    h.Tick();
    SH_CHECK(h.events.empty());
    SH_CHECK(h.detector.Stats().baselinesEstablished == 1);
}

SH_TEST(GuardOutcome_ObjectReplacement_DoesNotDiffAcrossInstances) {
    Harness h;
    h.Tick();
    h.Resolve(1);
    SH_CHECK(h.events.size() == 1);
    // New object: unrelated counter value, outcome already 1.
    h.generation = kGen + 1;
    h.counter = 42;
    h.outcome = 1;
    h.Tick();
    SH_CHECK(h.events.size() == 1); // baseline only, no fabricated event
    h.Resolve(0);
    SH_CHECK(h.events.size() == 2);
    SH_CHECK(h.events[1].kind == GuardEventKind::Block);
}

SH_TEST(GuardOutcome_ReadFailure_ThenResume_EmitsNoFabricatedEvent) {
    Harness h;
    h.Tick();
    h.Resolve(1);
    h.Failure();
    h.Failure();
    // Counter moved a lot while we could not see it.
    h.counter += 9;
    h.outcome = 0;
    h.Tick(); // first good read after failure: baseline only
    SH_CHECK(h.events.size() == 1);
    h.Resolve(1);
    SH_CHECK(h.events.size() == 2);
    SH_CHECK(h.events[1].kind == GuardEventKind::Deflect);
}

SH_TEST(GuardOutcome_CaptureGap_BreaksContinuityAndResumesCleanly) {
    Harness h;
    h.Tick();
    h.Resolve(0);
    h.counter += 4;
    h.outcome = 1;
    h.Gap(); // baseline only
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.detector.Stats().continuityBreaks == 1);
    h.Resolve(0);
    SH_CHECK(h.events.size() == 2);
    SH_CHECK(h.events[1].kind == GuardEventKind::Block);
}

// --- lagging outcome ------------------------------------------------------

SH_TEST(GuardOutcome_LaggingOutcome_ClassifiesFromSettledValue) {
    GuardDetectorConfig config;
    config.outcomeSettleUs = 10000;
    Harness h(config);
    h.Tick();
    // Occurrence advances while the outcome still shows the PREVIOUS result.
    h.counter += 1;
    h.outcome = 0;
    h.Tick();
    SH_CHECK(h.events.empty()); // held, not classified as a block
    h.outcome = 1;              // outcome catches up
    h.Tick();
    h.Tick();
    h.Tick();
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Deflect);
    // The event is timestamped at the occurrence, not at the settle.
    SH_CHECK(h.events[0].timestampUs == 10000);
}

SH_TEST(GuardOutcome_NextOccurrenceBeforeSettle_ReportsUnresolvedNotAGuess) {
    GuardDetectorConfig config;
    config.outcomeSettleUs = 50000;
    Harness h(config);
    h.Tick();
    h.Resolve(0);            // pending
    h.Resolve(1);            // second resolution arrives first
    SH_CHECK(h.events.size() >= 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Unresolved);
    SH_CHECK(h.events[0].reason == UnresolvedReason::OutcomeNeverSettled);
}

SH_TEST(GuardOutcome_PendingEventAtStreamEnd_IsUnresolved) {
    GuardDetectorConfig config;
    config.outcomeSettleUs = 50000;
    Harness h(config);
    h.Tick();
    h.Resolve(1);
    h.Finish();
    SH_CHECK(h.events.size() == 1);
    SH_CHECK(h.events[0].kind == GuardEventKind::Unresolved);
}

// --- alternative occurrence shapes ---------------------------------------

SH_TEST(GuardOutcome_PulseMode_OneEventPerRisingEdge) {
    GuardDetectorConfig config;
    config.mode = OccurrenceMode::Pulse;
    Harness h(config);
    h.counter = 0;
    h.Tick();
    for (int i = 0; i < 3; ++i) {
        h.counter = 1;
        h.outcome = 1;
        h.Tick();
        h.Tick(); // still high -- must not re-fire
        h.counter = 0;
        h.Tick();
    }
    SH_CHECK(h.CountOf(GuardEventKind::Deflect) == 3);
}

SH_TEST(GuardOutcome_IdentifierMode_ChangeToNewIdIsOneEvent) {
    GuardDetectorConfig config;
    config.mode = OccurrenceMode::Identifier;
    Harness h(config);
    h.counter = 0x1111;
    h.Tick();
    h.counter = 0x2222;
    h.outcome = 1;
    h.Tick();
    h.Tick(); // same id -- no repeat
    h.counter = 0x3333;
    h.outcome = 0;
    h.Tick();
    SH_CHECK(h.events.size() == 2);
    SH_CHECK(h.events[0].kind == GuardEventKind::Deflect);
    SH_CHECK(h.events[1].kind == GuardEventKind::Block);
}
