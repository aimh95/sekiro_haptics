#pragma once

#include "sekiro_haptics/events/IGameEventDetector.hpp"

namespace sekiro_haptics {

/// Deterministic, test-only detector used to exercise the rest of the
/// pipeline (mapping, presets, scheduling) without any real Sekiro event
/// detection. This is NOT a real Sekiro detector -- it only recognizes
/// hand-authored "manual.*" trace labels, e.g. from a human annotating a
/// recorded trace. See docs/testing.md.
///
/// Behavior:
///   signal == "manual.perfect_deflect", value == "true"
///       -> GameEvent{gameId="sekiro", eventId="combat.perfect_deflect"}
///   signal == "manual.take_damage", value == "true"
///       -> GameEvent{gameId="sekiro", eventId="combat.take_damage"}
///   anything else (including a manual.* signal with a non-"true" value)
///       -> no event
///
/// Repeated identical labels each emit their own event; there is no
/// de-duplication. `GameEvent::timestamp` copies the signal's timestamp;
/// `metadata["sourceSignal"]` records which signal produced the event.
class ManualLabelEventDetector final : public IGameEventDetector {
public:
    void OnSignal(const GameSignal& signal, std::vector<GameEvent>& outEvents) override;
};

} // namespace sekiro_haptics
