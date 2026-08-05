#include "sekiro_haptics/replay/TraceJsonl.hpp"
#include "testing.hpp"

#include <filesystem>
#include <string>

using namespace sekiro_haptics;
using namespace sekiro_haptics::trace;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

SH_TEST(TraceReader_ReadNext_ParsesValidLine) {
    TraceReader reader(Fixture("perfect_deflect.jsonl"));
    SH_CHECK(reader.IsOpen());

    GameSignal signal;
    TraceReadResult result = reader.ReadNext(signal);

    SH_CHECK(result == TraceReadResult::Success);
    SH_CHECK(signal.timestamp.count() == 1000);
    SH_CHECK(signal.signal == "player.block_state");
    SH_CHECK(signal.value == "active");
}

SH_TEST(TraceReader_ReadNext_ParsesMultipleLinesInOrder) {
    TraceReader reader(Fixture("perfect_deflect.jsonl"));

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.signal == "player.block_state");
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.signal == "enemy.attack_contact");
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.signal == "player.hp_delta");
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.signal == "manual.perfect_deflect");
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::EndOfTrace);
}

SH_TEST(TraceReader_ReadNext_RejectsMalformedJson) {
    TraceReader reader(Fixture("malformed_json.jsonl"));

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);

    std::string error;
    TraceReadResult result = reader.ReadNext(signal, &error);
    SH_CHECK(result == TraceReadResult::MalformedJson);
    SH_CHECK(error.find("line 2") != std::string::npos);
}

SH_TEST(TraceReader_ReadNext_RejectsMissingTimestamp) {
    TraceReader reader(Fixture("missing_timestamp.jsonl"));

    GameSignal signal;
    std::string error;
    TraceReadResult result = reader.ReadNext(signal, &error);

    SH_CHECK(result == TraceReadResult::MissingTimestamp);
    SH_CHECK(error.find("line 1") != std::string::npos);
}

SH_TEST(TraceReader_ReadNext_RejectsMissingSignal) {
    TraceReader reader(Fixture("missing_signal.jsonl"));

    GameSignal signal;
    std::string error;
    TraceReadResult result = reader.ReadNext(signal, &error);

    SH_CHECK(result == TraceReadResult::MissingSignal);
    SH_CHECK(error.find("line 1") != std::string::npos);
}

SH_TEST(TraceReader_ReadNext_PreservesUnknownOptionalFields) {
    TraceReader reader(Fixture("unknown_fields.jsonl"));

    GameSignal signal;
    TraceReadResult result = reader.ReadNext(signal);

    SH_CHECK(result == TraceReadResult::Success);
    SH_CHECK(signal.signal == "player.animation");
    SH_CHECK(signal.value == "parry_success");
    SH_CHECK(signal.extra.at("animId") == "42");
    SH_CHECK(signal.extra.at("note") == "test");
}

SH_TEST(TraceReader_ReadNext_EnforcesNonDecreasingTimestampOrder) {
    TraceReader reader(Fixture("out_of_order.jsonl"));

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);

    std::string error;
    TraceReadResult result = reader.ReadNext(signal, &error);
    SH_CHECK(result == TraceReadResult::OutOfOrderTimestamp);
    SH_CHECK(error.find("line 2") != std::string::npos);
}

SH_TEST(TraceReader_ReadNext_ReturnsEndOfTraceOnEmptyFile) {
    TraceReader reader(Fixture("empty_trace.jsonl"));
    SH_CHECK(reader.IsOpen());

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::EndOfTrace);
}

SH_TEST(TraceReader_ReadNext_SkipsBlankLines) {
    TraceReader reader(Fixture("blank_lines.jsonl"));

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.timestamp.count() == 1000);
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.timestamp.count() == 1010);
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::EndOfTrace);
}

SH_TEST(TraceReader_Rewind_ReplaysFromBeginning) {
    TraceReader reader(Fixture("perfect_deflect.jsonl"));

    GameSignal signal;
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);

    SH_CHECK(reader.Rewind());
    SH_CHECK(reader.LineNumber() == 0);

    SH_CHECK(reader.ReadNext(signal) == TraceReadResult::Success);
    SH_CHECK(signal.signal == "player.block_state");
}

SH_TEST(TraceWriter_WriteSignal_RoundTripsThroughTraceReader) {
    std::string path = (std::filesystem::temp_directory_path() / "sh_trace_roundtrip_test.jsonl").string();

    GameSignal original;
    original.timestamp = std::chrono::microseconds(4242);
    original.signal = "manual.perfect_deflect";
    original.value = "true";
    original.extra["note"] = "round trip";

    {
        TraceWriter writer(path);
        SH_CHECK(writer.IsOpen());
        SH_CHECK(writer.WriteSignal(original));
    }

    TraceReader reader(path);
    SH_CHECK(reader.IsOpen());

    GameSignal roundTripped;
    TraceReadResult result = reader.ReadNext(roundTripped);

    SH_CHECK(result == TraceReadResult::Success);
    SH_CHECK(roundTripped.timestamp == original.timestamp);
    SH_CHECK(roundTripped.signal == original.signal);
    SH_CHECK(roundTripped.value == original.value);
    SH_CHECK(roundTripped.extra == original.extra);

    std::filesystem::remove(path);
}
