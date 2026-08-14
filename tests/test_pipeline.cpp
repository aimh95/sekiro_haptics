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

SH_TEST(Pipeline_NegativeNoDeflectFixture_IdleAttackBlock_DispatchesNothing) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    ReplaySignalSource source(Fixture("negative_no_deflect.jsonl"));
    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;

    RunReplayLoop(source, pipeline, outcomes, malformed);

    SH_CHECK(malformed.empty());
    for (const auto& outcome : outcomes) {
        SH_CHECK(outcome.stage == PipelineStage::NoEventDetected);
    }
    SH_CHECK(backend.History().empty());
}

// --- Failure safety: RunReplayLoopStrict must make zero pipeline/backend
// calls for any of these trace categories, validating the whole trace
// (and its metadata sidecar) before ever touching the detector/scheduler/
// backend. This is a stronger guarantee than RunReplayLoop's per-line
// tolerance (see Pipeline_MalformedTraceLine_IsSkippedAndReported above),
// and deliberately coexists with it rather than replacing it.

namespace {

void AssertStrictReplayRejected(const std::string& tracePath) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    bool replayed = RunReplayLoopStrict(tracePath, pipeline, outcomes, malformed, validationErrors);

    SH_CHECK(replayed == false);
    SH_CHECK(outcomes.empty());
    SH_CHECK(!validationErrors.empty());
    SH_CHECK(backend.History().empty());
    SH_CHECK(backend.ResetCount() == 0);
}

} // namespace

SH_TEST(Pipeline_StrictReplay_MalformedJsonTrace_MakesZeroBackendCalls) {
    AssertStrictReplayRejected(Fixture("malformed_json.jsonl"));
}

SH_TEST(Pipeline_StrictReplay_UnsupportedSchemaVersionTrace_MakesZeroBackendCalls) {
    AssertStrictReplayRejected(Fixture("unsupported_schema.jsonl"));
}

SH_TEST(Pipeline_StrictReplay_OutOfOrderTimestampTrace_MakesZeroBackendCalls) {
    AssertStrictReplayRejected(Fixture("out_of_order.jsonl"));
}

SH_TEST(Pipeline_StrictReplay_MissingRequiredMetadataTrace_MakesZeroBackendCalls) {
    AssertStrictReplayRejected(Fixture("missing_metadata.jsonl"));
}

SH_TEST(Pipeline_StrictReplay_InvalidSignalValidityTrace_MakesZeroBackendCalls) {
    AssertStrictReplayRejected(Fixture("invalid_signal_validity.jsonl"));
}

// The specific policy fix this ticket makes: a trace with NO metadata
// sidecar at all must be rejected by RunReplayLoopStrict -- not silently
// treated as legacy v1 -- with zero pipeline/backend calls, exactly like
// every other invalid-trace category above.
SH_TEST(Pipeline_StrictReplay_NoSidecarAtAll_FailsWithMissingMetadataAndZeroBackendCalls) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    bool replayed =
        RunReplayLoopStrict(Fixture("no_sidecar_trace.jsonl"), pipeline, outcomes, malformed, validationErrors);

    SH_CHECK(replayed == false);
    SH_CHECK(outcomes.empty());       // detector/mapping/preset never invoked
    SH_CHECK(malformed.empty());      // never even got to reading the body
    SH_CHECK(backend.History().empty());
    SH_CHECK(backend.ResetCount() == 0);
    SH_CHECK(validationErrors.size() == 1);
    SH_CHECK(validationErrors[0].find("MissingMetadata") != std::string::npos);
}

SH_TEST(Pipeline_StrictReplay_ValidTrace_ActuallyReplaysAndDispatches) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    // perfect_deflect_versioned.jsonl mirrors perfect_deflect.jsonl but
    // carries a valid metadata sidecar -- perfect_deflect.jsonl itself must
    // stay sidecar-less, since it's the fixture used elsewhere to prove the
    // legacy path still works (see Pipeline_LegacyCompatibilityPath_... below
    // and ValidateTraceFile_AllowLegacy_NoSidecar_IsValid).
    bool replayed = RunReplayLoopStrict(Fixture("perfect_deflect_versioned.jsonl"), pipeline, outcomes, malformed,
                                         validationErrors);

    SH_CHECK(replayed);
    SH_CHECK(validationErrors.empty());
    SH_CHECK(backend.WaitForEffectCount(1, 1s));
    SH_CHECK(backend.History().size() == 1);
}

// The explicit legacy-compatibility opt-in this ticket requires: a caller
// that wants to keep accepting a trace with no metadata sidecar chooses
// RunReplayLoop directly (never RunReplayLoopStrict, which always rejects
// one) -- there is no auto-detection deciding this for them.
SH_TEST(Pipeline_LegacyCompatibilityPath_RunReplayLoopWithNoSidecarTrace_StillDispatchesNormally) {
    ManualLabelEventDetector detector;
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    ReplaySignalSource source(Fixture("perfect_deflect.jsonl")); // no sidecar
    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;

    std::size_t count = RunReplayLoop(source, pipeline, outcomes, malformed);

    SH_CHECK(count == 4);
    SH_CHECK(malformed.empty());
    SH_CHECK(backend.WaitForEffectCount(1, 1s));
    SH_CHECK(backend.History().size() == 1);
}
