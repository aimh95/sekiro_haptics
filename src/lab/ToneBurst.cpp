#include "sekiro_haptics/lab/ToneBurst.hpp"

#include <cmath>
#include <sstream>

namespace sekiro_haptics::lab {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

bool Finite(float v) { return std::isfinite(v); }

} // namespace

std::string ValidateToneBurst(const ToneBurstSpec& spec) {
    std::ostringstream why;
    if (!Finite(spec.frequencyHz) || spec.frequencyHz < kMinFrequencyHz ||
        spec.frequencyHz > kMaxFrequencyHz) {
        why << "frequency must be " << kMinFrequencyHz << ".." << kMaxFrequencyHz << " Hz";
        return why.str();
    }
    if (!Finite(spec.amplitude) || spec.amplitude < 0.0f || spec.amplitude > 1.0f)
        return "amplitude must be 0..1";
    if (!Finite(spec.lengthMs) || spec.lengthMs < kMinLengthMs || spec.lengthMs > kMaxLengthMs) {
        why << "length must be " << kMinLengthMs << ".." << kMaxLengthMs << " ms";
        return why.str();
    }
    if (!Finite(spec.fadeInMs) || spec.fadeInMs < 0.0f) return "fade-in must be >= 0 ms";
    if (!Finite(spec.fadeOutMs) || spec.fadeOutMs < 0.0f) return "fade-out must be >= 0 ms";
    // Overlapping fades would multiply each other and the burst would never
    // reach the amplitude that was asked for -- which is exactly the kind of
    // silent discrepancy this tool exists to rule out.
    if (spec.fadeInMs + spec.fadeOutMs > spec.lengthMs)
        return "fade-in + fade-out must fit inside the length";
    if (!Finite(spec.leftGain) || spec.leftGain < 0.0f || spec.leftGain > 1.0f)
        return "left gain must be 0..1";
    if (!Finite(spec.rightGain) || spec.rightGain < 0.0f || spec.rightGain > 1.0f)
        return "right gain must be 0..1";
    return {};
}

void RenderToneBurst(const ToneBurstSpec& spec, std::uint32_t sampleRate,
                     std::vector<float>& outLeft, std::vector<float>& outRight) {
    outLeft.clear();
    outRight.clear();
    if (sampleRate < 8'000 || sampleRate > 192'000) return;
    if (!ValidateToneBurst(spec).empty()) return;

    const auto rate = static_cast<double>(sampleRate);
    const auto frames = static_cast<std::size_t>(
        static_cast<double>(spec.lengthMs) / 1000.0 * rate);
    if (frames == 0) return;
    const auto fadeIn = static_cast<std::size_t>(static_cast<double>(spec.fadeInMs) / 1000.0 * rate);
    const auto fadeOut = static_cast<std::size_t>(static_cast<double>(spec.fadeOutMs) / 1000.0 * rate);

    outLeft.resize(frames);
    outRight.resize(frames);
    const double step = kTwoPi * static_cast<double>(spec.frequencyHz) / rate;
    for (std::size_t i = 0; i < frames; ++i) {
        // Phase from the sample index, not accumulated: an accumulator drifts
        // over a long burst, and two exports of the same spec must be the
        // same bytes.
        double v = std::sin(step * static_cast<double>(i)) * static_cast<double>(spec.amplitude);

        // Raised cosine, so the envelope leaves and returns to zero with zero
        // slope. A linear ramp has a corner at each end, and a corner is a
        // step in acceleration -- audible and feelable as a tick, which would
        // be read as part of whatever is being compared.
        if (fadeIn > 0 && i < fadeIn)
            v *= 0.5 - 0.5 * std::cos(3.14159265358979 * static_cast<double>(i) /
                                      static_cast<double>(fadeIn));
        if (fadeOut > 0 && i + fadeOut >= frames) {
            const auto into = frames - i - 1;
            v *= 0.5 - 0.5 * std::cos(3.14159265358979 * static_cast<double>(into) /
                                      static_cast<double>(fadeOut));
        }
        outLeft[i] = static_cast<float>(v * static_cast<double>(spec.leftGain));
        outRight[i] = static_cast<float>(v * static_cast<double>(spec.rightGain));
    }
}

ToneBurstStats MeasureToneBurst(const std::vector<float>& left, const std::vector<float>& right) {
    ToneBurstStats stats;
    stats.frames = left.size() > right.size() ? left.size() : right.size();
    auto measure = [](const std::vector<float>& v, float& peak, float& rms) {
        double sumSquares = 0.0;
        for (float s : v) {
            const float magnitude = std::fabs(s);
            if (magnitude > peak) peak = magnitude;
            sumSquares += static_cast<double>(s) * static_cast<double>(s);
        }
        rms = v.empty() ? 0.0f
                        : static_cast<float>(std::sqrt(sumSquares / static_cast<double>(v.size())));
    };
    measure(left, stats.peakLeft, stats.rmsLeft);
    measure(right, stats.peakRight, stats.rmsRight);
    return stats;
}

std::vector<float> InterleaveStereo(const std::vector<float>& left,
                                    const std::vector<float>& right) {
    const std::size_t frames = left.size() < right.size() ? left.size() : right.size();
    std::vector<float> out;
    out.reserve(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        out.push_back(left[i]);
        out.push_back(right[i]);
    }
    return out;
}

} // namespace sekiro_haptics::lab
