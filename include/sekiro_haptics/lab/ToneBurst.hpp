#pragma once

// One sine burst, for comparing PCM haptics by hand without the game.
//
// WHAT THIS IS NOT
// ----------------
// This renders MATHEMATICS. A 40 Hz burst and a 300 Hz burst at the same
// amplitude are the same number on the wire, and nothing here says the two
// actuators move by the same amount at both -- a voice coil has its own
// response, and this project has not measured it. The numbers this file
// reports are sample values, not vibration. Anything about how strong
// something FEELS has to come from a hand on the device.
//
// So there is no equal-loudness curve, no per-frequency trim and no
// normalisation here, deliberately: a tool for finding out what the device
// does must not quietly correct for what it does.

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::lab {

/// The ranges are what the UI offers and what this validates against. They
/// are the bounds of the experiment, not claims about the hardware.
inline constexpr float kMinFrequencyHz = 20.0f;
inline constexpr float kMaxFrequencyHz = 500.0f;
inline constexpr float kMinLengthMs = 20.0f;
inline constexpr float kMaxLengthMs = 1000.0f;

struct ToneBurstSpec {
    float frequencyHz = 60.0f;
    /// Peak of the sine BEFORE the fades, in sample units. 1.0 is full scale.
    float amplitude = 0.5f;
    float lengthMs = 200.0f;
    /// Raised-cosine fades. They are part of the waveform being compared, so
    /// they are reported in the measurements rather than hidden.
    float fadeInMs = 5.0f;
    float fadeOutMs = 10.0f;
    /// Per-side scale, applied after the fades. Two independent numbers, not
    /// a balance control: the point is to drive one actuator without the
    /// other when that is what you want to feel.
    float leftGain = 1.0f;
    float rightGain = 1.0f;
};

/// Empty when the spec is usable, otherwise why not.
///
/// Rejects rather than clamps. A silently corrected experiment reports a
/// number that was never played.
std::string ValidateToneBurst(const ToneBurstSpec& spec);

/// De-interleaved, one vector per side, both the same length.
///
/// The wave is a plain sine through zero: it swings NEGATIVE as well as
/// positive, which is the whole point of driving a voice coil from PCM rather
/// than from a rumble motor's single-sided magnitude.
///
/// Leaves both vectors empty if the spec does not validate.
void RenderToneBurst(const ToneBurstSpec& spec, std::uint32_t sampleRate,
                     std::vector<float>& outLeft, std::vector<float>& outRight);

struct ToneBurstStats {
    std::size_t frames = 0;
    float peakLeft = 0.0f, peakRight = 0.0f;
    float rmsLeft = 0.0f, rmsRight = 0.0f;
};

/// Measures whatever it is given -- the generated burst, or a capture of what
/// the device was actually handed. The same function on both ends is the
/// point: it is how "it got quieter somewhere in the middle" becomes a
/// comparison instead of an impression.
ToneBurstStats MeasureToneBurst(const std::vector<float>& left, const std::vector<float>& right);

/// 48 kHz interleaved stereo, for the WAV export.
std::vector<float> InterleaveStereo(const std::vector<float>& left,
                                    const std::vector<float>& right);

} // namespace sekiro_haptics::lab
