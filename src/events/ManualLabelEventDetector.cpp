#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"

namespace sekiro_haptics {

namespace {

GameEvent MakeEvent(const GameSignal& signal, const std::string& eventId) {
    GameEvent event;
    event.gameId = "sekiro";
    event.eventId = eventId;
    event.timestamp = signal.timestamp;
    event.metadata["sourceSignal"] = signal.signal;
    return event;
}

} // namespace

void ManualLabelEventDetector::OnSignal(const GameSignal& signal, std::vector<GameEvent>& outEvents) {
    if (signal.value != "true") {
        return;
    }

    auto validityIt = signal.extra.find("validity");
    if (validityIt != signal.extra.end() && validityIt->second != "valid") {
        return;
    }

    if (signal.signal == "manual.perfect_deflect") {
        outEvents.push_back(MakeEvent(signal, "combat.perfect_deflect"));
    } else if (signal.signal == "manual.take_damage") {
        outEvents.push_back(MakeEvent(signal, "combat.take_damage"));
    }
}

} // namespace sekiro_haptics
