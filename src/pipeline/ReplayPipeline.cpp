#include "sekiro_haptics/pipeline/ReplayPipeline.hpp"

namespace sekiro_haptics {

const char* ToString(PipelineStage stage) {
    switch (stage) {
        case PipelineStage::NoEventDetected:
            return "NoEventDetected";
        case PipelineStage::EventDetected:
            return "EventDetected";
        case PipelineStage::MappingResolved:
            return "MappingResolved";
        case PipelineStage::PresetResolved:
            return "PresetResolved";
        case PipelineStage::Dispatched:
            return "Dispatched";
    }
    return "Unknown";
}

const char* ToString(PipelineError error) {
    switch (error) {
        case PipelineError::None:
            return "None";
        case PipelineError::NoMappingForEvent:
            return "NoMappingForEvent";
        case PipelineError::MappingReferencesMissingPreset:
            return "MappingReferencesMissingPreset";
        case PipelineError::BackendNotConnected:
            return "BackendNotConnected";
    }
    return "Unknown";
}

ReplayPipeline::ReplayPipeline(IGameEventDetector& detector, const PresetRepository& presets,
                                const MappingRepository& mappings, HapticScheduler& scheduler,
                                IHapticBackend& backend)
    : detector_(detector), presets_(presets), mappings_(mappings), scheduler_(scheduler), backend_(backend) {}

void ReplayPipeline::ProcessSignal(const GameSignal& signal, std::vector<PipelineStepOutcome>& outOutcomes) {
    std::vector<GameEvent> events;
    detector_.OnSignal(signal, events);

    if (events.empty()) {
        PipelineStepOutcome outcome;
        outcome.stage = PipelineStage::NoEventDetected;
        outcome.error = PipelineError::None;
        outOutcomes.push_back(std::move(outcome));
        return;
    }

    for (const GameEvent& event : events) {
        ResolveAndDispatch(event, outOutcomes);
    }
}

void ReplayPipeline::Flush(std::vector<PipelineStepOutcome>& outOutcomes) {
    std::vector<GameEvent> events;
    detector_.Flush(events);
    for (const GameEvent& event : events) {
        ResolveAndDispatch(event, outOutcomes);
    }
}

void ReplayPipeline::ResolveAndDispatch(const GameEvent& event, std::vector<PipelineStepOutcome>& outOutcomes) {
    PipelineStepOutcome outcome;
    outcome.stage = PipelineStage::EventDetected;
    outcome.event = event;

    const EventMapping* mapping = mappings_.Find(event.gameId, event.eventId);
    if (mapping == nullptr) {
        outcome.error = PipelineError::NoMappingForEvent;
        outcome.detail = "no mapping for " + event.gameId + "/" + event.eventId;
        outOutcomes.push_back(std::move(outcome));
        return;
    }

    outcome.stage = PipelineStage::MappingResolved;
    outcome.presetId = mapping->presetId;

    const HapticPreset* preset = presets_.Find(mapping->presetId);
    if (preset == nullptr) {
        outcome.error = PipelineError::MappingReferencesMissingPreset;
        outcome.detail = "mapping references unknown presetId \"" + mapping->presetId + "\"";
        outOutcomes.push_back(std::move(outcome));
        return;
    }

    outcome.stage = PipelineStage::PresetResolved;

    if (!backend_.IsConnected()) {
        outcome.error = PipelineError::BackendNotConnected;
        outcome.detail = "backend is not connected; effect not scheduled";
        outOutcomes.push_back(std::move(outcome));
        return;
    }

    HapticEffectId id = scheduler_.Schedule(preset->effect);
    outcome.stage = PipelineStage::Dispatched;
    outcome.scheduledId = id;
    outOutcomes.push_back(std::move(outcome));
}

std::size_t RunReplayLoop(IGameSignalSource& source, ReplayPipeline& pipeline,
                           std::vector<PipelineStepOutcome>& outOutcomes,
                           std::vector<std::string>& outMalformedLines) {
    std::size_t successCount = 0;
    GameSignal signal;

    while (true) {
        std::string error;
        SignalSourceResult result = source.Next(signal, &error);
        if (result == SignalSourceResult::EndOfSource) {
            break;
        }
        if (result == SignalSourceResult::MalformedInput) {
            outMalformedLines.push_back(error);
            continue;
        }

        ++successCount;
        pipeline.ProcessSignal(signal, outOutcomes);
    }

    pipeline.Flush(outOutcomes);
    return successCount;
}

} // namespace sekiro_haptics
