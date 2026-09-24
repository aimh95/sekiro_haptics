// Start/end rules for wire actions.
//
// Synthetic: these drive the detector with hand-made observations. They say
// nothing about whether the game was read correctly -- that part is the live
// capture in docs/astra/results/live/wire_scene*.jsonl.

#include "sekiro_haptics/process/SekiroWireActionReader.hpp"
#include "testing.hpp"

using namespace sekiro_haptics::process;

namespace {

constexpr std::uint64_t kAnchorA = 0x20B0007500000023ull;
constexpr std::uint64_t kAnchorB = 0x20B000A700000002ull;

WireObservation Obs(std::int64_t t, bool inAction, std::uint64_t handle = kAnchorA,
                    std::uint64_t generation = 1, bool readOk = true, bool breakContinuity = false) {
    WireObservation o;
    o.timestampUs = t;
    o.readOk = readOk;
    o.generation = generation;
    o.inWireAction = inAction;
    o.handle = inAction ? handle : 0xFFFFFFFFFFFFFFFFull;
    o.continuityBreak = breakContinuity;
    return o;
}

} // namespace

SH_TEST(Wire_OneActionProducesOneStartAndOneEndWithItsDuration) {
    WireActionEventDetector detector;
    SH_CHECK(detector.Update(Obs(0, false)).empty());          // baseline
    auto started = detector.Update(Obs(1000, true));
    SH_CHECK(started.size() == 1);
    SH_CHECK(started[0].kind == WireEventKind::Started);
    SH_CHECK(started[0].handle == kAnchorA);

    SH_CHECK(detector.Update(Obs(1500, true)).empty());        // still running
    auto ended = detector.Update(Obs(3000, false));
    SH_CHECK(ended.size() == 1);
    SH_CHECK(ended[0].kind == WireEventKind::Ended);
    SH_CHECK(ended[0].durationUs == 2000);
    SH_CHECK(!detector.InAction());
}

SH_TEST(Wire_AnActionAlreadyRunningAtTheFirstLookIsNotReportedAsStarted) {
    // We never saw it start, so saying it started would be inventing the
    // moment.
    WireActionEventDetector detector;
    SH_CHECK(detector.Update(Obs(0, true)).empty());
    SH_CHECK(detector.InAction());
    auto ended = detector.Update(Obs(500, false));
    SH_CHECK(ended.size() == 1);
    SH_CHECK(ended[0].kind == WireEventKind::Ended);
}

SH_TEST(Wire_TwoActionsBackToBackAreTwoEventsNotOneLongOne) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    detector.Update(Obs(100, true, kAnchorA));
    auto swapped = detector.Update(Obs(900, true, kAnchorB));
    // The handle changed without passing through idle.
    SH_CHECK(swapped.size() == 2);
    SH_CHECK(swapped[0].kind == WireEventKind::Ended);
    SH_CHECK(swapped[0].handle == kAnchorA);
    SH_CHECK(swapped[1].kind == WireEventKind::Started);
    SH_CHECK(swapped[1].handle == kAnchorB);
}

SH_TEST(Wire_RepeatedActionsOnTheSameAnchorAreStillSeparateEvents) {
    // A fixed cooldown would merge these; nothing here has one.
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    std::size_t starts = 0;
    for (int i = 0; i < 4; ++i) {
        const std::int64_t base = 1000 + i * 3000;
        for (const auto& event : detector.Update(Obs(base, true, kAnchorA)))
            if (event.kind == WireEventKind::Started) ++starts;
        detector.Update(Obs(base + 1500, false));
    }
    SH_CHECK(starts == 4);
}

SH_TEST(Wire_AReadFailureEndsTheActionAsUnobservedNotAsCompleted) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    detector.Update(Obs(100, true));
    auto lost = detector.Update(Obs(600, false, kAnchorA, 1, /*readOk=*/false));
    SH_CHECK(lost.size() == 1);
    SH_CHECK(lost[0].kind == WireEventKind::EndedUnobserved);
    SH_CHECK(lost[0].durationUs == 0);   // no duration is claimed
    SH_CHECK(!detector.InAction());
}

SH_TEST(Wire_ACaptureGapEndsTheActionAsUnobserved) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    detector.Update(Obs(100, true));
    auto gap = detector.Update(Obs(5000, true, kAnchorA, 1, true, /*breakContinuity=*/true));
    SH_CHECK(gap.size() == 1);
    SH_CHECK(gap[0].kind == WireEventKind::EndedUnobserved);
}

SH_TEST(Wire_ObjectReplacementDoesNotDiffAcrossInstances) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false, kAnchorA, /*generation=*/1));
    detector.Update(Obs(100, true, kAnchorA, 1));
    // A load or a death: the module instance changed.
    auto swapped = detector.Update(Obs(400, false, kAnchorA, /*generation=*/2));
    SH_CHECK(swapped.size() == 1);
    SH_CHECK(swapped[0].kind == WireEventKind::EndedUnobserved);

    // And the new generation baselines rather than emitting.
    SH_CHECK(detector.Update(Obs(500, true, kAnchorB, 2)).size() == 1);
}

SH_TEST(Wire_AfterAGapTheNextActionIsStillReported) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    detector.Update(Obs(100, true));
    detector.Update(Obs(600, false, kAnchorA, 1, /*readOk=*/false));
    // Re-baseline, then a real new action.
    detector.Update(Obs(1000, false));
    auto started = detector.Update(Obs(1500, true, kAnchorB));
    SH_CHECK(started.size() == 1);
    SH_CHECK(started[0].kind == WireEventKind::Started);
}

SH_TEST(Wire_FinishClosesAnOpenActionAsUnobserved) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    detector.Update(Obs(100, true));
    auto closing = detector.Finish(900);
    SH_CHECK(closing.size() == 1);
    SH_CHECK(closing[0].kind == WireEventKind::EndedUnobserved);
    SH_CHECK(detector.Finish(1000).empty());   // idempotent
}

SH_TEST(Wire_IdleNeverProducesAnything) {
    WireActionEventDetector detector;
    detector.Update(Obs(0, false));
    for (int i = 1; i < 50; ++i) SH_CHECK(detector.Update(Obs(i * 5000, false)).empty());
}
