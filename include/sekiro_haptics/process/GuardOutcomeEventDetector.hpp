#pragma once

// Turns a stream of raw player-guard observations into one event per *guard
// resolution*, so that deflect->deflect and block->block are preserved as two
// separate events instead of collapsing into one.
//
// WHY THIS EXISTS
// ---------------
// The confirmed outcome byte (SprjChrActionFlagModule +0xE10, see
// docs/astra/results/DEFLECT_STATUS.md section 6) answers "what was the last
// guard result" -- 0 = normal block, 1 = Just Guard / deflect. It HOLDS that
// value until the next guard resolves, so watching it for transitions cannot
// see a second deflect in a row at all. Detecting repeats therefore needs a
// second, independent input: something that says "a NEW guard resolution just
// happened". This class consumes that second input but does NOT claim to know
// what it is.
//
// !! STATUS: NO OCCURRENCE FIELD HAS BEEN CONFIRMED IN THE GAME YET. !!
// `GuardObservation::occurrence` is supplied by the caller. Until a real field
// is found and validated live, this class is exercised only by synthetic
// tests -- passing those tests says nothing about in-game accuracy.
//
// DESIGN RULES (each one exists because the opposite is a real failure mode):
//  * An event is emitted only on a validated occurrence change. A held outcome
//    value never produces an event, in either direction.
//  * The outcome is read from the SAME observation that carried the occurrence
//    change -- never combined across samples by accident. If the outcome is
//    known to update slightly after the occurrence, set `outcomeSettleUs` and
//    the event is held until the outcome stops moving; it is never guessed.
//  * A counter that jumps by more than one reports ONE Unresolved event
//    carrying the jump size. Missed resolutions are never reconstructed into
//    N fabricated events.
//  * The first observation after attach, object replacement, read failure, or
//    a capture gap establishes a baseline only. No event can be produced by
//    comparing across a continuity break.
//  * Time-based debouncing is deliberately absent. Two genuine hits 40 ms
//    apart are two events.

#include <cstdint>
#include <optional>
#include <vector>

namespace sekiro_haptics::process {

/// How the caller's occurrence signal behaves. Chosen by the caller from live
/// evidence; this class does not infer it.
enum class OccurrenceMode {
    /// Monotonically increasing count of resolutions. +1 == one new event;
    /// any larger step is reported Unresolved.
    Counter,
    /// Briefly non-zero per resolution. A zero -> non-zero edge is one event.
    Pulse,
    /// An identifier replaced per resolution. Any change to a different
    /// non-zero value is one event.
    Identifier,
};

enum class GuardEventKind {
    /// Outcome byte read as 1 at the moment the occurrence changed.
    Deflect,
    /// Outcome byte read as 0 at the moment the occurrence changed.
    Block,
    /// A resolution certainly happened but cannot be classified or counted --
    /// NEVER report these as deflects or blocks.
    Unresolved,
};

enum class UnresolvedReason {
    None,
    /// Counter advanced by more than 1: at least one resolution was missed.
    CounterJump,
    /// The outcome byte was still moving when the next occurrence arrived.
    OutcomeNeverSettled,
    /// A continuity break landed on a pending event.
    ContinuityBreak,
    /// The outcome byte held a value outside {0, 1}.
    OutcomeOutOfRange,
};

struct GuardEvent {
    GuardEventKind kind = GuardEventKind::Unresolved;
    UnresolvedReason reason = UnresolvedReason::None;
    /// Timestamp of the observation that carried the occurrence change --
    /// not of the sample the outcome finally settled on.
    std::int64_t timestampUs = 0;
    std::uint64_t generation = 0;
    std::uint64_t occurrence = 0;
    /// For Counter mode: how far the counter moved. 1 for a clean event.
    std::uint64_t occurrenceDelta = 0;
    std::uint8_t outcome = 0;
};

/// One polled observation of the player's guard state.
struct GuardObservation {
    std::int64_t timestampUs = 0;
    /// False when the pointer chain failed to resolve, the vptr check failed,
    /// or the read failed. `occurrence`/`outcome` are then ignored entirely.
    bool readOk = false;
    /// Identifies the object instance. A change means a different object --
    /// its values must never be diffed against the previous one's.
    std::uint64_t generation = 0;
    /// Set for the first observation after a recorded gap, so the detector
    /// does not compare across unobserved time.
    bool continuityBreak = false;
    std::uint64_t occurrence = 0;
    std::uint8_t outcome = 0;
};

struct GuardDetectorConfig {
    OccurrenceMode mode = OccurrenceMode::Counter;
    /// Bits of the counter, for wrap arithmetic. 0 means 64.
    unsigned counterBits = 32;
    /// If > 0, an event is held this long so a lagging outcome byte can
    /// settle. 0 classifies immediately from the same observation.
    std::int64_t outcomeSettleUs = 0;
};

struct GuardDetectorStats {
    std::uint64_t observations = 0;
    std::uint64_t baselinesEstablished = 0;
    std::uint64_t continuityBreaks = 0;
    std::uint64_t readFailures = 0;
    std::uint64_t generationChanges = 0;
    std::uint64_t eventsEmitted = 0;
    std::uint64_t unresolvedEmitted = 0;
};

class GuardOutcomeEventDetector {
public:
    explicit GuardOutcomeEventDetector(GuardDetectorConfig config = {}) : config_(config) {}

    /// Feeds one observation and returns the events it completed (usually
    /// none or one; a pending event plus a new one can complete together).
    std::vector<GuardEvent> Update(const GuardObservation& obs) {
        std::vector<GuardEvent> out;
        ++stats_.observations;

        if (!obs.readOk) {
            ++stats_.readFailures;
            FlushPendingAs(UnresolvedReason::ContinuityBreak, out);
            Invalidate();
            return out;
        }
        if (obs.continuityBreak) {
            ++stats_.continuityBreaks;
            FlushPendingAs(UnresolvedReason::ContinuityBreak, out);
            Invalidate();
            // The observation itself is still a valid first look at the object.
            EstablishBaseline(obs);
            return out;
        }
        if (!haveBaseline_ || obs.generation != generation_) {
            if (haveBaseline_ && obs.generation != generation_) {
                ++stats_.generationChanges;
                FlushPendingAs(UnresolvedReason::ContinuityBreak, out);
            }
            Invalidate();
            EstablishBaseline(obs);
            return out;
        }

        // A pending event may settle on this observation, but a *new*
        // occurrence on the same observation takes priority: the pending one
        // then never settled.
        const std::optional<std::uint64_t> delta = OccurrenceStep(obs.occurrence);

        if (pending_.has_value()) {
            if (delta.has_value()) {
                FlushPendingAs(UnresolvedReason::OutcomeNeverSettled, out);
            } else if (obs.outcome != pendingOutcome_) {
                // still moving -- restart the settle window
                pendingOutcome_ = obs.outcome;
                pendingSince_ = obs.timestampUs;
            } else if (obs.timestampUs - pendingSince_ >= config_.outcomeSettleUs) {
                GuardEvent ev = *pending_;
                ev.outcome = obs.outcome;
                Classify(ev);
                Emit(ev, out);
                pending_.reset();
            }
        }

        if (delta.has_value()) {
            GuardEvent ev;
            ev.timestampUs = obs.timestampUs;
            ev.generation = obs.generation;
            ev.occurrence = obs.occurrence;
            ev.occurrenceDelta = *delta;
            ev.outcome = obs.outcome;
            if (config_.mode == OccurrenceMode::Counter && *delta > 1) {
                ev.kind = GuardEventKind::Unresolved;
                ev.reason = UnresolvedReason::CounterJump;
                Emit(ev, out);
            } else if (config_.outcomeSettleUs > 0) {
                pending_ = ev;
                pendingOutcome_ = obs.outcome;
                pendingSince_ = obs.timestampUs;
            } else {
                Classify(ev);
                Emit(ev, out);
            }
        }

        lastOccurrence_ = obs.occurrence;
        return out;
    }

    /// Call when the observation stream ends. A still-pending event is
    /// reported Unresolved rather than silently dropped or guessed.
    std::vector<GuardEvent> Finish() {
        std::vector<GuardEvent> out;
        FlushPendingAs(UnresolvedReason::OutcomeNeverSettled, out);
        return out;
    }

    const GuardDetectorStats& Stats() const { return stats_; }

private:
    void EstablishBaseline(const GuardObservation& obs) {
        generation_ = obs.generation;
        lastOccurrence_ = obs.occurrence;
        haveBaseline_ = true;
        ++stats_.baselinesEstablished;
    }

    void Invalidate() {
        haveBaseline_ = false;
        pending_.reset();
    }

    void FlushPendingAs(UnresolvedReason reason, std::vector<GuardEvent>& out) {
        if (!pending_.has_value()) {
            return;
        }
        GuardEvent ev = *pending_;
        ev.kind = GuardEventKind::Unresolved;
        ev.reason = reason;
        Emit(ev, out);
        pending_.reset();
    }

    void Emit(const GuardEvent& ev, std::vector<GuardEvent>& out) {
        if (ev.kind == GuardEventKind::Unresolved) {
            ++stats_.unresolvedEmitted;
        } else {
            ++stats_.eventsEmitted;
        }
        out.push_back(ev);
    }

    static void Classify(GuardEvent& ev) {
        if (ev.outcome == 1) {
            ev.kind = GuardEventKind::Deflect;
        } else if (ev.outcome == 0) {
            ev.kind = GuardEventKind::Block;
        } else {
            ev.kind = GuardEventKind::Unresolved;
            ev.reason = UnresolvedReason::OutcomeOutOfRange;
        }
    }

    /// How far the occurrence signal moved, or nullopt for "no new resolution".
    std::optional<std::uint64_t> OccurrenceStep(std::uint64_t now) const {
        switch (config_.mode) {
            case OccurrenceMode::Counter: {
                if (now == lastOccurrence_) {
                    return std::nullopt;
                }
                const unsigned bits = config_.counterBits == 0 ? 64 : config_.counterBits;
                const std::uint64_t mask =
                    bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
                // Unsigned wrap-around arithmetic: 0xFFFFFFFF -> 0 is a step of 1.
                return (now - lastOccurrence_) & mask;
            }
            case OccurrenceMode::Pulse:
                return (lastOccurrence_ == 0 && now != 0) ? std::optional<std::uint64_t>{1}
                                                          : std::nullopt;
            case OccurrenceMode::Identifier:
                return (now != lastOccurrence_ && now != 0) ? std::optional<std::uint64_t>{1}
                                                            : std::nullopt;
        }
        return std::nullopt;
    }

    GuardDetectorConfig config_;
    GuardDetectorStats stats_;

    bool haveBaseline_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t lastOccurrence_ = 0;

    std::optional<GuardEvent> pending_;
    std::uint8_t pendingOutcome_ = 0;
    std::int64_t pendingSince_ = 0;
};

} // namespace sekiro_haptics::process
