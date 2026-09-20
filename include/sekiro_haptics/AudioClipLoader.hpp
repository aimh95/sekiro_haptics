#pragma once

// Decodes an audio file to mono float PCM at a requested sample rate, using
// Windows Media Foundation -- already part of Windows, so no new third-party
// decoder is added to the project.
//
// Used for the deflect/block speaker cues when real recordings are supplied in
// audio/. The HAPTIC waveform is never taken from these files: a recorded
// metal clang lives in the kHz range that a voice coil cannot reproduce as an
// impact, so haptics stay procedurally synthesised (see GuardCue.hpp).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sekiro_haptics {

struct AudioClipInfo {
    bool ok = false;
    std::uint32_t sourceSampleRate = 0;
    std::uint32_t sourceChannels = 0;
    std::uint32_t decodedSampleRate = 0;
    std::size_t frames = 0;
    /// Absolute sample peak as decoded. MAY EXCEED 1.0 -- float decoding is
    /// not bounded and these sources do exceed it, so amplifying without
    /// measuring first would clip.
    float peak = 0.0f;
    bool clippedInSource = false;
    std::string error;
};

/// Decodes `path` to mono float samples in [-1, 1] at `targetSampleRate`.
/// Media Foundation performs the resample and downmix; if it refuses the
/// requested output type this reports the failure rather than silently
/// returning something at a different rate.
std::vector<float> LoadAudioClipMono(const std::filesystem::path& path,
                                     std::uint32_t targetSampleRate,
                                     AudioClipInfo& outInfo);

/// Peak-normalises to `targetPeak` (no-op when the clip is silent). Handles
/// input peaks above 1.0. Applied
/// so a supplied recording and a synthesised cue can be compared at a similar
/// level instead of one simply being louder.
void NormalizePeak(std::vector<float>& clip, float targetPeak);

/// Fades the last `milliseconds` to zero so a truncated clip cannot click.
void ApplyFadeOut(std::vector<float>& clip, std::uint32_t sampleRate, float milliseconds);

/// Trims leading samples below `threshold` so the impact starts immediately --
/// a recording with 30 ms of silence in front would otherwise add 30 ms of
/// latency to every hit. Returns how many frames were removed.
std::size_t TrimLeadingSilence(std::vector<float>& clip, float threshold);

} // namespace sekiro_haptics
