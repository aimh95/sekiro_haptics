#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace sekiro_haptics {

/// An interpreted "something happened" fact, produced by an
/// IGameEventDetector from one or more GameSignal observations.
///
/// A GameEvent describes *what happened* (e.g. gameId="sekiro",
/// eventId="combat.perfect_deflect") -- it says nothing about what should
/// be felt on a controller. That translation is EventMapping's job (event
/// -> presetId) and HapticPreset's job (presetId -> HapticEffect). Keeping
/// these separate means a new game event never requires touching
/// HapticEffect/HapticEffectType -- see the migration note in
/// docs/01-architecture.md.
struct GameEvent {
    std::string gameId;
    std::string eventId;
    std::chrono::microseconds timestamp{0};
    std::unordered_map<std::string, std::string> metadata;
};

} // namespace sekiro_haptics
