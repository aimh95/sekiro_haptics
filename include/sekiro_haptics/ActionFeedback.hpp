#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sekiro_haptics {

// Semantic output commands, NOT memory observations or a Sekiro detector.
enum class SekiroAction {
    Heartbeat, Deflect, Block, Damage, PostureBreak, Deathblow,
    SelectProsthetic, UseProsthetic, StopProsthetic,
    GrappleLaunch, GrappleLatch, GrapplePull, GrappleEnd,
    Pause, Resume, Discontinuity
};
enum class Prosthetic {
    None, Shuriken, Axe, Spear, FlameVent, Firecracker, Umbrella,
    Sabimaru, MistRaven, FingerWhistle, DivineAbduction
};
enum class SpeakerCue { Deflect, Block, Impact, Metal, Fire, RopeLaunch, RopeLatch };

const char* ToString(SekiroAction action);
const char* ToString(Prosthetic tool);
const char* ToString(SpeakerCue cue);
std::optional<SekiroAction> ParseSekiroAction(std::string_view text);
std::optional<Prosthetic> ParseProsthetic(std::string_view text);

struct TriggerResistance {
    float start = 0.0f; // normalized travel position
    float force = 0.0f; // zero releases the trigger
};
struct FeedbackState {
    float leftMotor = 0.0f;
    float rightMotor = 0.0f;
    TriggerResistance leftTrigger;
    TriggerResistance rightTrigger;
};
struct FeedbackFrame {
    FeedbackState state;
    std::vector<SpeakerCue> cues; // one-shot; consumed by Tick, never by HID refresh
    bool stopAudio = false;
};
struct FeedbackOptions {
    float rumbleGain = 0.65f;
    float triggerGain = 0.55f;
    std::int64_t sourceTimeoutUs = 500'000;
};
struct ActionCommand {
    SekiroAction action = SekiroAction::Heartbeat;
    Prosthetic tool = Prosthetic::None;
    std::uint64_t sequence = 0; // increasing source occurrence ID; no time debounce
};

// Single-owner state machine. Submit and Tick run on the same output thread.
// No delayed Reset jobs: every frame recomputes the complete motor/trigger state.
class SekiroFeedbackEngine {
public:
    explicit SekiroFeedbackEngine(FeedbackOptions options = {});
    bool Submit(const ActionCommand& command, std::int64_t nowUs);
    FeedbackFrame Tick(std::int64_t nowUs);
    void Reset();
    Prosthetic SelectedTool() const { return selected_; }
private:
    struct Pulse { std::int64_t startUs, durationUs; float left, right; };
    FeedbackOptions options_;
    Prosthetic selected_ = Prosthetic::None;
    std::vector<Pulse> pulses_;
    struct PendingCue { SpeakerCue cue; std::int64_t timestampUs; };
    std::vector<PendingCue> cues_;
    std::optional<std::uint64_t> lastSequence_;
    std::optional<std::int64_t> lastInputUs_;
    std::int64_t clockUs_ = 0;
    std::int64_t useStartUs_ = -1;
    std::int64_t grappleStartUs_ = -1;
    SekiroAction grapplePhase_ = SekiroAction::GrappleEnd;
    bool paused_ = false;
    bool stopAudio_ = false;
    void PulseAt(std::int64_t now, std::int64_t duration, float left, float right);
    void Cue(SpeakerCue cue);
};

// Original procedural mono PCM. No game audio assets. Gain is [0,1].
std::vector<float> SynthesizeCue(SpeakerCue cue, std::uint32_t sampleRate, float gain);

} // namespace sekiro_haptics
