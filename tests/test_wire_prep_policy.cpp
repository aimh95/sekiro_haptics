// When L2 should be holding a catch for the NEXT grapple.
//
// Synthetic: these drive the rule with hand-made ticks. Each test names the
// hardware failure it exists to prevent.

#include "sekiro_haptics/WirePrepPolicy.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;

namespace {

WirePrepInput Tick(bool ground, bool air, bool launched = false, bool readOk = true,
                   bool inWireAction = false) {
    WirePrepInput in;
    in.readOk = readOk;
    in.groundTarget = ground;
    in.airTarget = air;
    in.launched = launched;
    in.inWireAction = inWireAction;
    return in;
}

} // namespace

SH_TEST(WirePrep_ArmsWhileAGroundTargetIsAvailable) {
    WirePrepPolicy policy;
    SH_CHECK(!policy.Update(Tick(false, false)).armed);
    const auto on = policy.Update(Tick(true, false));
    SH_CHECK(on.armed);
    SH_CHECK(on.changed);
    SH_CHECK(std::string(on.reason) == "ground target");
    SH_CHECK(policy.Armed());
}

SH_TEST(WirePrep_FiringDoesNotTakeTheCatchAway) {
    // THE RULE.
    //
    // In Weapon mode the "it gave way" sensation is the finger crossing the
    // end zone, not this process cancelling the effect. Cancelling on launch
    // adds nothing a hand can feel and removes the catch the NEXT press needs.
    // So as long as something is still grapplable, the catch stays.
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    SH_CHECK(policy.Armed());

    const auto fired = policy.Update(Tick(true, false, /*launched=*/true));
    SH_CHECK(fired.armed);
    SH_CHECK(!fired.changed);       // nothing to re-send
    SH_CHECK(policy.Armed());
}

SH_TEST(WirePrep_ChainedAirGrapplesKeepTheCatchThroughout) {
    // Leaving the ground hands availability from the ground flag to the air
    // flag in the same instant, so their OR never dips. Any rule that waited
    // for that OR to fall left an airborne chain with no catch at all.
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));                       // grounded, armed
    policy.Update(Tick(true, false, /*launched=*/true));    // launch 1
    SH_CHECK(policy.Armed());

    // 연속 그래플: 공중이고 **와이어 액션 중**이다.
    const auto handover = policy.Update(Tick(false, true, false, true, true));
    SH_CHECK(handover.armed);
    SH_CHECK(!handover.changed);                            // stayed armed
    SH_CHECK(std::string(policy.Armed() ? "armed" : "off") == "armed");

    policy.Update(Tick(false, true, true, true, true));     // launch 2, in the air
    SH_CHECK(policy.Armed());
    policy.Update(Tick(false, true, true, true, true));     // launch 3
    SH_CHECK(policy.Armed());
}

SH_TEST(WirePrep_NoTargetMeansNoCatch) {
    WirePrepPolicy policy;
    for (int i = 0; i < 10; ++i) SH_CHECK(!policy.Update(Tick(false, false)).armed);
}

SH_TEST(WirePrep_LosingTheTargetReleases) {
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    const auto lost = policy.Update(Tick(false, false));
    SH_CHECK(!lost.armed);
    SH_CHECK(lost.changed);
    SH_CHECK(std::string(lost.reason) == "no target");
}

SH_TEST(WirePrep_JumpingWithNoTargetNeverArms) {
    // Ordinary jumps and falls must not produce a catch: both flags stay 0.
    WirePrepPolicy policy;
    for (int i = 0; i < 20; ++i) {
        SH_CHECK(!policy.Update(Tick(false, false)).armed);
        SH_CHECK(!policy.Update(Tick(false, false, /*launched=*/false)).armed);
    }
}

SH_TEST(WirePrep_AFailedReadReleasesRatherThanHolding) {
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    SH_CHECK(policy.Armed());

    const auto lost = policy.Update(Tick(true, false, false, /*readOk=*/false));
    SH_CHECK(!lost.armed);
    SH_CHECK(lost.changed);
    SH_CHECK(std::string(lost.reason) == "read failed");

    // And it comes back once the read does.
    SH_CHECK(policy.Update(Tick(true, false)).armed);
}

SH_TEST(WirePrep_ResetLeavesItDisarmed) {
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    SH_CHECK(policy.Armed());
    policy.Reset();
    SH_CHECK(!policy.Armed());
}

SH_TEST(WirePrep_AnUnchangedStateNeverAsksForAnotherWrite) {
    // Re-applying the same effect every tick would churn HID writes and
    // replace the live effect several times a second.
    WirePrepPolicy policy;
    SH_CHECK(policy.Update(Tick(true, false)).changed);
    for (int i = 0; i < 50; ++i) SH_CHECK(!policy.Update(Tick(true, false)).changed);
    SH_CHECK(policy.Update(Tick(false, false)).changed);
    for (int i = 0; i < 50; ++i) SH_CHECK(!policy.Update(Tick(false, false)).changed);
}

SH_TEST(WirePrep_GroundToAirSwapDoesNotReportAChange) {
    // Both are "a target is available"; swapping which one is set must not
    // cause a pointless cancel/re-apply pair on the device.
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    SH_CHECK(!policy.Update(Tick(false, true, false, true, true)).changed);
    SH_CHECK(policy.Armed());
}

SH_TEST(WirePrep_FiringAsksForTheCatchToBeRebuilt) {
    // Weapon mode latches released once the trigger is pulled through its end
    // zone: the effect is still applied, the firmware just stops pushing back,
    // and letting go does not bring it back. Observed on the device -- after
    // one grapple the catch never returned. So a launch has to ask for a
    // rebuild even though `armed` is unchanged.
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    SH_CHECK(policy.Armed());

    const auto fired = policy.Update(Tick(true, false, /*launched=*/true));
    SH_CHECK(fired.armed);
    SH_CHECK(fired.reArm);
    SH_CHECK(std::string(fired.reason) == "re-arm after firing");
    SH_CHECK(policy.Armed());

    // And the tick after is quiet again -- one rebuild per launch.
    const auto after = policy.Update(Tick(true, false));
    SH_CHECK(!after.reArm);
    SH_CHECK(!after.changed);
}

SH_TEST(WirePrep_AChainRebuildsTheCatchOnEveryLaunch) {
    WirePrepPolicy policy;
    policy.Update(Tick(true, false));
    int rebuilds = 0;
    if (policy.Update(Tick(true, false, true)).reArm) ++rebuilds;   // ground launch
    if (policy.Update(Tick(false, true, true, true, true)).reArm) ++rebuilds;  // air launch
    if (policy.Update(Tick(false, true, true, true, true)).reArm) ++rebuilds;  // air launch
    SH_CHECK(rebuilds == 3);
    SH_CHECK(policy.Armed());
}

SH_TEST(WirePrep_FiringWithNoTargetDoesNotAskForARebuild) {
    // Nothing to rebuild if there is no catch to begin with.
    WirePrepPolicy policy;
    const auto fired = policy.Update(Tick(false, false, /*launched=*/true));
    SH_CHECK(!fired.armed);
    SH_CHECK(!fired.reArm);
}


SH_TEST(WirePrep_AnOrdinaryJumpDoesNotArm) {
    // THE BUG THIS EXISTS FOR (2026-09-24).
    //
    // The player reported L2 resisting on every jump. The air byte had been
    // identified in a scene with a grapple point in view throughout, so it
    // could never tell "airborne AND target" from plain "airborne" -- and the
    // symptom says it is the second. So the air byte arms nothing alone.
    WirePrepPolicy policy;
    for (int i = 0; i < 30; ++i) {
        const auto jumping = policy.Update(Tick(false, /*air=*/true, false, true,
                                                /*inWireAction=*/false));
        SH_CHECK(!jumping.armed);
    }
    SH_CHECK(!policy.Armed());
}

SH_TEST(WirePrep_AirborneArmsOnlyWhileAWireActionIsRunning) {
    // The chained grapple still works: the corroboration is that a wire action
    // is actually in progress, which a jump never has.
    WirePrepPolicy policy;
    SH_CHECK(!policy.Update(Tick(false, true, false, true, false)).armed);
    SH_CHECK(policy.Update(Tick(false, true, false, true, true)).armed);
    SH_CHECK(!policy.Update(Tick(false, true, false, true, false)).armed);
}

SH_TEST(WirePrep_GroundTargetStillArmsOnItsOwn) {
    // The ground byte IS confirmed -- nine on/off spans following the
    // indicator -- so nothing extra is demanded of it.
    WirePrepPolicy policy;
    SH_CHECK(policy.Update(Tick(true, false, false, true, false)).armed);
}
