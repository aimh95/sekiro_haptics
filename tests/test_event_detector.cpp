#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "testing.hpp"

using namespace sekiro_haptics;

namespace {
GameSignal Signal(const std::string& name, const std::string& value) {
    GameSignal signal;
    signal.signal = name;
    signal.value = value;
    signal.timestamp = std::chrono::microseconds(1234);
    return signal;
}
} // namespace

SH_TEST(ManualLabelEventDetector_PerfectDeflectSignal_EmitsExactlyOneEvent) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("manual.perfect_deflect", "true"), events);

    SH_CHECK(events.size() == 1);
    SH_CHECK(events[0].gameId == "sekiro");
    SH_CHECK(events[0].eventId == "combat.perfect_deflect");
}

SH_TEST(ManualLabelEventDetector_TakeDamageSignal_EmitsExpectedEvent) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("manual.take_damage", "true"), events);

    SH_CHECK(events.size() == 1);
    SH_CHECK(events[0].eventId == "combat.take_damage");
}

SH_TEST(ManualLabelEventDetector_UnrelatedSignal_EmitsNoEvent) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("player.hp_delta", "0"), events);
    detector.OnSignal(Signal("player.block_state", "active"), events);

    SH_CHECK(events.empty());
}

SH_TEST(ManualLabelEventDetector_PerfectDeflectValueFalse_EmitsNoEvent) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("manual.perfect_deflect", "false"), events);

    SH_CHECK(events.empty());
}

SH_TEST(ManualLabelEventDetector_DuplicateManualLabels_EmitEventEachTime) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("manual.perfect_deflect", "true"), events);
    detector.OnSignal(Signal("manual.perfect_deflect", "true"), events);

    SH_CHECK(events.size() == 2);
}

SH_TEST(ManualLabelEventDetector_EmittedEvent_HasExpectedGameIdAndTimestamp) {
    ManualLabelEventDetector detector;
    std::vector<GameEvent> events;

    detector.OnSignal(Signal("manual.perfect_deflect", "true"), events);

    SH_CHECK(events.size() == 1);
    SH_CHECK(events[0].gameId == "sekiro");
    SH_CHECK(events[0].timestamp.count() == 1234);
    SH_CHECK(events[0].metadata.at("sourceSignal") == "manual.perfect_deflect");
}
