// Full-pipeline tests wiring DualSenseLegacyBackend + FakeDualSenseTransport
// into the real ReplayPipeline/RunReplayLoopStrict path -- the
// OUT-LEGACY-002 target path:
//
//   RunReplayLoopStrict -> ReplaySignalSource -> ManualLabelEventDetector ->
//   MappingRepository -> PresetRepository -> HapticScheduler ->
//   DualSenseLegacyBackend -> FakeDualSenseTransport
//
// No MockHapticBackend here -- assertions inspect raw report bytes the fake
// transport actually received. No fixed sleep -- FakeDualSenseTransport's
// condition-variable WaitForWriteCount() (mirroring
// MockHapticBackend::WaitForEffectCount) covers HapticScheduler's
// asynchronous dispatch.

#include "sekiro_haptics/DualSenseLegacyBackend.hpp"
#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "sekiro_haptics/pipeline/ReplayPipeline.hpp"
#include "testing.hpp"

#include "FakeDualSenseTransport.hpp"

#include <chrono>
#include <string>

using namespace sekiro_haptics;
using namespace std::chrono_literals;

namespace {

std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}

GameSignal ManualSignal(const std::string& name, const std::string& value) {
    GameSignal signal;
    signal.signal = name;
    signal.value = value;
    return signal;
}

} // namespace

SH_TEST(HardwareBackend_PositiveVersionedFixture_StartThenStopReport_NoDuplicateNonZero) {
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);

    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    bool replayed = RunReplayLoopStrict(Fixture("perfect_deflect_versioned.jsonl"), pipeline, outcomes, malformed,
                                         validationErrors);

    SH_CHECK(replayed);
    SH_CHECK(validationErrors.empty());

    int dispatchedCount = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.stage == PipelineStage::Dispatched) {
            ++dispatchedCount;
            SH_CHECK(outcome.presetId.has_value());
            SH_CHECK(*outcome.presetId == "sharp_metal_v1");
        }
    }
    SH_CHECK(dispatchedCount == 1); // exactly one event, mapping+preset each resolved exactly once

    SH_CHECK(transport.WaitForWriteCount(1, 1s));
    auto startReport = transport.WrittenReports()[0];
    SH_CHECK(startReport[3] != 0 || startReport[4] != 0); // start: non-zero motor bytes

    // No manual Reset() here -- sharp_metal_v1's durationMs=28 (see
    // presets.json) means HapticScheduler auto-stops this effect on its
    // own shortly after dispatch. This is the behavior this ticket adds:
    // the preset's own duration determines the stop, not this test/replay
    // completing.
    SH_CHECK(transport.WaitForWriteCount(2, 1s));
    SH_CHECK(transport.WriteCount() == 2); // start, then auto-stop -- no duplicate non-zero report
    auto stopReport = transport.WrittenReports()[1];
    SH_CHECK(stopReport[3] == 0);
    SH_CHECK(stopReport[4] == 0);
}

SH_TEST(HardwareBackend_NegativeNoDeflectFixture_IdleAttackBlock_ZeroHardwareWrites) {
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);

    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    bool replayed = RunReplayLoopStrict(Fixture("negative_no_deflect.jsonl"), pipeline, outcomes, malformed,
                                         validationErrors);

    SH_CHECK(replayed);
    SH_CHECK(validationErrors.empty());
    for (const auto& outcome : outcomes) {
        SH_CHECK(outcome.stage == PipelineStage::NoEventDetected);
    }
    SH_CHECK(transport.WriteCount() == 0);
}

SH_TEST(HardwareBackend_UnmappedEvent_ZeroHardwareWrites) {
    PresetRepository presets;
    MappingRepository mappings; // intentionally empty: no mapping loaded

    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.ProcessSignal(ManualSignal("manual.perfect_deflect", "true"), outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].error == PipelineError::NoMappingForEvent);
    SH_CHECK(transport.WriteCount() == 0);
}

SH_TEST(HardwareBackend_UnsupportedPreset_MappingReferencesMissingPreset_ZeroHardwareWrites) {
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok); // does not contain does_not_exist_v1
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings_missing_preset.json")).ok);

    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    pipeline.ProcessSignal(ManualSignal("manual.take_damage", "true"), outcomes);

    SH_CHECK(outcomes.size() == 1);
    SH_CHECK(outcomes[0].error == PipelineError::MappingReferencesMissingPreset);
    SH_CHECK(transport.WriteCount() == 0);
}

namespace {

void AssertStrictReplayRejectedWithZeroHardwareWrites(const std::string& tracePath) {
    PresetRepository presets;
    SH_CHECK(presets.LoadFromFile(Fixture("presets.json")).ok);
    MappingRepository mappings;
    SH_CHECK(mappings.LoadFromFile(Fixture("mappings.json")).ok);

    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformed;
    std::vector<std::string> validationErrors;

    bool replayed = RunReplayLoopStrict(tracePath, pipeline, outcomes, malformed, validationErrors);

    SH_CHECK(replayed == false);
    SH_CHECK(outcomes.empty());
    SH_CHECK(!validationErrors.empty());
    SH_CHECK(transport.WriteCount() == 0);
    SH_CHECK(transport.OpenAttempts() == 0); // the transport itself is never even touched
}

} // namespace

SH_TEST(HardwareBackend_NoSidecarTrace_StrictReplayRejected_ZeroHardwareWrites) {
    AssertStrictReplayRejectedWithZeroHardwareWrites(Fixture("no_sidecar_trace.jsonl"));
}

SH_TEST(HardwareBackend_MalformedTrace_StrictReplayRejected_ZeroHardwareWrites) {
    AssertStrictReplayRejectedWithZeroHardwareWrites(Fixture("malformed_json.jsonl"));
}

SH_TEST(HardwareBackend_OutOfOrderTimestampTrace_StrictReplayRejected_ZeroHardwareWrites) {
    AssertStrictReplayRejectedWithZeroHardwareWrites(Fixture("out_of_order.jsonl"));
}

SH_TEST(HardwareBackend_InvalidSignalValidityTrace_StrictReplayRejected_ZeroHardwareWrites) {
    AssertStrictReplayRejectedWithZeroHardwareWrites(Fixture("invalid_signal_validity.jsonl"));
}

SH_TEST(HardwareBackend_UnsupportedSchemaVersionTrace_StrictReplayRejected_ZeroHardwareWrites) {
    AssertStrictReplayRejectedWithZeroHardwareWrites(Fixture("unsupported_schema.jsonl"));
}
