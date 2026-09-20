#include "sekiro_haptics/ActionFeedback.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace sekiro_haptics {
namespace {
float Unit(float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f; }
constexpr std::array<const char*, 17> kActions{
    "heartbeat", "deflect", "block", "damage", "posture_break", "deathblow",
    "select", "use", "use_end", "grapple_launch", "grapple_latch", "grapple_pull",
    "grapple_end", "pause", "resume", "discontinuity", nullptr};
constexpr std::array<const char*, 11> kTools{
    "none", "shuriken", "axe", "spear", "flame_vent", "firecracker", "umbrella",
    "sabimaru", "mist_raven", "finger_whistle", "divine_abduction"};
struct ToolProfile { float start, idle, recoil, left, right; std::int64_t duration; SpeakerCue cue; };
// Authored sensations, not measured game physics. All durations are bounded.
constexpr std::array<ToolProfile, 11> kProfiles{{
    {0,0,0,0,0,0,SpeakerCue::Metal},
    {.55f,.13f,.40f,.18f,.55f,160'000,SpeakerCue::Metal},
    {.20f,.48f,.90f,.90f,.50f,360'000,SpeakerCue::Impact},
    {.35f,.28f,.65f,.50f,.60f,280'000,SpeakerCue::Metal},
    {.18f,.20f,.48f,.35f,.55f,600'000,SpeakerCue::Fire},
    {.60f,.12f,.55f,.45f,.75f,220'000,SpeakerCue::Impact},
    {.15f,.38f,.70f,.50f,.28f,650'000,SpeakerCue::Metal},
    {.50f,.16f,.42f,.25f,.55f,240'000,SpeakerCue::Metal},
    {.65f,.10f,.25f,.18f,.32f,300'000,SpeakerCue::Fire},
    {.65f,.08f,.18f,.12f,.20f,180'000,SpeakerCue::Metal},
    {.30f,.22f,.40f,.25f,.38f,350'000,SpeakerCue::Fire}
}};
std::size_t ToolIndex(Prosthetic tool) { return static_cast<std::size_t>(tool); }
}

const char* ToString(SekiroAction a) {
    const auto i = static_cast<std::size_t>(a);
    return i < kActions.size()-1 ? kActions[i] : "unknown";
}
const char* ToString(Prosthetic t) {
    const auto i = ToolIndex(t); return i < kTools.size() ? kTools[i] : "unknown";
}
const char* ToString(SpeakerCue c) {
    switch (c) {
        case SpeakerCue::Deflect: return "deflect";
        case SpeakerCue::Block: return "block";
        case SpeakerCue::Impact: return "impact";
        case SpeakerCue::Metal: return "metal";
        case SpeakerCue::Fire: return "fire";
        case SpeakerCue::RopeLaunch: return "rope_launch";
        case SpeakerCue::RopeLatch: return "rope_latch";
    }
    return "unknown";
}
std::optional<SekiroAction> ParseSekiroAction(std::string_view text) {
    for (std::size_t i=0; i<kActions.size()-1; ++i)
        if (text == kActions[i]) return static_cast<SekiroAction>(i);
    return std::nullopt;
}
std::optional<Prosthetic> ParseProsthetic(std::string_view text) {
    for (std::size_t i=0; i<kTools.size(); ++i)
        if (text == kTools[i]) return static_cast<Prosthetic>(i);
    return std::nullopt;
}

SekiroFeedbackEngine::SekiroFeedbackEngine(FeedbackOptions options) : options_(options) {
    options_.rumbleGain = Unit(options_.rumbleGain);
    options_.triggerGain = Unit(options_.triggerGain);
    options_.sourceTimeoutUs = std::clamp<std::int64_t>(options_.sourceTimeoutUs, 50'000, 2'000'000);
}
void SekiroFeedbackEngine::Reset() {
    selected_ = Prosthetic::None;
    pulses_.clear(); cues_.clear(); useStartUs_ = grappleStartUs_ = -1;
    grapplePhase_ = SekiroAction::GrappleEnd;
    lastInputUs_.reset(); stopAudio_ = true;
    // Keep sequence watermark: stale queued commands cannot revive effects.
}
void SekiroFeedbackEngine::PulseAt(std::int64_t now, std::int64_t duration, float left, float right) {
    if (pulses_.size() == 16) pulses_.erase(pulses_.begin());
    pulses_.push_back({now,duration,left,right});
}
void SekiroFeedbackEngine::Cue(SpeakerCue cue) {
    if (cues_.size() == 16) cues_.erase(cues_.begin());
    cues_.push_back({cue,clockUs_});
}
bool SekiroFeedbackEngine::Submit(const ActionCommand& c, std::int64_t now) {
    if (now < clockUs_ || (lastSequence_ && c.sequence <= *lastSequence_) ||
        static_cast<std::size_t>(c.action) >= kActions.size()-1 || ToolIndex(c.tool) >= kTools.size())
        return false;
    clockUs_ = now;
    if (lastInputUs_ && now - *lastInputUs_ > options_.sourceTimeoutUs) Reset();
    lastSequence_ = c.sequence;
    lastInputUs_ = now;
    if (c.action == SekiroAction::Pause || c.action == SekiroAction::Discontinuity) {
        Reset(); paused_ = true; return true;
    }
    if (c.action == SekiroAction::Resume) { Reset(); paused_ = false; lastInputUs_ = now; return true; }
    if (paused_ || c.action == SekiroAction::Heartbeat) return true;
    switch (c.action) {
        case SekiroAction::Deflect:
            PulseAt(now,100'000,.70f,1.0f); Cue(SpeakerCue::Deflect); break;
        case SekiroAction::Block:
            PulseAt(now,150'000,.32f,.24f); Cue(SpeakerCue::Block); break;
        case SekiroAction::Damage:
            PulseAt(now,260'000,.85f,.42f); Cue(SpeakerCue::Impact); break;
        case SekiroAction::PostureBreak:
            PulseAt(now,350'000,.85f,.70f); Cue(SpeakerCue::Metal); break;
        case SekiroAction::Deathblow:
            PulseAt(now,420'000,1.0f,.65f); Cue(SpeakerCue::Impact); break;
        case SekiroAction::SelectProsthetic:
            selected_ = c.tool; useStartUs_ = -1; break;
        case SekiroAction::UseProsthetic: {
            if (selected_ == Prosthetic::None) return true;
            useStartUs_ = now;
            const auto& p = kProfiles[ToolIndex(selected_)];
            PulseAt(now,p.duration,p.left,p.right); Cue(p.cue); break;
        }
        case SekiroAction::StopProsthetic: useStartUs_ = -1; break;
        case SekiroAction::GrappleLaunch:
            grapplePhase_ = c.action; grappleStartUs_ = now;
            PulseAt(now,120'000,.16f,.24f); Cue(SpeakerCue::RopeLaunch); break;
        case SekiroAction::GrappleLatch:
            if (grapplePhase_ != SekiroAction::GrappleLaunch) return true;
            grapplePhase_ = c.action; grappleStartUs_ = now;
            PulseAt(now,110'000,.65f,.32f); Cue(SpeakerCue::RopeLatch); break;
        case SekiroAction::GrapplePull:
            if (grapplePhase_ != SekiroAction::GrappleLatch) return true;
            grapplePhase_ = c.action; grappleStartUs_ = now; break;
        case SekiroAction::GrappleEnd:
            grapplePhase_ = c.action; grappleStartUs_ = -1; break;
        default: break;
    }
    return true;
}
FeedbackFrame SekiroFeedbackEngine::Tick(std::int64_t now) {
    if (now < clockUs_) { Reset(); paused_ = true; }
    clockUs_ = std::max(clockUs_, now);
    if (lastInputUs_ && now - *lastInputUs_ > options_.sourceTimeoutUs) Reset();
    FeedbackFrame out;
    out.stopAudio = stopAudio_; stopAudio_ = false;
    for (const auto& cue : cues_)
        if (now-cue.timestampUs <= 150'000) out.cues.push_back(cue.cue);
    cues_.clear();
    if (paused_ || !lastInputUs_) return out;
    std::erase_if(pulses_, [now](const Pulse& p) { return now-p.startUs >= p.durationUs; });
    for (const auto& p : pulses_) {
        const auto age = now-p.startUs;
        const float envelope = age < 20'000 ? 1.0f :
            std::max(0.0f,1.0f-static_cast<float>(age-20'000)/static_cast<float>(p.durationUs-20'000));
        out.state.leftMotor = std::max(out.state.leftMotor,p.left*envelope);
        out.state.rightMotor = std::max(out.state.rightMotor,p.right*envelope);
    }
    const auto& tool = kProfiles[ToolIndex(selected_)];
    out.state.rightTrigger = {tool.start,tool.idle};
    if (useStartUs_ >= 0) {
        const auto age = now-useStartUs_;
        if (age < tool.duration) {
            const float release = 1.0f-static_cast<float>(age)/static_cast<float>(tool.duration);
            out.state.rightTrigger.force = tool.idle+(tool.recoil-tool.idle)*release;
        } else useStartUs_ = -1;
    }
    if (grappleStartUs_ >= 0) {
        const auto age = now-grappleStartUs_;
        const auto limit = grapplePhase_ == SekiroAction::GrapplePull ? 2'500'000 : 600'000;
        if (age >= limit) {
            grapplePhase_ = SekiroAction::GrappleEnd; grappleStartUs_ = -1;
        } else if (grapplePhase_ == SekiroAction::GrappleLaunch) {
            // Cable spools out: low resistance moves away from the finger.
            out.state.leftTrigger = {.25f+.50f*static_cast<float>(age)/600'000.0f,.09f};
        } else if (grapplePhase_ == SekiroAction::GrappleLatch) {
            out.state.leftTrigger = {.15f,.70f};
        } else if (grapplePhase_ == SekiroAction::GrapplePull) {
            // 8 Hz tension envelope, updated at 100 Hz. No undocumented trigger mode.
            const float wave = .5f+.5f*std::sin(static_cast<float>(age%125'000)*6.2831853f/125'000.0f);
            out.state.leftTrigger = {.12f,.35f+.40f*wave};
            out.state.leftMotor = std::max(out.state.leftMotor,.20f+.22f*wave);
            out.state.rightMotor = std::max(out.state.rightMotor,.12f+.12f*(1-wave));
        }
    }
    out.state.leftMotor *= options_.rumbleGain;
    out.state.rightMotor *= options_.rumbleGain;
    out.state.leftTrigger.force *= options_.triggerGain;
    out.state.rightTrigger.force *= options_.triggerGain;
    return out;
}

std::vector<float> SynthesizeCue(SpeakerCue cue, std::uint32_t rate, float gain) {
    if (rate < 8'000 || rate > 192'000) throw std::invalid_argument("unsupported sample rate");
    gain = Unit(gain);
    const float duration = cue == SpeakerCue::Deflect ? .24f : .18f;
    std::vector<float> pcm(static_cast<std::size_t>(duration*rate));
    std::uint32_t noiseState = 0x234517abu;
    for (std::size_t i=0;i<pcm.size();++i) {
        const float t = static_cast<float>(i)/static_cast<float>(rate);
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        const float noise = static_cast<float>(noiseState&65535)/32767.5f-1;
        const float attack = std::min(1.0f,t/.002f);
        const float tail = std::min(1.0f,(duration-t)/.010f);
        float value;
        if (cue == SpeakerCue::Deflect || cue == SpeakerCue::Metal || cue == SpeakerCue::RopeLatch) {
            const float base = cue == SpeakerCue::Deflect ? 1900.0f : 1100.0f;
            value = (.48f*std::sin(6.2831853f*base*t)+.24f*std::sin(6.2831853f*base*1.73f*t))
                *std::exp(-22*t)+.20f*noise*std::exp(-100*t);
        } else if (cue == SpeakerCue::RopeLaunch || cue == SpeakerCue::Fire) {
            value = .65f*noise*std::exp(-18*t)*( .7f+.3f*std::sin(6.2831853f*65*t));
        } else {
            value = .60f*std::sin(6.2831853f*240*t)*std::exp(-32*t)+.25f*noise*std::exp(-70*t);
        }
        pcm[i] = std::clamp(value*attack*tail*gain,-1.0f,1.0f);
    }
    return pcm;
}
} // namespace sekiro_haptics
