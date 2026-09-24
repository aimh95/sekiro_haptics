#pragma once

// A preset is several waveform LAYERS summed on one sample clock.
//
// WHY LAYERS ARE RENDERED TOGETHER, NOT QUEUED SEPARATELY
// -------------------------------------------------------
// Every layer's start time and starting phase are counted in SAMPLES from the
// preset's first sample. That is the only way "start the second waveform
// 30 ms into the first" means the same thing twice: a GUI timer, a sleep or a
// second queued voice all land wherever the scheduler happens to wake up,
// which on Windows is tens of milliseconds of jitter -- larger than most of
// the intervals worth experimenting with here.
//
// So the whole preset is rendered to one stereo buffer and queued as one pair
// of voices. Layer 2 starting during layer 1 is then arithmetic, exact and
// reproducible, and the WAV export is the same buffer that was played rather
// than a re-derivation of it.
//
// WHAT THIS DOES NOT DO
// ---------------------
// No per-layer normalisation, ever. Comparing a low frequency alone against
// the same low frequency plus a high one only means something if the low one
// did not change, and normalising either side would change it. The sum is
// left at whatever it comes to, measured before the limiter and after, and
// both numbers are reported.
//
// No automatic envelope, decay or smoothing on a formula layer either. A
// formula is evaluated as written; envelopes and fades are separate options
// the caller turns on, and the fact that they are on is part of what gets
// reported.

#include "sekiro_haptics/lab/Expression.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::lab {

enum class LayerWaveform {
    Sine,        ///< one frequency
    Impulse,     ///< a short, finite strike: one damped half-cycle burst
    Chirp,       ///< frequency sweeps from `frequencyHz` to `frequencyEndHz`
    AmSine,      ///< a carrier whose amplitude is shaped by a slower wave
    Formula,     ///< the expression, in radians (see Expression.hpp)
};

/// Attack / hold / decay / release, in milliseconds.
///
/// `Total()` is not forced to equal the layer's duration: when the segments
/// are shorter the layer simply holds at the sustain level, and when they are
/// longer they are scaled down to fit, which is reported rather than silent.
struct LayerEnvelope {
    bool enabled = false;
    float attackMs = 5.0f;
    float holdMs = 0.0f;
    float decayMs = 30.0f;
    /// Level held after the decay, 0..1. The release starts from here.
    float sustain = 0.6f;
    float releaseMs = 40.0f;

    float Total() const { return attackMs + holdMs + decayMs + releaseMs; }
};

struct WaveformLayer {
    std::string name = "레이어";
    bool muted = false;
    bool solo = false;

    /// Both in milliseconds from the start of the PRESET.
    float startMs = 0.0f;
    float durationMs = 200.0f;

    LayerWaveform waveform = LayerWaveform::Sine;
    float frequencyHz = 60.0f;
    /// Chirp end frequency. Ignored by every other waveform.
    float frequencyEndHz = 200.0f;
    /// AM modulation rate and depth (0..1). Ignored by every other waveform.
    float modulationHz = 8.0f;
    float modulationDepth = 1.0f;
    /// Impulse decay, in "how many times quieter per second".
    float decayPerSecond = 40.0f;

    /// Linear amplitude, 0..1. `gainDb` is an additional, independent trim so
    /// a level can be nudged in the unit an audio person expects; the two
    /// multiply and both are shown, so neither is a hidden second gain.
    float amplitude = 0.5f;
    float gainDb = 0.0f;

    /// Where in the cycle the layer begins, in DEGREES. 0 starts at zero
    /// going positive; 90 starts at the peak.
    float startPhaseDeg = 0.0f;

    LayerEnvelope envelope;

    /// Per-side scale, 0..1 each. Two independent numbers, not a balance:
    /// driving one actuator alone is a normal thing to want here.
    float leftGain = 1.0f;
    float rightGain = 1.0f;

    /// Formula layers only.
    std::string formula = "sin(2*pi*60*t)";
    /// The `t0` a piecewise formula can reference, in SECONDS. Validated
    /// positive whenever the formula mentions it, because the formulas this
    /// exists for divide by something derived from it.
    float t0Seconds = 0.05f;
    /// `t` counts from the PRESET start by default. When this is set it
    /// counts from this layer's own start instead, which is usually what a
    /// formula written in isolation assumes.
    bool formulaTimeFromLayerStart = false;
};

struct LayeredPresetSpec {
    std::string name = "새 프리셋";
    /// One multiplier over the whole sum, applied after mixing and before the
    /// device. Kept separate from layer gains so an A/B can hold it fixed.
    float masterGain = 1.0f;

    /// Fold the summed waveform down onto this level, or 0 to leave it alone.
    ///
    /// WHY THIS EXISTS
    /// ---------------
    /// The device's limiter holds a channel at 0.95 with a 120 ms release, so
    /// once a loud preset trips it, it stays tripped for the rest of the cue
    /// and hands the level straight back. Measured on hand-tuned presets:
    /// +24 dB of layer gain produced between +0.9 dB and +13.9 dB of actual
    /// output, and for one of them 24 dB in was 0.9 dB out.
    ///
    /// Saturating HERE, just below that threshold, puts the energy into the
    /// waveform permanently -- the limiter then never engages for a single
    /// cue and goes back to its real job of catching sums during overlap.
    /// This is the same technique the game path's blade cues use.
    ///
    /// It is off by default: it changes the SHAPE of a waveform, and a bench
    /// whose job is to show what a waveform does must not reshape one unless
    /// asked. When it is on, the analysis reports both sides of it.
    float saturationCeiling = 0.0f;
    std::vector<WaveformLayer> layers;

    /// Milliseconds the preset lasts: the furthest layer end, at minimum.
    float LengthMs() const;
};

/// What a render actually produced, in SAMPLE VALUES.
///
/// Not force, not travel, not energy delivered to a hand. This process cannot
/// observe any of those and does not guess at them.
struct SignalStats {
    std::size_t frames = 0;
    float peak = 0.0f;
    float rms = 0.0f;
    /// The average. A non-zero one means the waveform sits off centre, which
    /// pushes the actuator to one side and turns into heat rather than
    /// movement -- worth seeing, and invisible in a peak or an RMS.
    float mean = 0.0f;
    /// Samples outside +-1.0 BEFORE any limiting. This is the honest count of
    /// how far the sum went past what the format can carry.
    std::uint64_t overRange = 0;
};

struct RenderDiagnostics {
    ExpressionDiagnostics expression;
    /// Layers whose envelope segments had to be scaled to fit the duration.
    std::vector<std::string> envelopeScaled;
    /// Non-finite samples in the SUM, replaced by 0 and counted.
    std::uint64_t nonFiniteSamples = 0;
};

struct RenderedPreset {
    bool ok = false;
    std::string error;

    std::uint32_t sampleRate = 48'000;
    /// The sum, before the device limiter. What the formulas actually came to.
    std::vector<float> left, right;
    /// Each layer on its own, same length and alignment as the sum, so a
    /// layer can be seen in the place it occupies rather than in isolation.
    std::vector<std::vector<float>> layerLeft, layerRight;

    SignalStats statsLeft, statsRight;
    RenderDiagnostics diagnostics;
};

/// Empty when the spec can be rendered, otherwise the first reason it cannot.
/// Refuses rather than clamps: a silently corrected experiment reports a
/// number that was never played.
std::string ValidateLayeredPreset(const LayeredPresetSpec& spec);

/// Render the whole preset on one sample clock.
///
/// `soloAware` honours any layer marked solo (and then mutes the rest), which
/// is how a single layer is auditioned without changing the preset.
RenderedPreset RenderLayeredPreset(const LayeredPresetSpec& spec, std::uint32_t sampleRate);

SignalStats MeasureSignal(const std::vector<float>& samples);

/// The device's limiter and final clamp, applied offline to a rendered
/// buffer, so "what will survive" can be shown next to "what I made" without
/// having to play it first.
///
/// These are the same numbers `DualSenseAudioDevice` uses; this is a
/// PREDICTION of that path, clearly labelled as such, not a capture of it.
struct LimiterPreview {
    std::vector<float> samples;
    float worstReductionDb = 0.0f;
    std::uint64_t limitedFrames = 0;
    std::uint64_t clampedSamples = 0;
};
LimiterPreview PreviewLimiter(const std::vector<float>& samples, std::uint32_t sampleRate,
                              float threshold, float attackMs, float releaseMs);

enum class FftWindow { Rectangular, Hann, Hamming };

struct FftResult {
    bool ok = false;
    std::string error;
    std::uint32_t sampleRate = 0;
    std::size_t size = 0;            ///< transform length, a power of two
    std::size_t startFrame = 0;
    FftWindow window = FftWindow::Hann;
    /// Bin centre frequencies, and magnitude in PCM sample units (not dB, and
    /// not any physical unit). Scaled by 2/N and corrected for the window's
    /// coherent gain, so a full-scale sine reads about 1.0 at its bin.
    std::vector<float> frequencyHz;
    std::vector<float> magnitude;
};

/// One window of `samples`, starting at `startFrame`. Refuses rather than
/// zero-padding past the end: a spectrum of half a window of silence is not
/// the spectrum of the signal, and padding it would look like one.
FftResult AnalyzeSpectrum(const std::vector<float>& samples, std::uint32_t sampleRate,
                          std::size_t startFrame, std::size_t size, FftWindow window);

const char* ToString(LayerWaveform waveform);
const char* ToString(FftWindow window);
bool ParseWaveform(const std::string& text, LayerWaveform& out);
bool ParseFftWindow(const std::string& text, FftWindow& out);

} // namespace sekiro_haptics::lab
