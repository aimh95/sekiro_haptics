#pragma once

// When should L2 be holding a catch, ready for the NEXT grapple?
//
// "Re-arming" here means rebuilding the catch so the next press has something
// to give way against. It is a STATE that lasts as long as a target is worth
// grappling to -- not a short buzz fired off when something happens.
//
// WHY THIS IS ITS OWN TYPE
// ------------------------
// The rule lives here, away from the polling loop, because each of its clauses
// exists to stop a specific failure that was observed on the hardware, and
// each one deserves a test that fails if it is removed.
//
// THE THREE MEASUREMENTS BEHIND IT
// --------------------------------
// THE JUMP BUG (2026-09-24)
// -------------------------
// The rule used to be `groundTarget || airTarget`. The player reported L2
// resisting on every ordinary jump. The air byte was identified in a scene
// that had a grapple point in view throughout, so it could never distinguish
// "airborne AND target" from plain "airborne" -- and the symptom says it is
// the latter.
//
// So the air byte no longer arms anything by itself. In the air it only
// counts while a wire action is actually in progress, which is the chained
// grapple this feature was built for and is never true of a plain jump.
//
//  1. The target flags do NOT fall at launch. They stay set for ~550 ms into
//     the flight (8380 prep on, 9082 launch, 9630 flag down; the same 550-580
//     ms on every run in that session). So "arm whenever a target is
//     available" keeps pushing back through the first half of the flight,
//     which is felt as a late release.
//
//  2. A level test on "a wire action is in progress" cannot be used to gate
//     re-arming: chained grapples end and start in the SAME tick (10679 ms
//     showed `ended` and `started` together), so that flag never returns to
//     false between links and the catch would never come back.
//
//  3. The ground flag and the air flag are in exact anti-phase -- jumping in
//     place while aimed at an anchor flipped both at identical timestamps, 26
//     times, always in opposite directions. Leaving the ground therefore hands
//     "target available" from one flag to the other WITHOUT their OR ever
//     reaching zero.
//
// (3) is what broke chained AIR grapples: a suppression that waited for the
// OR to fall never cleared, because it never fell. So the suppression is
// tracked PER FLAG -- it lifts once each flag that was set at the launch has
// gone down, which the ground->air handover satisfies immediately.

#include <cstdint>

namespace sekiro_haptics {

/// One tick of what the reader saw.
struct WirePrepInput {
    /// False when the read failed. An unknown state releases; it never holds.
    bool readOk = false;
    bool groundTarget = false;
    bool airTarget = false;
    /// True only on the tick a wire action was observed to START.
    bool launched = false;
    /// A wire action is in progress right now.
    ///
    /// Needed because the AIR target byte turned out not to be trustworthy on
    /// its own (see SekiroWireActionReader.hpp). Being in a wire action is the
    /// corroboration that separates "airborne mid-chain, another grapple is
    /// coming" from "airborne because the player jumped".
    bool inWireAction = false;
};

/// What L2 should be doing, and why -- the reason is logged, never guessed at.
struct WirePrepDecision {
    bool armed = false;
    /// True on the tick this changed, so the caller only writes on a change.
    bool changed = false;
    /// The catch must be REBUILT even though `armed` did not change.
    ///
    /// Weapon mode latches once the trigger is pulled through its end zone:
    /// the effect is still applied, the firmware simply stops giving force,
    /// and letting go does not bring it back. Observed directly -- after one
    /// grapple the catch never returned even with the effect untouched and
    /// the finger fully released. Re-sending the identical effect is not
    /// enough either, because an unchanged report is skipped; the caller has
    /// to clear the side and set it again.
    bool reArm = false;
    const char* reason = "idle";
};

class WirePrepPolicy {
public:
    WirePrepDecision Update(const WirePrepInput& input);

    bool Armed() const { return armed_; }
    /// Exposed for logging: is a launch still holding re-arming off?
    bool Suppressed() const { return suppressed_; }

    /// Forget everything (device loss, session end). Leaves the policy
    /// disarmed, so nothing is remembered as applied that is not.
    void Reset();

private:
    bool armed_ = false;
    /// Set by a launch, lifted once every flag that was set at that launch has
    /// fallen at least once. Per flag, not on their OR -- see the header.
    bool suppressed_ = false;
    bool awaitGroundFall_ = false;
    bool awaitAirFall_ = false;
};

} // namespace sekiro_haptics
