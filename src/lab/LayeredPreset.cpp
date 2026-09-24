#include "sekiro_haptics/lab/LayeredPreset.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <sstream>

namespace sekiro_haptics::lab {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr std::size_t kMaxLayers = 32;
constexpr float kMaxPresetMs = 10'000.0f;

bool Finite(float v) { return std::isfinite(v); }

/// The envelope at `t` seconds into a layer of `durationSeconds`.
///
/// Segments are scaled to fit when they are longer than the layer, so the
/// shape is preserved rather than truncated mid-attack -- a cut-off attack is
/// a step, and a step is a click that would be read as part of the waveform.
double EnvelopeAt(const LayerEnvelope& envelope, double t, double durationSeconds, bool& scaled) {
    if (!envelope.enabled) return 1.0;
    double attack = std::max(0.0f, envelope.attackMs) / 1000.0;
    double hold = std::max(0.0f, envelope.holdMs) / 1000.0;
    double decay = std::max(0.0f, envelope.decayMs) / 1000.0;
    double release = std::max(0.0f, envelope.releaseMs) / 1000.0;
    const double total = attack + hold + decay + release;
    if (total > durationSeconds && total > 0.0) {
        const double squeeze = durationSeconds / total;
        attack *= squeeze;
        hold *= squeeze;
        decay *= squeeze;
        release *= squeeze;
        scaled = true;
    }
    const double sustain = std::clamp(static_cast<double>(envelope.sustain), 0.0, 1.0);
    const double releaseStart = std::max(0.0, durationSeconds - release);

    if (t >= releaseStart && release > 0.0) {
        const double into = (t - releaseStart) / release;
        return sustain * std::max(0.0, 1.0 - into);
    }
    if (t < attack) return attack > 0.0 ? t / attack : 1.0;
    if (t < attack + hold) return 1.0;
    if (t < attack + hold + decay) {
        const double into = decay > 0.0 ? (t - attack - hold) / decay : 1.0;
        return 1.0 + (sustain - 1.0) * into;
    }
    return sustain;
}

/// One layer's contribution at `t` seconds from ITS OWN start.
double SampleLayer(const WaveformLayer& layer, double t, double presetT,
                   Expression& formula, ExpressionDiagnostics& diagnostics) {
    const double phase = static_cast<double>(layer.startPhaseDeg) * kPi / 180.0;
    switch (layer.waveform) {
    case LayerWaveform::Sine:
        return std::sin(kTwoPi * layer.frequencyHz * t + phase);
    case LayerWaveform::Impulse: {
        // A finite strike: one decaying oscillation. The decay is in the
        // waveform itself rather than in an envelope, because "how fast the
        // strike dies" is the thing being compared here.
        const double decay = std::exp(-std::max(0.0f, layer.decayPerSecond) * t);
        return std::sin(kTwoPi * layer.frequencyHz * t + phase) * decay;
    }
    case LayerWaveform::Chirp: {
        // Linear sweep. The phase is the INTEGRAL of the instantaneous
        // frequency, not frequency times time -- the naive version sweeps at
        // half the rate it claims to and ends at the wrong frequency.
        const double duration = std::max(1e-6, layer.durationMs / 1000.0);
        const double rate = (layer.frequencyEndHz - layer.frequencyHz) / duration;
        const double angle = kTwoPi * (layer.frequencyHz * t + 0.5 * rate * t * t) + phase;
        return std::sin(angle);
    }
    case LayerWaveform::AmSine: {
        const double depth = std::clamp(static_cast<double>(layer.modulationDepth), 0.0, 1.0);
        const double modulator = 1.0 - depth * 0.5 * (1.0 - std::cos(kTwoPi * layer.modulationHz * t));
        return std::sin(kTwoPi * layer.frequencyHz * t + phase) * modulator;
    }
    case LayerWaveform::Formula: {
        const double time = layer.formulaTimeFromLayerStart ? t : presetT;
        const std::vector<ExpressionBinding> bindings = {
            {"t", time},
            {"t0", static_cast<double>(layer.t0Seconds)},
            {"f", static_cast<double>(layer.frequencyHz)},
        };
        return formula.Evaluate(bindings, diagnostics);
    }
    }
    return 0.0;
}

} // namespace

float LayeredPresetSpec::LengthMs() const {
    float end = 0.0f;
    for (const auto& layer : layers) end = std::max(end, layer.startMs + layer.durationMs);
    return end;
}

std::string ValidateLayeredPreset(const LayeredPresetSpec& spec) {
    if (spec.layers.empty()) return "레이어가 없습니다";
    if (spec.layers.size() > kMaxLayers) return "레이어가 너무 많습니다 (최대 32개)";
    if (!Finite(spec.masterGain) || spec.masterGain < 0.0f || spec.masterGain > 4.0f)
        return "masterGain 은 0~4 사이여야 합니다";
    if (!Finite(spec.saturationCeiling) || spec.saturationCeiling < 0.0f ||
        spec.saturationCeiling > 1.0f)
        return "출력 포화 한계는 0(끔)~1 사이여야 합니다";
    if (spec.LengthMs() > kMaxPresetMs) return "프리셋 길이가 10초를 넘습니다";

    for (std::size_t i = 0; i < spec.layers.size(); ++i) {
        const auto& layer = spec.layers[i];
        const std::string where = "레이어 " + std::to_string(i + 1) + " (" + layer.name + "): ";
        if (!Finite(layer.startMs) || layer.startMs < 0.0f) return where + "시작 시각은 0 이상";
        if (!Finite(layer.durationMs) || layer.durationMs < 1.0f || layer.durationMs > kMaxPresetMs)
            return where + "지속시간은 1~10000 ms";
        if (!Finite(layer.amplitude) || layer.amplitude < 0.0f || layer.amplitude > 1.0f)
            return where + "진폭은 0~1";
        if (!Finite(layer.gainDb) || layer.gainDb < -60.0f || layer.gainDb > 24.0f)
            return where + "gain 은 -60~+24 dB";
        if (!Finite(layer.startPhaseDeg) || std::fabs(layer.startPhaseDeg) > 3600.0f)
            return where + "시작 위상이 범위를 벗어났습니다";
        if (!Finite(layer.leftGain) || layer.leftGain < 0.0f || layer.leftGain > 1.0f)
            return where + "왼쪽 출력 비율은 0~1";
        if (!Finite(layer.rightGain) || layer.rightGain < 0.0f || layer.rightGain > 1.0f)
            return where + "오른쪽 출력 비율은 0~1";

        if (layer.waveform != LayerWaveform::Formula) {
            if (!Finite(layer.frequencyHz) || layer.frequencyHz <= 0.0f || layer.frequencyHz > 2000.0f)
                return where + "주파수는 0보다 크고 2000 Hz 이하";
            if (layer.waveform == LayerWaveform::Chirp &&
                (!Finite(layer.frequencyEndHz) || layer.frequencyEndHz <= 0.0f ||
                 layer.frequencyEndHz > 2000.0f))
                return where + "끝 주파수는 0보다 크고 2000 Hz 이하";
            if (layer.waveform == LayerWaveform::AmSine) {
                if (!Finite(layer.modulationHz) || layer.modulationHz <= 0.0f ||
                    layer.modulationHz > 500.0f)
                    return where + "변조 주파수는 0보다 크고 500 Hz 이하";
                if (!Finite(layer.modulationDepth) || layer.modulationDepth < 0.0f ||
                    layer.modulationDepth > 1.0f)
                    return where + "변조 깊이는 0~1";
            }
            if (layer.waveform == LayerWaveform::Impulse &&
                (!Finite(layer.decayPerSecond) || layer.decayPerSecond < 0.0f ||
                 layer.decayPerSecond > 2000.0f))
                return where + "감쇠율은 0~2000";
        } else {
            Expression expression;
            const auto why = expression.Parse(layer.formula, {"t", "t0", "f"});
            if (!why.empty()) return where + why;
            // Only checked when the formula actually uses it. The formulas
            // this exists for divide by something built from t0, so a
            // non-positive one is the division-by-zero waiting to happen.
            if (layer.formula.find("t0") != std::string::npos &&
                (!Finite(layer.t0Seconds) || layer.t0Seconds <= 0.0f))
                return where + "t0 는 0보다 커야 합니다";
        }

        const auto& envelope = layer.envelope;
        if (envelope.enabled) {
            if (!Finite(envelope.attackMs) || envelope.attackMs < 0.0f ||
                !Finite(envelope.holdMs) || envelope.holdMs < 0.0f ||
                !Finite(envelope.decayMs) || envelope.decayMs < 0.0f ||
                !Finite(envelope.releaseMs) || envelope.releaseMs < 0.0f)
                return where + "포락선 구간은 0 이상이어야 합니다";
            if (!Finite(envelope.sustain) || envelope.sustain < 0.0f || envelope.sustain > 1.0f)
                return where + "포락선 유지 레벨은 0~1";
        }
    }
    return {};
}

RenderedPreset RenderLayeredPreset(const LayeredPresetSpec& spec, std::uint32_t sampleRate) {
    RenderedPreset out;
    out.sampleRate = sampleRate;
    out.error = ValidateLayeredPreset(spec);
    if (!out.error.empty()) return out;
    if (sampleRate < 8'000 || sampleRate > 192'000) {
        out.error = "지원하지 않는 샘플레이트입니다";
        return out;
    }

    const auto frames = static_cast<std::size_t>(
        std::ceil(static_cast<double>(spec.LengthMs()) / 1000.0 * sampleRate));
    if (frames == 0) { out.error = "프리셋 길이가 0입니다"; return out; }

    out.left.assign(frames, 0.0f);
    out.right.assign(frames, 0.0f);
    out.layerLeft.assign(spec.layers.size(), {});
    out.layerRight.assign(spec.layers.size(), {});

    // Solo wins over mute, and mutes everything else. It is an audition
    // control, so it never edits the preset -- turning it off restores
    // exactly what was there.
    const bool anySolo = std::any_of(spec.layers.begin(), spec.layers.end(),
                                     [](const WaveformLayer& l) { return l.solo; });

    for (std::size_t index = 0; index < spec.layers.size(); ++index) {
        const auto& layer = spec.layers[index];
        out.layerLeft[index].assign(frames, 0.0f);
        out.layerRight[index].assign(frames, 0.0f);
        if (anySolo ? !layer.solo : layer.muted) continue;

        Expression formula;
        if (layer.waveform == LayerWaveform::Formula)
            formula.Parse(layer.formula, {"t", "t0", "f"});

        // Start and length in SAMPLES. Everything downstream counts in these,
        // so two renders of the same preset line up to the sample.
        const auto startFrame = static_cast<std::size_t>(
            std::llround(static_cast<double>(layer.startMs) / 1000.0 * sampleRate));
        const auto layerFrames = static_cast<std::size_t>(
            std::llround(static_cast<double>(layer.durationMs) / 1000.0 * sampleRate));
        if (startFrame >= frames || layerFrames == 0) continue;

        const double duration = static_cast<double>(layer.durationMs) / 1000.0;
        const double gain = static_cast<double>(layer.amplitude) *
                            std::pow(10.0, static_cast<double>(layer.gainDb) / 20.0);
        bool scaled = false;

        const auto stop = std::min(frames, startFrame + layerFrames);
        for (std::size_t f = startFrame; f < stop; ++f) {
            const double t = static_cast<double>(f - startFrame) / sampleRate;
            const double presetT = static_cast<double>(f) / sampleRate;
            double value = SampleLayer(layer, t, presetT, formula, out.diagnostics.expression);
            value *= EnvelopeAt(layer.envelope, t, duration, scaled) * gain;
            if (!std::isfinite(value)) { ++out.diagnostics.nonFiniteSamples; value = 0.0; }
            out.layerLeft[index][f] = static_cast<float>(value * layer.leftGain);
            out.layerRight[index][f] = static_cast<float>(value * layer.rightGain);
        }
        if (scaled) out.diagnostics.envelopeScaled.push_back(layer.name);

        // Summed in floating point, and NOT normalised. Whatever it comes to
        // is what gets measured and what the limiter then has to deal with.
        for (std::size_t f = startFrame; f < stop; ++f) {
            out.left[f] += out.layerLeft[index][f];
            out.right[f] += out.layerRight[index][f];
        }
    }

    const float master = std::clamp(spec.masterGain, 0.0f, 4.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        out.left[f] *= master;
        out.right[f] *= master;
        if (!std::isfinite(out.left[f])) { ++out.diagnostics.nonFiniteSamples; out.left[f] = 0.0f; }
        if (!std::isfinite(out.right[f])) { ++out.diagnostics.nonFiniteSamples; out.right[f] = 0.0f; }
    }
    // The per-layer traces carry the master too, so what is drawn for a layer
    // is its real contribution to what was played rather than a different
    // scale that happens to look similar.
    for (auto& trace : out.layerLeft)
        for (float& v : trace) v *= master;
    for (auto& trace : out.layerRight)
        for (float& v : trace) v *= master;

    // 포화는 마스터 게인 **뒤**, 측정 **앞**이다. 그래야 화면의 숫자가 장치로
    // 나가는 신호를 말한다 -- 포화 전 값을 보여 주면 리미터가 무엇을 만나는지
    // 알 수 없다. 포화 전 값이 궁금하면 한계를 0 으로 두면 된다.
    if (spec.saturationCeiling > 0.0f) {
        const float limit = std::clamp(spec.saturationCeiling, 0.05f, 1.0f);
        // y = c * tanh(x / c). 한계 아래는 tanh(u) ~= u 라 거의 그대로 지나가고,
        // 훨씬 위는 한계에 점근할 뿐 모서리가 생기지 않는다. 보이스 코일에
        // 각진 모서리는 클릭이다.
        for (float& v : out.left) v = limit * std::tanh(v / limit);
        for (float& v : out.right) v = limit * std::tanh(v / limit);
        for (auto& trace : out.layerLeft)
            for (float& v : trace) v = limit * std::tanh(v / limit);
        for (auto& trace : out.layerRight)
            for (float& v : trace) v = limit * std::tanh(v / limit);
    }

    out.statsLeft = MeasureSignal(out.left);
    out.statsRight = MeasureSignal(out.right);
    out.ok = true;
    return out;
}

SignalStats MeasureSignal(const std::vector<float>& samples) {
    SignalStats stats;
    stats.frames = samples.size();
    if (samples.empty()) return stats;
    double sum = 0.0, sumSquares = 0.0;
    for (float v : samples) {
        const float magnitude = std::fabs(v);
        stats.peak = std::max(stats.peak, magnitude);
        if (magnitude > 1.0f) ++stats.overRange;
        sum += v;
        sumSquares += static_cast<double>(v) * v;
    }
    stats.mean = static_cast<float>(sum / static_cast<double>(samples.size()));
    stats.rms = static_cast<float>(std::sqrt(sumSquares / static_cast<double>(samples.size())));
    return stats;
}

LimiterPreview PreviewLimiter(const std::vector<float>& samples, std::uint32_t sampleRate,
                              float threshold, float attackMs, float releaseMs) {
    LimiterPreview preview;
    preview.samples = samples;
    if (samples.empty() || sampleRate == 0) return preview;
    const float limit = std::clamp(threshold, 0.05f, 1.0f);
    const float rate = static_cast<float>(sampleRate);
    const float attack = 1.0f - std::exp(-1.0f / std::max(1.0f, attackMs * rate / 1000.0f));
    const float release = 1.0f - std::exp(-1.0f / std::max(1.0f, releaseMs * rate / 1000.0f));

    float gain = 1.0f;
    for (float& v : preview.samples) {
        const float magnitude = std::fabs(v);
        const float wanted = magnitude > limit ? limit / magnitude : 1.0f;
        gain += (wanted - gain) * (wanted < gain ? attack : release);
        gain = std::clamp(gain, 0.0f, 1.0f);
        if (gain < 0.999f) {
            ++preview.limitedFrames;
            preview.worstReductionDb =
                std::max(preview.worstReductionDb, -20.0f * std::log10(std::max(gain, 1e-6f)));
        }
        v *= gain;
        if (std::fabs(v) > limit) {
            const float sign = v < 0.0f ? -1.0f : 1.0f;
            const float over = (std::fabs(v) - limit) / std::max(1e-6f, 1.0f - limit);
            v = sign * (limit + (1.0f - limit) * std::tanh(over));
        }
        if (v > 1.0f || v < -1.0f) ++preview.clampedSamples;
        v = std::clamp(v, -1.0f, 1.0f);
    }
    return preview;
}

FftResult AnalyzeSpectrum(const std::vector<float>& samples, std::uint32_t sampleRate,
                          std::size_t startFrame, std::size_t size, FftWindow window) {
    FftResult result;
    result.sampleRate = sampleRate;
    result.startFrame = startFrame;
    result.size = size;
    result.window = window;
    if (size < 64 || size > 32'768 || (size & (size - 1)) != 0) {
        result.error = "FFT 길이는 64~32768 사이의 2의 거듭제곱이어야 합니다";
        return result;
    }
    // Refused rather than zero-padded. A window half full of silence has a
    // different spectrum from the signal, and padding it would present that
    // difference as if it were the signal's.
    if (startFrame + size > samples.size()) {
        result.error = "분석 구간이 신호 끝을 넘어갑니다";
        return result;
    }

    std::vector<std::complex<double>> buffer(size);
    double coherentGain = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
        double w = 1.0;
        const double ratio = static_cast<double>(i) / static_cast<double>(size - 1);
        if (window == FftWindow::Hann) w = 0.5 - 0.5 * std::cos(kTwoPi * ratio);
        else if (window == FftWindow::Hamming) w = 0.54 - 0.46 * std::cos(kTwoPi * ratio);
        coherentGain += w;
        buffer[i] = std::complex<double>(samples[startFrame + i] * w, 0.0);
    }
    coherentGain /= static_cast<double>(size);

    // Iterative radix-2: bit-reversal, then log2(N) butterfly stages.
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        std::size_t bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(buffer[i], buffer[j]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const double angle = -kTwoPi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < size; i += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t k = 0; k < length / 2; ++k) {
                const auto even = buffer[i + k];
                const auto odd = buffer[i + k + length / 2] * twiddle;
                buffer[i + k] = even + odd;
                buffer[i + k + length / 2] = even - odd;
                twiddle *= step;
            }
        }
    }

    const std::size_t bins = size / 2 + 1;
    result.frequencyHz.resize(bins);
    result.magnitude.resize(bins);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        result.frequencyHz[bin] =
            static_cast<float>(bin) * static_cast<float>(sampleRate) / static_cast<float>(size);
        // 2/N for the one-sided spectrum, divided by the window's coherent
        // gain so a full-scale sine reads ~1.0 whatever window is chosen.
        // The unit is PCM sample amplitude -- not dB, and not anything physical.
        const double scale = 2.0 / (static_cast<double>(size) * std::max(1e-9, coherentGain));
        result.magnitude[bin] = static_cast<float>(std::abs(buffer[bin]) * scale);
    }
    result.ok = true;
    return result;
}

const char* ToString(LayerWaveform waveform) {
    switch (waveform) {
    case LayerWaveform::Sine: return "sine";
    case LayerWaveform::Impulse: return "impulse";
    case LayerWaveform::Chirp: return "chirp";
    case LayerWaveform::AmSine: return "am";
    case LayerWaveform::Formula: return "formula";
    }
    return "sine";
}

const char* ToString(FftWindow window) {
    switch (window) {
    case FftWindow::Rectangular: return "rect";
    case FftWindow::Hann: return "hann";
    case FftWindow::Hamming: return "hamming";
    }
    return "hann";
}

bool ParseWaveform(const std::string& text, LayerWaveform& out) {
    if (text == "sine") { out = LayerWaveform::Sine; return true; }
    if (text == "impulse") { out = LayerWaveform::Impulse; return true; }
    if (text == "chirp") { out = LayerWaveform::Chirp; return true; }
    if (text == "am") { out = LayerWaveform::AmSine; return true; }
    if (text == "formula") { out = LayerWaveform::Formula; return true; }
    return false;
}

bool ParseFftWindow(const std::string& text, FftWindow& out) {
    if (text == "rect") { out = FftWindow::Rectangular; return true; }
    if (text == "hann") { out = FftWindow::Hann; return true; }
    if (text == "hamming") { out = FftWindow::Hamming; return true; }
    return false;
}

} // namespace sekiro_haptics::lab
