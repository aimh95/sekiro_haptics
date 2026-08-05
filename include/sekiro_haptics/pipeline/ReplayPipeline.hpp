#pragma once

#include "sekiro_haptics/HapticScheduler.hpp"
#include "sekiro_haptics/IHapticBackend.hpp"
#include "sekiro_haptics/events/GameEvent.hpp"
#include "sekiro_haptics/events/IGameEventDetector.hpp"
#include "sekiro_haptics/presets/MappingRepository.hpp"
#include "sekiro_haptics/presets/PresetRepository.hpp"
#include "sekiro_haptics/signals/GameSignal.hpp"
#include "sekiro_haptics/signals/IGameSignalSource.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace sekiro_haptics {

/// How far a single GameEvent got through the pipeline. Stages are ordered:
/// reaching a later stage implies every earlier one succeeded.
enum class PipelineStage {
    NoEventDetected,
    EventDetected,
    MappingResolved,
    PresetResolved,
    Dispatched,
};
const char* ToString(PipelineStage stage);

/// Why a PipelineStepOutcome stopped short of Dispatched. `None` is used
/// both for a successful Dispatched outcome and for a normal
/// NoEventDetected outcome -- neither is an error.
enum class PipelineError {
    None,
    NoMappingForEvent,
    MappingReferencesMissingPreset,
    BackendNotConnected,
};
const char* ToString(PipelineError error);

/// Record of what happened to one signal (if it produced no event) or one
/// detected event (if it did), through mapping/preset resolution and
/// scheduling.
struct PipelineStepOutcome {
    PipelineStage stage = PipelineStage::NoEventDetected;
    PipelineError error = PipelineError::None;
    std::optional<GameEvent> event;
    std::optional<std::string> presetId;
    std::optional<HapticEffectId> scheduledId;
    std::string detail;
};

/// Wires signal -> event detector -> mapping lookup -> preset lookup ->
/// scheduler -> backend, one signal at a time.
///
/// ReplayPipeline does not know or care whether the signals it's given
/// came from a real game or a replay file -- it only depends on
/// IGameEventDetector, PresetRepository, MappingRepository, HapticScheduler,
/// and IHapticBackend, none of which are replay-specific. See
/// docs/01-architecture.md.
class ReplayPipeline {
public:
    /// None of the referenced objects are owned; all must outlive this
    /// pipeline.
    ReplayPipeline(IGameEventDetector& detector, const PresetRepository& presets,
                   const MappingRepository& mappings, HapticScheduler& scheduler, IHapticBackend& backend);

    /// Runs one signal through the detector and, for each emitted event,
    /// resolves and dispatches it, appending one PipelineStepOutcome per
    /// event. If the detector emits nothing for this signal, appends
    /// exactly one outcome with stage=NoEventDetected, error=None -- that
    /// is a normal, expected outcome, not an error.
    void ProcessSignal(const GameSignal& signal, std::vector<PipelineStepOutcome>& outOutcomes);

    /// Drains the detector's Flush() and resolves/dispatches any events it
    /// emits, the same way ProcessSignal does.
    void Flush(std::vector<PipelineStepOutcome>& outOutcomes);

private:
    void ResolveAndDispatch(const GameEvent& event, std::vector<PipelineStepOutcome>& outOutcomes);

    IGameEventDetector& detector_;
    const PresetRepository& presets_;
    const MappingRepository& mappings_;
    HapticScheduler& scheduler_;
    IHapticBackend& backend_;
};

/// Drains `source` fully: calls ProcessSignal() for every successfully-read
/// signal, then Flush() once at the end.
///
/// A signal the source reports as MalformedInput is recorded (with its
/// error text) into `outMalformedLines` and skipped -- not fatal, so one
/// bad trace line doesn't lose the rest of the replay. Returns the count of
/// signals successfully read (Success results only).
std::size_t RunReplayLoop(IGameSignalSource& source, ReplayPipeline& pipeline,
                           std::vector<PipelineStepOutcome>& outOutcomes,
                           std::vector<std::string>& outMalformedLines);

} // namespace sekiro_haptics
