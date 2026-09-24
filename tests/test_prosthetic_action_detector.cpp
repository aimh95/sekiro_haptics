// Turning the action-request code into "the player used this prosthetic".
//
// The sequences below are the ones actually recorded on 2026-09-24 (see
// SekiroProstheticActionReader.hpp). Each test names the mistake it exists to
// prevent.

#include "sekiro_haptics/process/SekiroProstheticActionReader.hpp"
#include "testing.hpp"

#include <vector>

using namespace sekiro_haptics::process;

namespace {

struct Run {
    ProstheticActionDetector detector;
    std::int64_t now = 0;
    std::vector<ProstheticActionEvent> all;

    std::vector<ProstheticActionEvent> Feed(std::uint32_t code, bool ok = true,
                                            std::uint64_t generation = 1) {
        now += 5'000;
        auto got = detector.Update({now, ok, generation, code});
        all.insert(all.end(), got.begin(), got.end());
        return got;
    }
    int Attacks() const {
        int n = 0;
        for (const auto& e : all) n += e.phase == ProstheticPhase::Attack ? 1 : 0;
        return n;
    }
};

} // namespace

SH_TEST(ProstheticAction_FamilyNamesTheTool) {
    SH_CHECK(ToolForActionCode(70400100) == ProstheticTool::Shuriken);
    SH_CHECK(ToolForActionCode(73400100) == ProstheticTool::Axe);
    SH_CHECK(ToolForActionCode(78400100) == ProstheticTool::Spear);
    // The sword is its own family and never a prosthetic.
    SH_CHECK(!ToolForActionCode(50300010).has_value());
    // Idle values are far below any family.
    SH_CHECK(!ToolForActionCode(790010).has_value());
    SH_CHECK(!ToolForActionCode(0).has_value());
    // A family nobody has observed is unknown, not "probably a tool".
    SH_CHECK(!ToolForActionCode(71400100).has_value());
}

SH_TEST(ProstheticAction_PhaseComesFromTheLastFiveDigits) {
    SH_CHECK(PhaseForActionCode(70400000) == ProstheticPhase::Ready);
    SH_CHECK(PhaseForActionCode(70400010) == ProstheticPhase::Ready);
    SH_CHECK(PhaseForActionCode(70400100) == ProstheticPhase::Attack);
    SH_CHECK(PhaseForActionCode(70400120) == ProstheticPhase::Attack);
    SH_CHECK(PhaseForActionCode(70400900) == ProstheticPhase::None);   // R2 held
    SH_CHECK(PhaseForActionCode(78412000) == ProstheticPhase::None);   // seen while swapping
}

SH_TEST(ProstheticAction_OneUseIsOneAttack_OnTheAttackCodeNotTheReady) {
    // THE RULE. "..400000 then ..400100" is preparation then attack. Firing
    // on the first would put the axe's impact before its swing.
    Run r;
    r.Feed(790010);                                  // baseline
    const auto ready = r.Feed(70400000);
    SH_CHECK(ready.size() == 1 && ready[0].phase == ProstheticPhase::Ready);
    r.Feed(790010);
    const auto attack = r.Feed(70400100);
    SH_CHECK(attack.size() == 1);
    SH_CHECK(attack[0].phase == ProstheticPhase::Attack);
    SH_CHECK(attack[0].tool == ProstheticTool::Shuriken);
    SH_CHECK(r.Attacks() == 1);
}

SH_TEST(ProstheticAction_ARepeatedAttackPulseIsStillOneAttack) {
    // Recorded: the same attack code twice, 10 ms apart, with the idle value
    // between. One swing, requested twice.
    Run r;
    r.Feed(790040);
    r.Feed(70400000);
    r.Feed(790040);
    r.Feed(70400110);
    r.Feed(790040);
    r.Feed(70400110);
    r.Feed(790040);
    SH_CHECK(r.Attacks() == 1);
}

SH_TEST(ProstheticAction_RealRepeatsAreNotMergedByTime) {
    // Two uses in quick succession, each with its own ready. No timer
    // decides they are "the same" -- the ready between them says they are not.
    Run r;
    r.Feed(790010);
    for (int use = 0; use < 3; ++use) {
        r.Feed(70400000);
        r.Feed(790010);
        r.Feed(70400100);
        r.Feed(790010);
    }
    SH_CHECK(r.Attacks() == 3);
}

SH_TEST(ProstheticAction_AChainStepWithANewCodeCounts) {
    // Chained throws step the code (110 -> 120) without a ready between.
    Run r;
    r.Feed(790040);
    r.Feed(70400010);
    r.Feed(70400110);
    r.Feed(790040);
    r.Feed(70400120);
    r.Feed(790040);
    r.Feed(70400120);                                // re-request of the same step
    SH_CHECK(r.Attacks() == 2);
}

SH_TEST(ProstheticAction_HoldingR2IsNotAUse) {
    // The player's own rule: R2 input is not use. It shows as 70400900,
    // held for seconds.
    Run r;
    r.Feed(790010);
    for (int i = 0; i < 50; ++i) r.Feed(70400900);
    r.Feed(790010);
    SH_CHECK(r.Attacks() == 0);
}

SH_TEST(ProstheticAction_TheSwordNeverFires) {
    Run r;
    r.Feed(790040);
    for (std::uint32_t code : {50300010u, 790040u, 50300110u, 790040u, 50300020u, 790040u})
        r.Feed(code);
    SH_CHECK(r.all.empty());
}

SH_TEST(ProstheticAction_TheToolSwapIsNotAUse) {
    Run r;
    r.Feed(790010);
    r.Feed(412090);
    r.Feed(790010);
    r.Feed(78412000);
    r.Feed(790010);
    SH_CHECK(r.Attacks() == 0);
}

SH_TEST(ProstheticAction_TheSpearAndAxeReportThemselves) {
    Run r;
    r.Feed(790010);
    r.Feed(78400000);
    const auto spear = r.Feed(78400100);
    SH_CHECK(spear.size() == 1 && spear[0].tool == ProstheticTool::Spear);
    r.Feed(790010);
    r.Feed(73400000);
    // The axe's ready is held long -- many polls of the same code. That is
    // still one ready, and nothing fires until the attack code appears.
    for (int i = 0; i < 80; ++i) r.Feed(73400000);
    const auto axe = r.Feed(73400100);
    SH_CHECK(axe.size() == 1 && axe[0].tool == ProstheticTool::Axe);
    SH_CHECK(r.Attacks() == 2);
}

SH_TEST(ProstheticAction_TheFirstObservationIsOnlyABaseline) {
    // Starting the app mid-throw must not report the throw that was already
    // on screen.
    Run r;
    SH_CHECK(r.Feed(70400100).empty());
    SH_CHECK(r.Attacks() == 0);
}

SH_TEST(ProstheticAction_AReadFailureRebaselinesInsteadOfFabricating) {
    Run r;
    r.Feed(790010);
    r.Feed(70400000);
    r.Feed(0, /*ok=*/false);
    // The attack code is showing when the read comes back. It was not
    // observed to START, so it is not reported.
    SH_CHECK(r.Feed(70400100).empty());
    SH_CHECK(r.Attacks() == 0);
}

SH_TEST(ProstheticAction_ALoadDoesNotLookLikeAUse) {
    // A new object after an area transition is compared with nothing.
    Run r;
    r.Feed(790010, true, 1);
    SH_CHECK(r.Feed(70400100, true, 2).empty());
    SH_CHECK(r.Attacks() == 0);
}
