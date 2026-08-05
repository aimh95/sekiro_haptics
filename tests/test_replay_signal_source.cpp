#include "sekiro_haptics/signals/ReplaySignalSource.hpp"
#include "sekiro_haptics/signals/VectorSignalSource.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

SH_TEST(ReplaySignalSource_Next_EmitsAllSignalsInOrder) {
    ReplaySignalSource source(Fixture("perfect_deflect.jsonl"));
    SH_CHECK(source.IsOpen());

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "player.block_state");
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "enemy.attack_contact");
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "player.hp_delta");
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "manual.perfect_deflect");
    SH_CHECK(source.Next(signal) == SignalSourceResult::EndOfSource);
}

SH_TEST(ReplaySignalSource_Next_ReturnsEndOfSourceOnEmptyTrace) {
    ReplaySignalSource source(Fixture("empty_trace.jsonl"));

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::EndOfSource);
}

SH_TEST(ReplaySignalSource_Reset_ReplaysFromBeginning) {
    ReplaySignalSource source(Fixture("perfect_deflect.jsonl"));

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);

    SH_CHECK(source.Reset());

    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "player.block_state");
}

SH_TEST(ReplaySignalSource_Next_ReturnsMalformedInputWithLineNumber) {
    ReplaySignalSource source(Fixture("missing_timestamp.jsonl"));

    GameSignal signal;
    std::string error;
    SH_CHECK(source.Next(signal, &error) == SignalSourceResult::MalformedInput);
    SH_CHECK(error.find("line 1") != std::string::npos);
}

SH_TEST(VectorSignalSource_Next_EmitsProvidedSignalsInOrder) {
    std::vector<GameSignal> signals(2);
    signals[0].signal = "first";
    signals[1].signal = "second";
    VectorSignalSource source(signals);

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "first");
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "second");
    SH_CHECK(source.Next(signal) == SignalSourceResult::EndOfSource);
}

SH_TEST(VectorSignalSource_Reset_ReplaysFromBeginning) {
    std::vector<GameSignal> signals(1);
    signals[0].signal = "only";
    VectorSignalSource source(signals);

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(source.Next(signal) == SignalSourceResult::EndOfSource);

    SH_CHECK(source.Reset());
    SH_CHECK(source.Next(signal) == SignalSourceResult::Success);
    SH_CHECK(signal.signal == "only");
}

SH_TEST(VectorSignalSource_Next_ReturnsEndOfSourceWhenEmpty) {
    VectorSignalSource source({});

    GameSignal signal;
    SH_CHECK(source.Next(signal) == SignalSourceResult::EndOfSource);
}
