#pragma once

// IPcmOutputSink over the real mixer.
//
// It holds the clip LIBRARY, not the clips' storage policy: every clip is
// rendered once before the output loop starts and registered here, and the
// sink then hands the mixer a reference to it. Nothing on this path
// synthesises, decodes, reads a file or allocates a buffer -- that rule is
// why the render thread can keep its feed deadline (docs/11-guard-feedback.md
// section 7).
//
// The channel indices are whatever the caller established by listening and
// feeling (GuardCue.hpp, GuardDeviceSettings). They are not defaulted here,
// and a cue aimed at an unset channel is refused rather than retargeted.

#include "sekiro_haptics/DualSenseAudioDevice.hpp"
#include "sekiro_haptics/runtime/OutputRuntime.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace sekiro_haptics {

/// Where a registered clip is allowed to go. A clip registered for the
/// speaker cannot be queued onto the haptic channels by a mistyped preset,
/// and the reverse -- the two waveforms are designed for different
/// transducers and swapping them is never a harmless accident.
enum class PcmCueTarget { Speaker, Haptic };

class DualSenseAudioPcmSink final : public IPcmOutputSink {
public:
    explicit DualSenseAudioPcmSink(DualSenseAudioDevice& device);

    /// `clip` is referenced, not copied: it must outlive this sink.
    void RegisterCue(const std::string& cueId, const std::vector<float>& clip, PcmCueTarget target);

    /// A cue whose two sides were authored apart, for actuator assets that say
    /// something different to each hand.
    ///
    /// The sides are NOT summed into one waveform: that would average away the
    /// difference the asset exists to carry. They are queued as two voices,
    /// left to the left channel and right to the right, exactly as a single
    /// mono cue already becomes two. Both vectors are referenced and must
    /// outlive this sink, and a stereo cue can only target the actuators --
    /// the speaker is one channel and has no second side to put anywhere.
    void RegisterStereoCue(const std::string& cueId, const std::vector<float>& left,
                           const std::vector<float>& right);

    /// -1 means "not identified", and any cue aimed at it is refused.
    void SetSpeakerChannel(int channel);
    void SetHapticChannels(int left, int right);

    bool HasCue(const std::string& cueId) const override;
    bool QueueSpeaker(const std::string& cueId, float gain, std::uint64_t correlationId) override;
    bool QueueHaptic(const std::string& cueId, float gain, float balance,
                     std::uint64_t correlationId) override;

    /// Why the last refusal happened, for the caller's startup report.
    const std::string& LastError() const { return lastError_; }

private:
    struct Cue {
        const std::vector<float>* clip = nullptr;
        /// Null for a mono cue, which is sent to both sides as it always was.
        const std::vector<float>* rightClip = nullptr;
        PcmCueTarget target = PcmCueTarget::Speaker;
    };

    DualSenseAudioDevice& device_;
    std::unordered_map<std::string, Cue> cues_;
    int speakerChannel_ = -1;
    int hapticLeft_ = -1;
    int hapticRight_ = -1;
    std::string lastError_;
};

} // namespace sekiro_haptics
