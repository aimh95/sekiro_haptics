#pragma once

// Two collision feedbacks -- a deflect (Just Guard) and a normal block --
// synthesised procedurally, with the SPEAKER waveform and the HAPTIC waveform
// designed separately rather than one being a copy of the other.
//
// Why separate: the built-in speaker reproduces a bright inharmonic metal
// clang in the kHz range, while the voice-coil actuators are felt, not heard,
// and respond in a low band. Sending the kHz clang to the actuators would
// produce a thin buzz, not an impact. So each profile carries its own set of
// partials, envelopes and durations.
//
// Everything in GuardCueProfile is data loaded from config/guard_cues.json.
// The values shipped there are a STARTING POINT for side-by-side comparison
// on real hardware, not measured optima and not device specifications.

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics {

/// One inharmonic partial of the metallic body. Real struck metal is
/// inharmonic, so the ratios are deliberately not integers.
struct CuePartial {
    float ratio = 1.0f;      // multiple of baseHz
    float amplitude = 0.0f;
    float decayPerSecond = 20.0f;  // higher = dies sooner
};

/// The audible clang sent to the controller's built-in speaker.
struct SpeakerCueProfile {
    /// When set, this recording is used instead of the synthesised partials
    /// below. The haptic waveform is never taken from here.
    std::string clipPath;
    /// Play the recording AS RECORDED.
    ///
    /// Nothing is cut, normalised, faded, filtered or extended -- only the
    /// leading silence is removed (so the clang is not late) and `trimDb` is
    /// applied as a plain level scale. The level scale is not optional: these
    /// sources decode ABOVE 0 dBFS (deflect peaks at 1.1397, parry at 1.0257),
    /// so submitting them literally unscaled would hard-clip.
    ///
    /// Consequence, stated plainly: the supplied recordings contain five and
    /// six sword impacts. With this on, one guard event plays all of them.
    /// That is what "do not modify the audio" means for these files, and it
    /// is the caller's choice, not an accident -- so the impact count is
    /// reported rather than warned about.
    bool clipRaw = false;
    /// Peak the loaded clip is normalised to, so a recording and a synthesised
    /// cue can be compared at a similar level.
    float clipPeak = 0.92f;
    float durationMs = 140.0f;
    float attackMs = 1.0f;
    float releaseMs = 12.0f;     // fade-out so the clip never clicks at the end
    float baseHz = 2400.0f;
    std::vector<CuePartial> partials;
    float strikeNoiseAmplitude = 0.25f;   // the initial "hit", not the body
    float strikeNoiseDecayPerSecond = 160.0f;
    float strikeNoiseHighpass = 0.5f;     // 0 = raw noise, 1 = very bright
    float gain = 0.45f;
    /// Level trim in dB applied to the SPEAKER clip only, after normalisation.
    /// This is the knob for "is the breakup digital clipping or the driver
    /// being overdriven" -- it moves the digital level without touching the
    /// controller's own volume byte or Windows' endpoint level.
    float trimDb = 0.0f;
    /// Optional shaping, again SPEAKER ONLY and off by default (0 = off).
    /// These are candidate fixes to compare, not statements about what the
    /// controller's driver can reproduce.
    float highpassHz = 0.0f;
    float lowpassHz = 0.0f;
    /// Milliseconds of SYNTHESISED ring appended after the recording, 0 = off.
    ///
    /// Why this is needed: both supplied recordings have another sword strike
    /// about 66-70 ms after the one being used, so a single-impact slice can
    /// only be ~66 ms long -- and 66 ms of a metal clang sounds cut off,
    /// because the real tail was thrown away with the second strike. The tail
    /// is rebuilt from the clip's OWN late material (see ExtendDecayTail), so
    /// it keeps the timbre without importing a second attack.
    float tailMs = 0.0f;
    /// How fast the appended tail dies. Higher = shorter ring.
    float tailDecayPerSecond = 11.0f;
    /// Length of each grain taken from the clip's tail region. Grains are
    /// crossfaded, which is what keeps the join from reading as a new hit.
    float tailGrainMs = 18.0f;
};

/// Appends a decaying ring built from `clip`'s own tail region.
///
/// Grains are taken from the last third of the clip -- steady-state ring
/// material, not the attack -- and crossfaded under a monotonically decaying
/// envelope. Because no grain contains the attack and the envelope only ever
/// falls, the result still reads as ONE impact; that is checked by the onset
/// counter the app already runs on every cue.
void ExtendDecayTail(std::vector<float>& clip, std::uint32_t sampleRate,
                     float tailMs, float decayPerSecond, float grainMs);

/// What ApplySpeakerShaping actually did, so a comparison is not confounded by
/// the filters having also changed the loudness.
struct SpeakerShapingReport {
    float peakBefore = 0.0f;
    float rmsBefore = 0.0f;
    float peakAfter = 0.0f;
    float rmsAfter = 0.0f;
    bool highpassApplied = false;
    bool lowpassApplied = false;
    float trimDb = 0.0f;
};

/// Trim and (optionally) filter a speaker clip in place. NEVER call this on a
/// haptic clip: the bands it touches are the ones the voice coils live in.
SpeakerShapingReport ApplySpeakerShaping(std::vector<float>& clip, std::uint32_t sampleRate,
                                         const SpeakerCueProfile& profile);

/// One overlapping component of the felt impact. Three of these are summed:
/// a sharp contact, a body, and a thin ring. Windows overlap on purpose -- the
/// ring starts while the body is still sounding, which is what reads as metal
/// rather than as three separate taps.
struct HapticLayer {
    float startMs = 0.0f;
    float endMs = 30.0f;
    /// A layer may glide (contact does, 260 -> 220 Hz); set both the same for a
    /// fixed tone.
    float startHz = 200.0f;
    float endHz = 200.0f;
    /// How fast this layer comes up. Short = sharp contact; longer = rounder.
    float riseMs = 1.0f;
    /// How long the layer STAYS at full after the rise, before the exponential
    /// decay starts. Without this the body begins dying at t=0, so only the
    /// first instant is felt and the impact reads as thin -- which is what the
    /// original three-layer preset did. 0 reproduces that old behaviour
    /// exactly, so variant A stays bit-identical.
    float holdMs = 0.0f;
    /// Relative weight BEFORE the whole cue is peak-normalised.
    float weight = 1.0f;
    /// Shaping inside the layer's own window.
    float decayPerSecond = 40.0f;
};

/// The felt impact sent to the voice-coil actuators (PCM haptics).
///
/// Three layers, summed then peak-normalised. The normalised peak is a DIGITAL
/// amplitude: 0.30 does not mean "30% of the force" and two cues at the same
/// peak do not necessarily feel equally strong -- duration, the actuator's
/// response at that frequency and how the pad is held all change it. Levels
/// have to be matched by hand on the device.
struct HapticCueProfile {
    float totalMs = 72.0f;
    /// Peak the summed layers are normalised to.
    float normalizePeak = 0.30f;
    HapticLayer contact;   // the first sharp touch
    HapticLayer body;      // the mass behind it
    HapticLayer ring;      // the thin metallic tail
    /// Grain on the very first milliseconds; texture, not rumble.
    float noiseAmplitude = 0.10f;
    /// Master, applied after normalisation.
    float gain = 1.0f;
};

/// One complete feedback: what it sounds like and what it feels like.
struct GuardCueProfile {
    std::string name;
    SpeakerCueProfile speaker;
    HapticCueProfile haptic;
    float balance = 0.0f;   // -1 left .. +1 right, applied to both outputs
};

/// Which endpoint and channels this machine's controller actually uses.
/// Every one of these was established by listening/feeling, not by reading a
/// channel mask: the endpoint reports FL/FR/BL/BR, but on this controller
/// channel 1 is the built-in speaker and 2/3 are the left/right voice coils.
/// -1 means "not identified"; the app refuses to output to an unset channel.
struct GuardDeviceSettings {
    int audioEndpointIndex = -1;
    int speakerChannel = -1;
    int hapticLeftChannel = -1;
    int hapticRightChannel = -1;
    /// Built-in speaker level byte. NOT 0..255 -- see DualSenseUsbReport.hpp.
    int controllerSpeakerVolume = 100;   // 0x64
    int audioOutputPath = 3;
    int speakerPreGain = 7;
    /// Windows' level for this endpoint, applied only when >= 0. This affects
    /// other apps using the same device, so it stays opt-in.
    float windowsEndpointVolume = -1.0f;
};

/// Both profiles plus the shared output settings.
struct GuardCueConfig {
    GuardDeviceSettings device;
    GuardCueProfile deflect;
    GuardCueProfile block;
    float speakerVolume = 1.0f;   // master, on top of each profile's gain
    float hapticStrength = 1.0f;
    /// NO LONGER APPLIED WHEN A NEW EVENT ARRIVES. Overlapping tails is the
    /// intended behaviour: a second clang rings over the first. Kept only so
    /// an existing config file still parses, and so the change is visible
    /// rather than silent. It is used for explicit stops (device loss, player
    /// object replaced, shutdown).
    float retriggerDuck = 1.0f;
    /// How long a tail takes to fade when the voice cap forces one out. This
    /// is the ONLY place a sounding cue is shortened on purpose.
    float retireFadeMs = 25.0f;
    /// Summed-overlap protection. The limiter only moves when the sum actually
    /// exceeds the threshold -- it is never "divide by the number of voices",
    /// which would duck every hit the instant a second one started.
    float limiterThreshold = 0.95f;
    float speakerHeadroom = 0.85f;
    float hapticHeadroom = 1.0f;
    /// An event older than this when it reaches the output stage is dropped
    /// rather than played late.
    std::int64_t maxOutputLatencyUs = 120'000;
    /// Soft cap. One event costs 3 voices (1 speaker + 2 haptic). The clips
    /// are now the recordings' own length (378 ms deflect, 500 ms block), so
    /// far more of them overlap than when they were 71 ms: a 30 ms stress run
    /// of 10 hits reached 27 concurrent voices. 36 keeps even that from
    /// retiring anything, which is the point -- normal consecutive parrying
    /// must never hit the cap. Nothing is dropped outright until 8 above it.
    std::size_t voiceLimit = 36;
};

/// Built-in defaults, used when no config file is present. Same values as the
/// shipped config/guard_cues.json.
GuardCueConfig DefaultGuardCueConfig();

/// Parses a guard-cue config. On failure returns false and fills `error`;
/// `out` is left untouched so a bad edit cannot silently change the feel.
bool ParseGuardCueConfig(const std::string& json, GuardCueConfig& out, std::string& error);

/// Serialises back out, so a tuned config can be written and diffed.
std::string SerializeGuardCueConfig(const GuardCueConfig& config);

/// Mono PCM in [-1, 1] for the controller's speaker.
std::vector<float> SynthesizeGuardSpeaker(const SpeakerCueProfile& profile, std::uint32_t sampleRate,
                                          float masterVolume);

/// Mono PCM in [-1, 1] for the voice-coil actuators. Deliberately a different
/// signal from the speaker clip, not a filtered copy of it.
std::vector<float> SynthesizeGuardHaptic(const HapticCueProfile& profile, std::uint32_t sampleRate,
                                         float masterStrength);

/// Peak absolute sample, for reporting that a clip is not clipping.
float PeakAmplitude(const std::vector<float>& clip);

} // namespace sekiro_haptics
