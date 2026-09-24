#include "sekiro_haptics/runtime/OutputRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace sekiro_haptics {

namespace {

std::int64_t OffsetToUs(float offsetMs) {
    if (!std::isfinite(offsetMs) || offsetMs <= 0.0f) return 0;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(offsetMs) * 1000.0));
}

} // namespace

const char* ToString(DispatchOutcome outcome) {
    switch (outcome) {
        case DispatchOutcome::Dispatched: return "dispatched";
        case DispatchOutcome::NoMapping: return "no-mapping";
        case DispatchOutcome::NoPreset: return "no-preset";
        case DispatchOutcome::DroppedLate: return "dropped-late";
        case DispatchOutcome::Failed: return "failed";
    }
    return "unknown";
}

OutputRuntime::OutputRuntime(const MappingRepository& mappings,
                             const OutputPresetRepository& presets, IPcmOutputSink* pcm,
                             dualsense::AdaptiveTriggerRuntime* triggers,
                             dualsense::DualSenseOutputState* hidState,
                             OutputRuntimeConfig config)
    : mappings_(mappings), presets_(presets), pcm_(pcm), triggers_(triggers),
      hidState_(hidState), config_(config) {}

std::vector<BindProblem> OutputRuntime::Bind() const {
    std::vector<BindProblem> problems;
    for (const auto& id : presets_.PresetIds()) {
        const auto* preset = presets_.Find(id);
        if (preset == nullptr) continue;   // cannot happen; keeps the loop total
        if (pcm_ == nullptr) {
            if (!preset->speaker.empty() || !preset->pcmHaptic.empty())
                problems.push_back({{}, {}, id, "preset has PCM layers but no PCM sink is attached"});
            continue;
        }
        for (const auto& layer : preset->speaker)
            if (!pcm_->HasCue(layer.cueId))
                problems.push_back({{}, {}, id, "speaker cue \"" + layer.cueId + "\" does not resolve"});
        for (const auto& layer : preset->pcmHaptic)
            if (!pcm_->HasCue(layer.cueId))
                problems.push_back({{}, {}, id, "pcmHaptic cue \"" + layer.cueId + "\" does not resolve"});
        if (!preset->trigger.empty() && triggers_ == nullptr)
            problems.push_back({{}, {}, id, "preset has trigger layers but no trigger runtime is attached"});
        if (preset->legacyRumble && hidState_ == nullptr)
            problems.push_back({{}, {}, id, "preset has a legacyRumble layer but no HID output state is attached"});
    }
    return problems;
}

DispatchRecord* OutputRuntime::MutableRecord(std::uint64_t correlationId) {
    for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        if (it->correlationId == correlationId) return &*it;
    return nullptr;
}

void OutputRuntime::Remember(DispatchRecord record) {
    history_.push_back(std::move(record));
    while (history_.size() > config_.historyLimit) history_.pop_front();
}

bool OutputRuntime::DispatchSpeaker(const SpeakerOutputLayer& layer, DispatchRecord& record,
                                    std::int64_t nowUs) {
    if (pcm_ == nullptr) return false;
    if (!pcm_->QueueSpeaker(layer.cueId, layer.gain, record.correlationId)) return false;
    ++stats_.speakerLayersQueued;
    if (record.times.firstPcmQueuedUs == 0) record.times.firstPcmQueuedUs = nowUs;
    return true;
}

bool OutputRuntime::DispatchHaptic(const PcmHapticOutputLayer& layer, DispatchRecord& record,
                                   std::int64_t nowUs) {
    if (pcm_ == nullptr) return false;
    if (!pcm_->QueueHaptic(layer.cueId, layer.gain, layer.balance, record.correlationId)) return false;
    ++stats_.hapticLayersQueued;
    if (record.times.firstPcmQueuedUs == 0) record.times.firstPcmQueuedUs = nowUs;
    return true;
}

bool OutputRuntime::DispatchTrigger(const TriggerOutputLayer& layer, DispatchRecord& record,
                                    std::int64_t nowUs) {
    if (triggers_ == nullptr) return false;
    const auto durationUs = layer.durationMs > 0.0f
                                ? static_cast<std::int64_t>(std::llround(
                                      static_cast<double>(layer.durationMs) * 1000.0))
                                : dualsense::kHoldUntilReplaced;
    const auto applied = triggers_->Apply(layer.side, layer.spec, durationUs, nowUs);
    if (!applied.accepted) {
        record.note = applied.error;
        return false;
    }
    ++stats_.triggerLayersApplied;
    // The HID write has returned by now. That is a submission time, not the
    // instant the trigger's motor moved.
    if (record.times.firstHidSubmitUs == 0) record.times.firstHidSubmitUs = nowUs;
    return true;
}

bool OutputRuntime::DispatchRumble(const LegacyRumbleOutputLayer& layer, DispatchRecord& record,
                                   std::int64_t nowUs) {
    if (hidState_ == nullptr) return false;
    const auto toByte = [](float intensity) {
        if (!std::isfinite(intensity)) return static_cast<std::uint8_t>(0);
        return static_cast<std::uint8_t>(std::lround(std::clamp(intensity, 0.0f, 1.0f) * 255.0f));
    };
    // NOTE: claiming the motors moves the controller off the PCM haptic path
    // (DualSenseOutputState.hpp). A preset only gets here because it asked
    // for a legacyRumble layer explicitly.
    hidState_->SetLegacyRumble(toByte(layer.effect.intensity.left),
                               toByte(layer.effect.intensity.right));
    ++stats_.legacyRumbleLayersApplied;
    if (record.times.firstHidSubmitUs == 0) record.times.firstHidSubmitUs = nowUs;
    return true;
}

DispatchRecord OutputRuntime::Handle(const GameEvent& event, std::int64_t nowUs) {
    ++stats_.eventsReceived;

    DispatchRecord record;
    record.correlationId = nextCorrelationId_++;
    record.gameId = event.gameId;
    record.eventId = event.eventId;
    record.times.eventUs = event.timestamp.count();
    record.times.receivedUs = nowUs;

    const auto age = nowUs - record.times.eventUs;
    if (record.times.eventUs != 0 && age > config_.maxOutputLatencyUs) {
        record.outcome = DispatchOutcome::DroppedLate;
        std::ostringstream note;
        note << "event was " << age / 1000 << " ms old, budget " << config_.maxOutputLatencyUs / 1000
             << " ms";
        record.note = note.str();
        ++stats_.eventsDroppedLate;
        Remember(record);
        return record;
    }

    const auto* mapping = mappings_.Find(event.gameId, event.eventId);
    if (mapping == nullptr) {
        record.outcome = DispatchOutcome::NoMapping;
        ++stats_.eventsWithoutMapping;
        Remember(record);
        return record;
    }
    record.presetId = mapping->presetId;
    record.times.mappedUs = nowUs;

    const auto* preset = presets_.Find(mapping->presetId);
    if (preset == nullptr) {
        record.outcome = DispatchOutcome::NoPreset;
        record.note = "mapping names preset \"" + mapping->presetId + "\", which is not loaded";
        ++stats_.eventsWithoutPreset;
        Remember(record);
        return record;
    }
    record.layersTotal = preset->LayerCount();

    // Each layer starts on its own schedule. A layer with no offset goes out
    // now; one with an offset is queued and nothing else waits for it.
    auto schedule = [&](PendingLayer layer, float offsetMs) {
        const auto offsetUs = OffsetToUs(offsetMs);
        if (offsetUs == 0) return false;
        layer.correlationId = record.correlationId;
        layer.dueUs = nowUs + offsetUs;
        pending_.push_back(std::move(layer));
        ++record.layersPending;
        return true;
    };

    for (const auto& layer : preset->speaker) {
        PendingLayer pendingLayer;
        pendingLayer.kind = PendingLayer::Kind::Speaker;
        pendingLayer.speaker = layer;
        if (schedule(pendingLayer, layer.startOffsetMs)) continue;
        if (DispatchSpeaker(layer, record, nowUs)) ++record.layersDispatched;
        else { ++record.layersFailed; ++stats_.layerFailures; }
    }
    for (const auto& layer : preset->pcmHaptic) {
        PendingLayer pendingLayer;
        pendingLayer.kind = PendingLayer::Kind::PcmHaptic;
        pendingLayer.haptic = layer;
        if (schedule(pendingLayer, layer.startOffsetMs)) continue;
        if (DispatchHaptic(layer, record, nowUs)) ++record.layersDispatched;
        else { ++record.layersFailed; ++stats_.layerFailures; }
    }
    for (const auto& layer : preset->trigger) {
        PendingLayer pendingLayer;
        pendingLayer.kind = PendingLayer::Kind::Trigger;
        pendingLayer.trigger = layer;
        if (schedule(pendingLayer, layer.startOffsetMs)) continue;
        if (DispatchTrigger(layer, record, nowUs)) ++record.layersDispatched;
        else { ++record.layersFailed; ++stats_.layerFailures; }
    }
    if (preset->legacyRumble) {
        PendingLayer pendingLayer;
        pendingLayer.kind = PendingLayer::Kind::LegacyRumble;
        pendingLayer.rumble = *preset->legacyRumble;
        if (!schedule(pendingLayer, preset->legacyRumble->startOffsetMs)) {
            if (DispatchRumble(*preset->legacyRumble, record, nowUs)) ++record.layersDispatched;
            else { ++record.layersFailed; ++stats_.layerFailures; }
        }
    }

    record.times.lastDispatchUs = nowUs;
    // "Failed" only when nothing at all got out AND nothing is waiting --
    // an event with a deferred layer has not failed, it has not happened yet.
    if (record.layersDispatched == 0 && record.layersPending == 0 && record.layersFailed > 0)
        record.outcome = DispatchOutcome::Failed;
    else {
        record.outcome = DispatchOutcome::Dispatched;
        ++stats_.eventsDispatched;
    }

    stats_.pendingLayers = pending_.size();
    Remember(record);
    return record;
}

void OutputRuntime::Tick(std::int64_t nowUs) {
    // Trigger expiries first: an effect whose lifetime ran out should be
    // released before a new layer possibly replaces it, so the stats
    // attribute the release to expiry rather than to replacement.
    if (triggers_ != nullptr) triggers_->Tick(nowUs);

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowUs < it->dueUs) { ++it; continue; }

        auto* record = MutableRecord(it->correlationId);
        DispatchRecord scratch;   // used when the record has already aged out
        auto& target = record != nullptr ? *record : scratch;
        if (record == nullptr) target.correlationId = it->correlationId;

        bool ok = false;
        switch (it->kind) {
            case PendingLayer::Kind::Speaker: ok = DispatchSpeaker(it->speaker, target, nowUs); break;
            case PendingLayer::Kind::PcmHaptic: ok = DispatchHaptic(it->haptic, target, nowUs); break;
            case PendingLayer::Kind::Trigger: ok = DispatchTrigger(it->trigger, target, nowUs); break;
            case PendingLayer::Kind::LegacyRumble: ok = DispatchRumble(it->rumble, target, nowUs); break;
        }
        if (ok) ++target.layersDispatched;
        else { ++target.layersFailed; ++stats_.layerFailures; }
        if (target.layersPending > 0) --target.layersPending;
        target.times.lastDispatchUs = nowUs;

        it = pending_.erase(it);
    }
    stats_.pendingLayers = pending_.size();
}

void OutputRuntime::DropPending() {
    for (const auto& layer : pending_) {
        if (auto* record = MutableRecord(layer.correlationId)) {
            if (record->layersPending > 0) --record->layersPending;
            record->note = "pending layers dropped";
        }
    }
    pending_.clear();
    stats_.pendingLayers = 0;
}

OutputRuntimeStats OutputRuntime::Stats() const { return stats_; }

std::vector<DispatchRecord> OutputRuntime::History() const {
    return {history_.begin(), history_.end()};
}

const DispatchRecord* OutputRuntime::FindRecord(std::uint64_t correlationId) const {
    for (auto it = history_.rbegin(); it != history_.rend(); ++it)
        if (it->correlationId == correlationId) return &*it;
    return nullptr;
}

} // namespace sekiro_haptics
