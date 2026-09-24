#include "sekiro_haptics/runtime/DualSenseAudioPcmSink.hpp"

#include <algorithm>
#include <cmath>

namespace sekiro_haptics {

DualSenseAudioPcmSink::DualSenseAudioPcmSink(DualSenseAudioDevice& device) : device_(device) {}

void DualSenseAudioPcmSink::RegisterCue(const std::string& cueId, const std::vector<float>& clip,
                                        PcmCueTarget target) {
    cues_[cueId] = Cue{&clip, nullptr, target};
}

void DualSenseAudioPcmSink::RegisterStereoCue(const std::string& cueId,
                                              const std::vector<float>& left,
                                              const std::vector<float>& right) {
    cues_[cueId] = Cue{&left, &right, PcmCueTarget::Haptic};
}

void DualSenseAudioPcmSink::SetSpeakerChannel(int channel) { speakerChannel_ = channel; }

void DualSenseAudioPcmSink::SetHapticChannels(int left, int right) {
    hapticLeft_ = left;
    hapticRight_ = right;
}

bool DualSenseAudioPcmSink::HasCue(const std::string& cueId) const {
    return cues_.find(cueId) != cues_.end();
}

bool DualSenseAudioPcmSink::QueueSpeaker(const std::string& cueId, float gain,
                                         std::uint64_t correlationId) {
    const auto found = cues_.find(cueId);
    if (found == cues_.end()) { lastError_ = "unknown cue \"" + cueId + "\""; return false; }
    if (found->second.target != PcmCueTarget::Speaker) {
        lastError_ = "cue \"" + cueId + "\" is a haptic waveform and is not sent to the speaker";
        return false;
    }
    if (speakerChannel_ < 0) { lastError_ = "no speaker channel has been identified"; return false; }
    return device_.Queue(*found->second.clip, speakerChannel_, gain, correlationId);
}

bool DualSenseAudioPcmSink::QueueHaptic(const std::string& cueId, float gain, float balance,
                                        std::uint64_t correlationId) {
    const auto found = cues_.find(cueId);
    if (found == cues_.end()) { lastError_ = "unknown cue \"" + cueId + "\""; return false; }
    if (found->second.target != PcmCueTarget::Haptic) {
        lastError_ = "cue \"" + cueId + "\" is a speaker recording and is not sent to the actuators";
        return false;
    }
    if (hapticLeft_ < 0 && hapticRight_ < 0) {
        lastError_ = "no haptic channel has been identified";
        return false;
    }
    // One layer becomes two voices (left and right). That is why an event's
    // voice count is compared against the preset's layer count and never
    // assumed to be one.
    if (found->second.rightClip == nullptr)
        return device_.QueuePair(*found->second.clip, hapticLeft_, hapticRight_, gain, balance,
                                 correlationId);

    // Two waveforms, same two voices. The balance split is the one QueuePair
    // applies, kept identical here so a stereo cue and a mono cue answer a
    // preset's balance the same way.
    balance = std::isfinite(balance) ? std::clamp(balance, -1.0f, 1.0f) : 0.0f;
    const float leftGain = gain * (balance > 0.0f ? 1.0f - balance : 1.0f);
    const float rightGain = gain * (balance < 0.0f ? 1.0f + balance : 1.0f);
    bool queued = false;
    if (hapticLeft_ >= 0)
        queued = device_.Queue(*found->second.clip, hapticLeft_, leftGain, correlationId) || queued;
    if (hapticRight_ >= 0)
        queued = device_.Queue(*found->second.rightClip, hapticRight_, rightGain, correlationId) ||
                 queued;
    return queued;
}

} // namespace sekiro_haptics
