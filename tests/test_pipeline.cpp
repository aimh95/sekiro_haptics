#include "sekiro_haptics/MockHapticBackend.hpp"
#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "sekiro_haptics/pipeline/ReplayPipeline.hpp"
#include "sekiro_haptics/signals/ReplaySignalSource.hpp"
#include "sekiro_haptics/signals/VectorSignalSource.hpp"
#include "testing.hpp"

#include <chrono>
#include <string>

using namespace sekiro_haptics;
using namespace std::chrono_literals;

namespace {

std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}

// Test-only backend double: reports as never connected, so the pipeline's
// connectivity check has something to actually block on.
class DisconnectedBackend final : public IHapticBackend {
public:
    HapticBackendResult SendEffect(const HapticEffect&) override {
        return HapticBackendResult::NotConnected;
    }
    HapticBackendResult Reset() override {
        return HapticBackendResult::NotConnected;
    }
    bool IsConnected() const override {
        return false;
    }
};

// Test-only detector double: emits nothing per-signal, but emits one event
// from Flush() -- used to prove ReplayPipeline::Flush() forwards a
// detector's flushed events the same way ProcessSignal() does.
class BufferingDetector final : public IGameEventDetector {
public:
    void OnSignal(const GameSignal&, std::vector<GameEvent>&) override {}

    void Flush(std::vector<GameEvent>& outEvents) override {
        GameEvent event;
        event.gameId = "sekiro";
        event.eventId = "combat.perfect_deflect";
        outEvents.push_back(event);
    }
};

GameSignal ManualSignal(const std::string& name, const std::string& value) {
    GameSignal signal;
    signal.signal = name;
    signal.value = value;
    return signal;
}

} // namespace

SH_TEST(Pipeline_PerfectDeflectFixture_DispatchesExactlyOneEffect) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    ReplaySignalSource source(Fixture("perfect_deflect.jsonl"));
    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;

    std::size_t count = RunReplayLoop(source, pipeline, outcomes, malformed);

    SH_CHECK(count == 4);
    SH_CHECK(malformed.empty());

    int dispatchedCount = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.stage == PipelineStage::Dispatched) {
            ++dispatchedCount;
            SH_CHECK(outcome.presetId.has_value());
            SH_CHECK(*outcome.presetId == "sharp_metal_v1");
        }
    }
    SH_CHECK(dispatchedCount == 1);

    SH_CHECK(backend.WaitForEffectCount(1, 1s));
    SH_CHECK(backend.History().size() == 1);
}

SH_TEST(Pipeline_NormalBlockFixture_DispatchesNothing) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    ReplaySignalSource source(Fixture("normal_block.jsonl"));
    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;

    RunReplayLoop(source, pipeline, outcomes, malformed);

    for (const auto& outcome : outcomes) {
        SH_CHECK(outcome.stage == PipelineStage::NoEventDetected);
    }
    SH_CHECK(backend.History().empty());
}

SH_TEST(Pipeline_EventWithNoMapping_ReportsNoMappingForEvent) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    MappingRepository mappings; // intentionally empty: no mapping loaded
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.ProcessSignal(ManualSignal("manual.perfect_deflect", "true"), outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].stage == PipelineStage::EventDetected);
    SH_CHECK(outcomes[0].error == PipelineError::NoMappingForEvent);
}

SH_TEST(Pipeline_MappingReferencesMissingPreset_ReportsError) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok); // does not contain does_not_exist_v1
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings_missing_preset.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.ProcessSignal(ManualSignal("manual.take_damage", "true"), outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].stage == PipelineStage::MappingResolved);
    SH_CHECK(outcomes[0].error == PipelineError::MappingReferencesMissingPreset);
    SH_CHECK(outcomes[0].presetId.has_value());
    SH_CHECK(*outcomes[0].presetId == "does_not_exist_v1");
}

SH_TEST(Pipeline_BackendDisconnected_DoesNotDispatch) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    DisconnectedBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.ProcessSignal(ManualSignal("manual.perfect_deflect", "true"), outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].stage == PipelineStage::PresetResolved);
    SH_CHECK(outcomes[0].error == PipelineError::BackendNotConnected);
    SH_CHECK(!outcomes[0].scheduledId.has_value());
}

SH_TEST(Pipeline_Flush_ForwardsDetectorFlushEvents) {
    BufferingDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.Flush(outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].stage == PipelineStage::Dispatched);
    SH_CHECK(backend.WaitForEffectCount(1, 1s));
}

SH_TEST(Pipeline_MalformedTraceLine_IsSkippedAndReported) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    ReplaySignalSource source(Fixture("malformed_json.jsonl"));
    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;

    std::size_t count = RunReplayLoop(source, pipeline, outcomes, malformed);

    SH_CHECK(count == 1);
    SH_CHECK(malformed.size() == 1);
    SH_CHECK(malformed[0].find("line 2") != std::string::npos);
}
