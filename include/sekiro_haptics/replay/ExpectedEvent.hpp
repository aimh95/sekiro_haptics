#pragma once

#include <chrono>
#include <string>

namespace sekiro_haptics::replay {

/// A hand-authored ground-truth expectation: "this trace should produce
/// this GameEvent at approximately this time." Deliberately separate from
/// GameSignal/the JSONL trace body -- an ExpectedEvent is a claim about
/// what a detector *should* output, not a raw observation a detector
/// consumes, and mixing the two would let a "fake" event sneak into a
/// trace as if it were a real signal. See ReplayComparator for how these
/// get compared against a detector's actual output, and
/// ExpectedEventRepository for the on-disk JSON schema this is normally
/// loaded from.
struct ExpectedEvent {
    std::string gameId;
    std::string eventId;
    std::chrono::microseconds timestamp{0};
};

} // namespace sekiro_haptics::replay
