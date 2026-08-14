#pragma once

#include "sekiro_haptics/events/IGameEventDetector.hpp"

namespace sekiro_haptics {

/// Deterministic, test-only detector used to exercise the rest of the
/// pipeline (mapping, presets, scheduling) without any real Sekiro event
/// detection. This is NOT a real Sekiro detector -- it only recognizes
/// hand-authored "manual.*" trace labels, e.g. from a human annotating a
/// recorded trace. See docs/04-testing.md.
///
/// Behavior:
///   signal == "manual.perfect_deflect", value == "true"
///       -> GameEvent{gameId="sekiro", eventId="combat.perfect_deflect"}
///   signal == "manual.take_damage", value == "true"
///       -> GameEvent{gameId="sekiro", eventId="combat.take_damage"}
///   anything else (including a manual.* signal with a non-"true" value)
///       -> no event
///
/// A signal whose extra["validity"] is present and not "valid" (i.e.
/// "unavailable" or "disabled" -- see TraceJsonl.hpp) never produces an
/// event, regardless of its value: a reading the source couldn't actually
/// take is never treated as grounds for an event, even if it happens to
/// carry value == "true".
///
/// Repeated identical labels each emit their own event; there is no
/// de-duplication. `GameEvent::timestamp` copies the signal's timestamp;
/// `metadata["sourceSignal"]` records which signal produced the event.
class ManualLabelEventDetector final : public IGameEventDetector {
public:
    void OnSignal(const GameSignal& signal, std::vector<GameEvent>& outEvents) override;
};

} // namespace sekiro_haptics
