// GameEvent -> EventMapping -> OutputPreset -> outputs, with no controller
// and no audio endpoint: the PCM sink is a recording double and the trigger
// path runs on the existing fake transport.

#include "sekiro_haptics/runtime/OutputRuntime.hpp"
#include "FakeDualSenseTransport.hpp"
#include "testing.hpp"

#include <set>
#include <string>
#include <vector>

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

namespace {

constexpr std::int64_t kMs = 1000;

/// Records what was asked for, in order, with the correlation id intact.
class RecordingPcmSink final : public IPcmOutputSink {
public:
    struct Call {
        bool speaker = false;
        std::string cueId;
        float gain = 0.0f;
        float balance = 0.0f;
        std::uint64_t correlationId = 0;
    };

    explicit RecordingPcmSink(std::set<std::string> cues) : cues_(std::move(cues)) {}

    bool HasCue(const std::string& cueId) const override { return cues_.count(cueId) != 0; }

    bool QueueSpeaker(const std::string& cueId, float gain, std::uint64_t correlationId) override {
        if (!HasCue(cueId) || failEverything) return false;
        calls.push_back({true, cueId, gain, 0.0f, correlationId});
        return true;
    }

    bool QueueHaptic(const std::string& cueId, float gain, float balance,
                     std::uint64_t correlationId) override {
        if (!HasCue(cueId) || failEverything) return false;
        calls.push_back({false, cueId, gain, balance, correlationId});
        return true;
    }

    std::vector<Call> calls;
    bool failEverything = false;

private:
    std::set<std::string> cues_;
};

GameEvent MakeEvent(const std::string& eventId, std::int64_t timestampUs) {
    GameEvent event;
    event.gameId = "sekiro";
    event.eventId = eventId;
    event.timestamp = std::chrono::microseconds(timestampUs);
    return event;
}

OutputPreset GuardPreset(const std::string& id) {
    OutputPreset preset;
    preset.presetId = id;
    preset.displayName = id;
    preset.speaker.push_back({id + ".speaker", 1.0f, 0.0f});
    preset.pcmHaptic.push_back({id + ".haptic", 1.0f, 0.0f, 0.0f});
    return preset;
}

} // namespace

SH_TEST(OutputRuntime_AMappedEventProducesEveryLayerOfItsPreset) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto record = runtime.Handle(MakeEvent("combat.perfect_deflect", 0), 0);

    SH_CHECK(record.outcome == DispatchOutcome::Dispatched);
    SH_CHECK(record.presetId == "deflect");
    SH_CHECK(record.layersTotal == 2);
    SH_CHECK(record.layersDispatched == 2);
    SH_CHECK(record.layersFailed == 0);
    SH_CHECK(sink.calls.size() == 2);
    // Both outputs carry the same correlation id, which is what lets a voice
    // in the mixer be traced back to this event.
    SH_CHECK(sink.calls[0].correlationId == record.correlationId);
    SH_CHECK(sink.calls[1].correlationId == record.correlationId);
}

SH_TEST(OutputRuntime_ConsecutiveEventsEachGetTheirOwnIdAndTheirOwnLayers) {
    // The completion condition: repeated events produce no missing and no
    // duplicated output, counted against the preset's layer count rather than
    // assuming one voice per event.
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    mappings.AddMapping({"sekiro", "combat.block", "block"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    presets.AddPreset(GuardPreset("block"));
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic", "block.speaker", "block.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const std::vector<std::string> sequence = {
        "combat.perfect_deflect", "combat.perfect_deflect", "combat.block",
        "combat.block", "combat.perfect_deflect"};
    std::vector<std::uint64_t> ids;
    std::int64_t t = 0;
    for (const auto& eventId : sequence) {
        ids.push_back(runtime.Handle(MakeEvent(eventId, t), t).correlationId);
        t += 100 * kMs;
    }

    std::set<std::uint64_t> unique(ids.begin(), ids.end());
    SH_CHECK(unique.size() == sequence.size());          // no duplicates
    SH_CHECK(sink.calls.size() == sequence.size() * 2);  // no omissions
    SH_CHECK(runtime.Stats().eventsDispatched == sequence.size());
    SH_CHECK(runtime.Stats().layerFailures == 0);

    // Order preserved, and each event's two calls sit together.
    for (std::size_t i = 0; i < sequence.size(); ++i) {
        SH_CHECK(sink.calls[i * 2].correlationId == ids[i]);
        SH_CHECK(sink.calls[i * 2 + 1].correlationId == ids[i]);
        SH_CHECK(sink.calls[i * 2].speaker);
        SH_CHECK(!sink.calls[i * 2 + 1].speaker);
    }
}

SH_TEST(OutputRuntime_ALayerWithAStartOffsetWaitsWithoutHoldingUpTheOthers) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    auto preset = GuardPreset("deflect");
    preset.pcmHaptic[0].startOffsetMs = 20.0f;   // haptic lags the clang
    presets.AddPreset(preset);
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto record = runtime.Handle(MakeEvent("combat.perfect_deflect", 0), 0);
    SH_CHECK(record.layersDispatched == 1);
    SH_CHECK(record.layersPending == 1);
    SH_CHECK(sink.calls.size() == 1 && sink.calls[0].speaker);

    runtime.Tick(10 * kMs);
    SH_CHECK(sink.calls.size() == 1);   // not due yet

    runtime.Tick(20 * kMs);
    SH_CHECK(sink.calls.size() == 2);
    SH_CHECK(!sink.calls[1].speaker);
    SH_CHECK(sink.calls[1].correlationId == record.correlationId);
    SH_CHECK(runtime.Stats().pendingLayers == 0);

    const auto* stored = runtime.FindRecord(record.correlationId);
    SH_CHECK(stored != nullptr);
    SH_CHECK(stored->layersDispatched == 2);
    SH_CHECK(stored->layersPending == 0);
}

SH_TEST(OutputRuntime_TriggerLayersGoThroughTheTriggerRuntimeWithTheirOwnLifetime) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime triggers(transport, state);

    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "demo.trigger", "demo"});
    OutputPresetRepository presets;
    OutputPreset preset;
    preset.presetId = "demo";
    preset.displayName = "demo";
    TriggerOutputLayer layer;
    layer.side = TriggerSide::Right;
    layer.spec = TriggerEffectSpec::MakeWeapon(3, 7, 8);
    layer.durationMs = 120.0f;
    preset.trigger.push_back(layer);
    presets.AddPreset(preset);

    OutputRuntime runtime(mappings, presets, nullptr, &triggers, &state);
    // A non-zero clock, so "stamped" is distinguishable from "left at zero".
    const auto record = runtime.Handle(MakeEvent("demo.trigger", 4 * kMs), 4 * kMs);

    SH_CHECK(record.outcome == DispatchOutcome::Dispatched);
    SH_CHECK(record.layersDispatched == 1);
    SH_CHECK(record.times.firstHidSubmitUs == 4 * kMs);
    SH_CHECK(triggers.Active(TriggerSide::Right).active);

    // The trigger's 120 ms belongs to the trigger alone.
    runtime.Tick(123 * kMs);
    SH_CHECK(triggers.Active(TriggerSide::Right).active);
    runtime.Tick(125 * kMs);
    SH_CHECK(!triggers.Active(TriggerSide::Right).active);
    SH_CHECK(triggers.Stats().expired == 1);
}

SH_TEST(OutputRuntime_TriggerChangesDoNotDisturbTheQueuedPcmOutput) {
    // "Trigger 효과의 충돌 처리는 PCM 파형 합산과 별개" -- applying, replacing
    // and expiring a trigger must not touch a single PCM call.
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime triggers(transport, state);

    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    auto preset = GuardPreset("deflect");
    TriggerOutputLayer layer;
    layer.side = TriggerSide::Left;
    layer.spec = TriggerEffectSpec::MakeFeedback(3, 6);
    layer.durationMs = 60.0f;
    preset.trigger.push_back(layer);
    presets.AddPreset(preset);
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, &triggers, &state);

    runtime.Handle(MakeEvent("combat.perfect_deflect", 0), 0);
    runtime.Handle(MakeEvent("combat.perfect_deflect", 30 * kMs), 30 * kMs);   // replaces the trigger
    runtime.Tick(200 * kMs);                                                   // expires it

    SH_CHECK(sink.calls.size() == 4);     // 2 events x 2 PCM layers, untouched
    SH_CHECK(triggers.Stats().replaced == 1);
    SH_CHECK(triggers.Stats().expired == 1);
    SH_CHECK(runtime.Stats().speakerLayersQueued == 2);
    SH_CHECK(runtime.Stats().hapticLayersQueued == 2);
}

SH_TEST(OutputRuntime_AnUnmappedEventIsRecordedButProducesNothing) {
    MappingRepository mappings;
    OutputPresetRepository presets;
    RecordingPcmSink sink({});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto record = runtime.Handle(MakeEvent("combat.something_else", 0), 0);
    SH_CHECK(record.outcome == DispatchOutcome::NoMapping);
    SH_CHECK(sink.calls.empty());
    SH_CHECK(runtime.Stats().eventsWithoutMapping == 1);
    SH_CHECK(runtime.Stats().eventsDispatched == 0);
}

SH_TEST(OutputRuntime_AMappingToAMissingPresetIsReportedNotSilentlySkipped) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.block", "not_loaded"});
    OutputPresetRepository presets;
    RecordingPcmSink sink({});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto record = runtime.Handle(MakeEvent("combat.block", 0), 0);
    SH_CHECK(record.outcome == DispatchOutcome::NoPreset);
    SH_CHECK(!record.note.empty());
    SH_CHECK(runtime.Stats().eventsWithoutPreset == 1);
}

SH_TEST(OutputRuntime_AStaleEventIsDroppedRatherThanPlayedLate) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntimeConfig config;
    config.maxOutputLatencyUs = 120 * kMs;
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr, config);

    const auto record = runtime.Handle(MakeEvent("combat.perfect_deflect", 10 * kMs), 400 * kMs);
    SH_CHECK(record.outcome == DispatchOutcome::DroppedLate);
    SH_CHECK(sink.calls.empty());
    SH_CHECK(runtime.Stats().eventsDroppedLate == 1);
}

SH_TEST(OutputRuntime_BindRejectsAPresetNamingACueThatDoesNotExist) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    RecordingPcmSink sink({"deflect.speaker"});   // haptic clip missing
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto problems = runtime.Bind();
    SH_CHECK(problems.size() == 1);
    SH_CHECK(problems[0].message.find("deflect.haptic") != std::string::npos);
}

SH_TEST(OutputRuntime_BindPassesWhenEveryCueResolves) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);
    SH_CHECK(runtime.Bind().empty());
}

SH_TEST(OutputRuntime_AFailingSinkIsCountedRatherThanReportedAsSuccess) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    presets.AddPreset(GuardPreset("deflect"));
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    sink.failEverything = true;
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    const auto record = runtime.Handle(MakeEvent("combat.perfect_deflect", 0), 0);
    SH_CHECK(record.outcome == DispatchOutcome::Failed);
    SH_CHECK(record.layersFailed == 2);
    SH_CHECK(record.layersDispatched == 0);
    SH_CHECK(runtime.Stats().layerFailures == 2);
    SH_CHECK(runtime.Stats().eventsDispatched == 0);
}

SH_TEST(OutputRuntime_DropPendingDiscardsUndispatchedLayersOnly) {
    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    auto preset = GuardPreset("deflect");
    preset.pcmHaptic[0].startOffsetMs = 50.0f;
    presets.AddPreset(preset);
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, nullptr, nullptr);

    runtime.Handle(MakeEvent("combat.perfect_deflect", 0), 0);
    SH_CHECK(sink.calls.size() == 1);
    runtime.DropPending();
    runtime.Tick(1000 * kMs);
    SH_CHECK(sink.calls.size() == 1);    // the deferred layer never fires
    SH_CHECK(runtime.Stats().pendingLayers == 0);
}

SH_TEST(OutputRuntime_StageTimesAreRecordedSeparatelyForPcmAndHid) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseOutputState state;
    AdaptiveTriggerRuntime triggers(transport, state);

    MappingRepository mappings;
    mappings.AddMapping({"sekiro", "combat.perfect_deflect", "deflect"});
    OutputPresetRepository presets;
    auto preset = GuardPreset("deflect");
    TriggerOutputLayer layer;
    layer.side = TriggerSide::Right;
    layer.spec = TriggerEffectSpec::MakeFeedback(2, 5);
    layer.startOffsetMs = 30.0f;     // the HID stage happens later than the PCM one
    preset.trigger.push_back(layer);
    presets.AddPreset(preset);
    RecordingPcmSink sink({"deflect.speaker", "deflect.haptic"});
    OutputRuntime runtime(mappings, presets, &sink, &triggers, &state);

    const auto record = runtime.Handle(MakeEvent("combat.perfect_deflect", 5 * kMs), 8 * kMs);
    SH_CHECK(record.times.eventUs == 5 * kMs);
    SH_CHECK(record.times.receivedUs == 8 * kMs);
    SH_CHECK(record.times.mappedUs == 8 * kMs);
    SH_CHECK(record.times.firstPcmQueuedUs == 8 * kMs);
    SH_CHECK(record.times.firstHidSubmitUs == 0);      // not yet

    runtime.Tick(38 * kMs);
    const auto* stored = runtime.FindRecord(record.correlationId);
    SH_CHECK(stored != nullptr);
    SH_CHECK(stored->times.firstHidSubmitUs == 38 * kMs);
    SH_CHECK(stored->times.firstPcmQueuedUs == 8 * kMs);
}
