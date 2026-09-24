#pragma once

// The stage that turns a detected GameEvent into output, without the detector
// knowing anything about HID or WASAPI.
//
//   GameSignal -> IGameEventDetector -> GameEvent
//                                          |
//                                   MappingRepository   (event -> presetId)
//                                          |
//                                OutputPresetRepository (presetId -> layers)
//                                          |
//                                     OutputRuntime     (this type)
//                                      |               |
//                              IPcmOutputSink   AdaptiveTriggerRuntime
//                             (speaker + PCM)    (L2/R2 effect lifetime)
//
// WHY THE SINKS ARE INTERFACES
// ----------------------------
// So this whole path is testable with no controller and no audio endpoint.
// The real PCM sink wraps DualSenseAudioDevice; the trigger side is already
// abstracted behind IDualSenseTransport, so the existing fake transport
// covers it.
//
// CORRELATION AND TIME
// --------------------
// Every accepted event gets a correlation id, and each stage that can
// actually be observed is stamped with the monotonic clock the caller passes
// in: when the runtime received the event, when the mapping resolved, when a
// layer was dispatched, when a PCM voice was queued, and when a HID report
// was submitted. The id is handed to the PCM sink so the mixer's own
// lifecycle record carries it too, which is what lets "this deflect produced
// these three voices and they all played to the end" be checked.
//
// WHAT THESE TIMES ARE NOT: the moment a sound was heard or a trigger moved.
// A PCM queue time is when a voice entered the mixer, and the mixer's
// firstRenderedUs is when its first sample went into a submitted buffer --
// both are before the audio engine's own buffer delay. A HID submit time is
// when hid_write returned. Neither is a physical onset, and nothing in this
// file reports one.

#include "sekiro_haptics/events/GameEvent.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"
#include "sekiro_haptics/dualsense/DualSenseOutputState.hpp"
#include "sekiro_haptics/presets/MappingRepository.hpp"
#include "sekiro_haptics/presets/OutputPresetRepository.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace sekiro_haptics {

/// Where the PCM layers go. The real implementation wraps
/// DualSenseAudioDevice; tests use a recording double.
class IPcmOutputSink {
public:
    virtual ~IPcmOutputSink() = default;

    /// True when `cueId` names a clip this sink can play. Checked at bind
    /// time so a preset referring to a missing clip is rejected before any
    /// event arrives, rather than silently producing nothing mid-fight.
    virtual bool HasCue(const std::string& cueId) const = 0;

    /// Queue onto the speaker path. Returns false if the cue is unknown or
    /// the sink has no speaker channel configured.
    virtual bool QueueSpeaker(const std::string& cueId, float gain,
                              std::uint64_t correlationId) = 0;

    /// Queue onto the voice-coil path, with a left/right balance.
    virtual bool QueueHaptic(const std::string& cueId, float gain, float balance,
                             std::uint64_t correlationId) = 0;
};

/// What happened to one event.
enum class DispatchOutcome {
    Dispatched,
    /// No mapping for (gameId, eventId). Not an error -- an event this
    /// profile does not care about.
    NoMapping,
    /// The mapping names a presetId the preset repository does not have.
    NoPreset,
    /// The event reached the runtime later than the configured budget and was
    /// dropped rather than played late.
    DroppedLate,
    /// Every layer failed to dispatch.
    Failed,
};

const char* ToString(DispatchOutcome outcome);

/// Stage times for one event, all on the caller's monotonic clock and all in
/// microseconds. A zero means "this stage did not happen", never "time zero".
struct OutputStageTimes {
    /// The detector's own timestamp for the event.
    std::int64_t eventUs = 0;
    std::int64_t receivedUs = 0;
    std::int64_t mappedUs = 0;
    /// When the last layer of this event was dispatched. Layers with a start
    /// offset move this later.
    std::int64_t lastDispatchUs = 0;
    /// First PCM voice queued, and first HID report submitted, for this
    /// event. Kept separate because the two paths do not travel together:
    /// a HID write returns when the USB write returns, while a PCM voice
    /// waits for the mixer's next buffer.
    std::int64_t firstPcmQueuedUs = 0;
    std::int64_t firstHidSubmitUs = 0;
};

struct DispatchRecord {
    std::uint64_t correlationId = 0;
    std::string gameId;
    std::string eventId;
    std::string presetId;
    OutputStageTimes times;
    std::size_t layersTotal = 0;
    std::size_t layersDispatched = 0;
    /// Layers with a start offset, still waiting for Tick() to reach them.
    std::size_t layersPending = 0;
    std::size_t layersFailed = 0;
    DispatchOutcome outcome = DispatchOutcome::Dispatched;
    std::string note;
};

struct OutputRuntimeStats {
    std::uint64_t eventsReceived = 0;
    std::uint64_t eventsDispatched = 0;
    std::uint64_t eventsWithoutMapping = 0;
    std::uint64_t eventsWithoutPreset = 0;
    std::uint64_t eventsDroppedLate = 0;
    std::uint64_t speakerLayersQueued = 0;
    std::uint64_t hapticLayersQueued = 0;
    std::uint64_t triggerLayersApplied = 0;
    std::uint64_t legacyRumbleLayersApplied = 0;
    std::uint64_t layerFailures = 0;
    /// Layers still waiting on their start offset right now.
    std::size_t pendingLayers = 0;
};

struct OutputRuntimeConfig {
    /// An event older than this when it reaches the runtime is dropped rather
    /// than played late. Matches the existing live path's default and its
    /// reasoning: a stale hit played now is worse than no hit.
    std::int64_t maxOutputLatencyUs = 120'000;
    /// Bound on the dispatch history kept for tracing.
    std::size_t historyLimit = 256;
};

/// One binding problem found by Bind().
struct BindProblem {
    std::string gameId;
    std::string eventId;
    std::string presetId;
    std::string message;
};

class OutputRuntime {
public:
    /// `triggers` and `hidState` may be null when a caller has no controller
    /// attached (PCM-only runs); the corresponding layers then fail to
    /// dispatch and are counted, rather than being silently skipped.
    OutputRuntime(const MappingRepository& mappings, const OutputPresetRepository& presets,
                  IPcmOutputSink* pcm, dualsense::AdaptiveTriggerRuntime* triggers,
                  dualsense::DualSenseOutputState* hidState, OutputRuntimeConfig config = {});

    /// Checks, for every mapping, that its preset exists and that every cue
    /// it names resolves in the PCM sink. Returns the problems found; an
    /// empty result means every mapped event can actually be produced.
    ///
    /// This runs before any event, which is the whole point: a missing clip
    /// should be a startup error, not a silent no-op during a fight.
    std::vector<BindProblem> Bind() const;

    /// Handles one event. Layers with no start offset are dispatched
    /// immediately; the rest wait for Tick().
    DispatchRecord Handle(const GameEvent& event, std::int64_t nowUs);

    /// Dispatches any layer whose start offset has arrived, and ticks the
    /// trigger runtime so expiries land. Safe to call on every poll.
    void Tick(std::int64_t nowUs);

    /// Drops every layer that has not been dispatched yet. For player-object
    /// replacement, device loss and shutdown -- the same situations in which
    /// the mixer's DropPending() is called. Already-sounding voices are NOT
    /// touched here; ending them is the sink's decision, not this one's.
    void DropPending();

    OutputRuntimeStats Stats() const;
    std::vector<DispatchRecord> History() const;
    /// The record for one correlation id, or nullptr if it has aged out.
    const DispatchRecord* FindRecord(std::uint64_t correlationId) const;

private:
    struct PendingLayer {
        std::uint64_t correlationId = 0;
        std::int64_t dueUs = 0;
        enum class Kind { Speaker, PcmHaptic, Trigger, LegacyRumble } kind = Kind::Speaker;
        SpeakerOutputLayer speaker;
        PcmHapticOutputLayer haptic;
        TriggerOutputLayer trigger;
        LegacyRumbleOutputLayer rumble;
    };

    bool DispatchSpeaker(const SpeakerOutputLayer& layer, DispatchRecord& record, std::int64_t nowUs);
    bool DispatchHaptic(const PcmHapticOutputLayer& layer, DispatchRecord& record, std::int64_t nowUs);
    bool DispatchTrigger(const TriggerOutputLayer& layer, DispatchRecord& record, std::int64_t nowUs);
    bool DispatchRumble(const LegacyRumbleOutputLayer& layer, DispatchRecord& record, std::int64_t nowUs);
    DispatchRecord* MutableRecord(std::uint64_t correlationId);
    void Remember(DispatchRecord record);

    const MappingRepository& mappings_;
    const OutputPresetRepository& presets_;
    IPcmOutputSink* pcm_;
    dualsense::AdaptiveTriggerRuntime* triggers_;
    dualsense::DualSenseOutputState* hidState_;
    OutputRuntimeConfig config_;

    std::uint64_t nextCorrelationId_ = 1;
    std::deque<PendingLayer> pending_;
    std::deque<DispatchRecord> history_;
    OutputRuntimeStats stats_;
};

} // namespace sekiro_haptics
