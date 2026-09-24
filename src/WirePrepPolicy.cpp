#include "sekiro_haptics/WirePrepPolicy.hpp"

namespace sekiro_haptics {

void WirePrepPolicy::Reset() {
    armed_ = false;
    suppressed_ = false;
    awaitGroundFall_ = false;
    awaitAirFall_ = false;
}

WirePrepDecision WirePrepPolicy::Update(const WirePrepInput& input) {
    WirePrepDecision decision;
    const bool wasArmed = armed_;

    if (!input.readOk) {
        // Unknown state. Release: a catch that outlives the thing justifying
        // it is the failure this whole feature is built to avoid.
        armed_ = false;
        decision.armed = false;
        decision.reason = "read failed";
        decision.changed = wasArmed;
        return decision;
    }

    // The rule is exactly one line: a target is grapplable -> L2 holds a catch.
    //
    // A launch does NOT release it. That was tried and was wrong twice over:
    //
    //   - In Weapon mode the "it gave way" sensation is the finger crossing
    //     the end zone, not this process cancelling anything. Cancelling adds
    //     nothing a hand can feel and takes the catch away from the NEXT press.
    //   - Suppressing re-arming after a launch broke chained air grapples
    //     outright. Leaving the ground hands availability from the ground flag
    //     to the air flag in the same instant, so any suppression keyed on
    //     their OR never lifted.
    //
    // The GROUND byte is confirmed: looking at a grapple point and away from
    // it toggled it nine times, and it is 0 whenever nothing is grapplable.
    //
    // The AIR byte is not. It was identified in a scene that had a target in
    // view the whole time, so "airborne AND target" and plain "airborne" look
    // identical in it -- and L2 resisting on every ordinary jump says it is
    // the second one. It therefore arms nothing on its own; in the air the
    // catch is only held while a wire action is actually running, which is
    // the chained grapple and is never a plain jump.
    const bool available = input.groundTarget || (input.airTarget && input.inWireAction);
    armed_ = available;
    decision.armed = armed_;
    decision.changed = armed_ != wasArmed;
    // A launch means the trigger was pulled through the catch, so the
    // firmware has latched it released. Rebuild it for the next press.
    decision.reArm = input.launched && armed_;
    if (decision.reArm) {
        decision.reason = "re-arm after firing";
        return decision;
    }
    if (armed_) {
        decision.reason = input.airTarget ? "air target" : "ground target";
    } else {
        decision.reason = "no target";
    }
    return decision;
}

} // namespace sekiro_haptics
