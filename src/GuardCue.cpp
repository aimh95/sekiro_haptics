#include "sekiro_haptics/GuardCue.hpp"

#include "sekiro_haptics/Json.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>

namespace sekiro_haptics {
namespace {

constexpr float kTwoPi = 6.283185307179586f;

float Finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

/// xorshift32 so a rendered clip is bit-identical on every run and platform.
struct Noise {
    std::uint32_t state;
    explicit Noise(std::uint32_t seed) : state(seed ? seed : 0x2545F491u) {}
    float Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state & 0xFFFFu) / 32767.5f - 1.0f;
    }
};

double Number(const json::JsonValue& parent, const char* key, double fallback) {
    const auto* v = parent.Find(key);
    return (v && v->IsNumber() && std::isfinite(v->AsNumber())) ? v->AsNumber() : fallback;
}

void AppendNumber(std::ostringstream& out, const char* key, double value, bool last = false) {
    out << "\"" << key << "\": " << value << (last ? "" : ",");
}

} // namespace

GuardCueConfig DefaultGuardCueConfig() {
    GuardCueConfig c;

    // Established on this machine by listening and by feel (docs/11-guard-feedback.md):
    // endpoint 6, channel 1 = built-in speaker, channels 2/3 = left/right voice coils.
    // Another machine will differ -- re-run --channel-test there.
    c.device.audioEndpointIndex = 6;
    c.device.speakerChannel = 1;
    c.device.hapticLeftChannel = 2;
    c.device.hapticRightChannel = 3;

    // ---- deflect: bright, tight, inharmonic; dies quickly ----------------
    c.deflect.name = "deflect";
    // The supplied recording, played as recorded. Slicing it to a single
    // impact made it 66 ms, and 66 ms of a clang reads as cut off; rebuilding
    // a tail from its own material did not settle it either. So the file goes
    // out whole and the impacts it contains are heard.
    c.deflect.speaker.clipPath = "audio/sekiro_deflect.mp3";
    c.deflect.speaker.clipRaw = true;
    c.deflect.speaker.durationMs = 140.0f;
    c.deflect.speaker.attackMs = 1.0f;
    c.deflect.speaker.releaseMs = 14.0f;
    c.deflect.speaker.baseHz = 2350.0f;
    c.deflect.speaker.partials = {
        {1.00f, 0.50f, 26.0f},
        {1.51f, 0.34f, 34.0f},
        {2.13f, 0.24f, 46.0f},
        {2.87f, 0.15f, 62.0f},
        {3.74f, 0.09f, 84.0f},
    };
    c.deflect.speaker.strikeNoiseAmplitude = 0.30f;
    c.deflect.speaker.strikeNoiseDecayPerSecond = 220.0f;
    c.deflect.speaker.strikeNoiseHighpass = 0.75f;
    c.deflect.speaker.gain = 0.45f;
    // Much quieter. This is the speaker clip's own trim, so it does not touch
    // the haptic channels, the controller's volume byte, or any Windows level
    // -- the haptics stay exactly as strong. -18 dB is about an eighth of the
    // previous amplitude. Tune with --speaker-trim-db.
    c.deflect.speaker.trimDb = -18.0f;
    // The slice can only be 66 ms (another strike follows), so the ring is
    // rebuilt from the slice's own late material instead of being lost.
    // The ring has to stay AUDIBLE for its whole length, not merely exist.
    // At 13/s it fell below -20 dB of the attack by 200 ms and to -38 dB by
    // 300 ms, which on this speaker is silence -- so the cue still sounded cut
    // off even though the samples were there. 4/s ends around -11 dB below the
    // join instead.
    c.deflect.speaker.tailMs = 0.0f;

    // Three overlapping layers: a sharp 260->220 Hz contact, a 205 Hz body,
    // and a very thin 250 Hz ring that outlives both. Sharp touch, fine metal
    // ring, clean end. First prototype values, not device optima.
    // Matched to the SPEAKER CLIP's own length (deflect_hit.wav is 378 ms),
    // so the felt impact and the sound stop together instead of one of them
    // ending early. The extra length is all in the ring layer: contact and
    // body -- where the strength is -- are unchanged, so this is a longer
    // fading tail, not a longer buzz.
    c.deflect.haptic.totalMs = 378.0f;
    // A DIGITAL amplitude, not a percentage of force. 0.85 is ~9 dB above the
    // original 0.30 and leaves 15% headroom before full scale, which the
    // overlapping tail of a previous cue can use. The actuator's output is
    // NOT a linear function of this, and past some excursion it rattles
    // instead of pushing harder -- so if this still feels weak, the next thing
    // to change is the waveform, not another gain step.
    c.deflect.haptic.normalizePeak = 0.85f;
    //                          start   end      f0      f1   rise   hold  weight  decay
    c.deflect.haptic.contact = { 0.0f,  10.0f, 260.0f, 220.0f, 0.7f,  0.0f, 0.90f, 170.0f};
    c.deflect.haptic.body    = { 4.0f,  75.0f, 190.0f, 190.0f, 3.0f, 22.0f, 1.00f,  26.0f};
    c.deflect.haptic.ring    = {40.0f, 378.0f, 250.0f, 250.0f, 5.0f, 10.0f, 0.20f,   9.0f};
    c.deflect.haptic.noiseAmplitude = 0.10f;
    c.deflect.haptic.gain = 1.0f;

    // ---- block: still metal, but lower, thicker, less bright -------------
    c.block.name = "block";
    c.block.speaker.clipPath = "audio/sekiro_parry.mp3";
    c.block.speaker.clipRaw = true;
    c.block.speaker.durationMs = 95.0f;
    c.block.speaker.attackMs = 4.0f;
    c.block.speaker.releaseMs = 18.0f;
    c.block.speaker.baseHz = 1250.0f;
    c.block.speaker.partials = {
        {1.00f, 0.52f, 34.0f},
        {1.41f, 0.30f, 44.0f},
        {2.06f, 0.14f, 70.0f},
        {2.94f, 0.05f, 110.0f},
    };
    c.block.speaker.strikeNoiseAmplitude = 0.22f;
    c.block.speaker.strikeNoiseDecayPerSecond = 120.0f;
    c.block.speaker.strikeNoiseHighpass = 0.30f;
    c.block.speaker.gain = 0.45f;
    c.block.speaker.trimDb = -18.0f;
    c.block.speaker.tailMs = 0.0f;

    // Same three layers, moved down and widened: a softer 185 Hz contact, a
    // rounder 115 Hz body that carries the weight, and a quieter 210 Hz ring.
    // Must stay metallic -- not a wooden knock, not a long hum.
    // Same rule: block_hit.wav is 500 ms.
    c.block.haptic.totalMs = 500.0f;
    c.block.haptic.normalizePeak = 0.85f;
    c.block.haptic.contact = { 0.0f,  14.0f, 180.0f, 180.0f, 1.6f,  0.0f, 0.80f, 130.0f};
    c.block.haptic.body    = { 4.0f, 100.0f, 145.0f, 145.0f, 4.0f, 28.0f, 1.00f,  18.0f};
    c.block.haptic.ring    = {55.0f, 500.0f, 210.0f, 210.0f, 6.0f, 12.0f, 0.20f,   7.0f};
    c.block.haptic.noiseAmplitude = 0.06f;
    c.block.haptic.gain = 1.0f;

    return c;
}

std::vector<float> SynthesizeGuardSpeaker(const SpeakerCueProfile& p, std::uint32_t rate,
                                          float masterVolume) {
    if (rate < 8'000 || rate > 192'000) return {};
    const float duration = std::clamp(Finite(p.durationMs, 140.0f), 5.0f, 2'000.0f) / 1000.0f;
    const float attack = std::max(0.0002f, Finite(p.attackMs, 1.0f) / 1000.0f);
    const float release = std::max(0.001f, Finite(p.releaseMs, 12.0f) / 1000.0f);
    const float base = std::clamp(Finite(p.baseHz, 2400.0f), 40.0f, static_cast<float>(rate) * 0.45f);
    const float gain = std::clamp(Finite(p.gain, 0.45f) * Finite(masterVolume, 1.0f), 0.0f, 1.0f);

    const auto samples = static_cast<std::size_t>(duration * static_cast<float>(rate));
    std::vector<float> pcm(samples, 0.0f);
    Noise noise(0x5EC1'11A0u);
    float previousNoise = 0.0f;
    const float highpass = std::clamp(Finite(p.strikeNoiseHighpass, 0.5f), 0.0f, 1.0f);

    for (std::size_t i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        float body = 0.0f;
        for (const auto& partial : p.partials) {
            const float hz = base * Finite(partial.ratio, 1.0f);
            if (hz >= static_cast<float>(rate) * 0.48f) continue;   // never alias
            body += Finite(partial.amplitude, 0.0f) *
                    std::sin(kTwoPi * hz * t) *
                    std::exp(-std::max(0.0f, Finite(partial.decayPerSecond, 20.0f)) * t);
        }
        // The strike: a very short noise burst, brightened by a one-pole
        // high-pass so it reads as metal-on-metal rather than a thud.
        const float raw = noise.Next();
        const float bright = raw - highpass * previousNoise;
        previousNoise = raw;
        const float strike = Finite(p.strikeNoiseAmplitude, 0.25f) * bright *
                             std::exp(-std::max(0.0f, Finite(p.strikeNoiseDecayPerSecond, 160.0f)) * t);

        const float rise = std::min(1.0f, t / attack);
        const float fall = std::min(1.0f, std::max(0.0f, duration - t) / release);
        pcm[i] = std::clamp((body + strike) * rise * fall * gain, -1.0f, 1.0f);
    }
    return pcm;
}

namespace {

/// One layer's contribution at time `t` (seconds from the start of the cue).
float LayerAt(const HapticLayer& layer, float t, float rate) {
    const float start = std::max(0.0f, Finite(layer.startMs, 0.0f)) / 1000.0f;
    const float end = std::max(start + 0.001f, Finite(layer.endMs, 30.0f) / 1000.0f);
    if (t < start || t >= end) return 0.0f;
    const float local = t - start;
    const float span = end - start;
    const float rise = std::max(0.0002f, Finite(layer.riseMs, 1.0f) / 1000.0f);

    // A glide is integrated so the phase stays continuous; stepping the
    // frequency per sample would click.
    const float f0 = std::clamp(Finite(layer.startHz, 200.0f), 20.0f, rate * 0.45f);
    const float f1 = std::clamp(Finite(layer.endHz, f0), 20.0f, rate * 0.45f);
    const float phase = kTwoPi * (f0 * local + 0.5f * (f1 - f0) / span * local * local);

    const float attack = std::min(1.0f, local / rise);
    // Rise, then HOLD, then decay. The decay clock starts at the end of the
    // hold, not at the start of the layer, so the body keeps its weight for a
    // real span instead of being a spike that happens to be labelled "body".
    const float hold = std::max(0.0f, Finite(layer.holdMs, 0.0f)) / 1000.0f;
    const float decayFrom = std::max(0.0f, local - rise - hold);
    const float decay = std::exp(-std::max(0.0f, Finite(layer.decayPerSecond, 40.0f)) * decayFrom);
    // Taper the last 3 ms of the layer's own window so a layer ending mid-cue
    // cannot step.
    const float close = std::min(1.0f, (end - t) / 0.003f);
    return Finite(layer.weight, 0.0f) * std::sin(phase) * attack * decay * close;
}

} // namespace

std::vector<float> SynthesizeGuardHaptic(const HapticCueProfile& p, std::uint32_t rate,
                                         float masterStrength) {
    if (rate < 8'000 || rate > 192'000) return {};
    const float total = std::clamp(Finite(p.totalMs, 72.0f), 5.0f, 1'000.0f) / 1000.0f;
    const auto samples = static_cast<std::size_t>(total * static_cast<float>(rate));
    if (samples == 0) return {};
    std::vector<float> pcm(samples, 0.0f);
    Noise noise(0x1234'ABCDu);
    const float fr = static_cast<float>(rate);

    for (std::size_t i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / fr;
        float value = LayerAt(p.contact, t, fr) + LayerAt(p.body, t, fr) + LayerAt(p.ring, t, fr);
        value += Finite(p.noiseAmplitude, 0.10f) * noise.Next() * std::exp(-300.0f * t);
        // Clean end: the last 4 ms always reach zero, so a cue never leaves the
        // actuator held.
        value *= std::min(1.0f, std::max(0.0f, total - t) / 0.004f);
        pcm[i] = value;
    }

    // Peak-normalise, then apply the masters. Normalising first is what keeps
    // the two profiles comparable at the same digital peak.
    float peak = 0.0f;
    for (float v : pcm) peak = std::max(peak, std::fabs(v));
    const float target = std::clamp(Finite(p.normalizePeak, 0.30f), 0.0f, 1.0f);
    const float scale = peak > 1e-6f ? target / peak : 0.0f;
    const float master = std::clamp(Finite(p.gain, 1.0f) * Finite(masterStrength, 1.0f), 0.0f, 4.0f);
    for (float& v : pcm) v = std::clamp(v * scale * master, -1.0f, 1.0f);
    return pcm;
}

namespace {

/// One-pole sections, cascaded twice for ~12 dB/oct. Deliberately simple and
/// deliberately gentle: the point is an A/B that can be reversed, not a
/// surgical filter.
void HighpassTwice(std::vector<float>& clip, float rate, float hz) {
    const float rc = 1.0f / (kTwoPi * hz);
    const float dt = 1.0f / rate;
    const float a = rc / (rc + dt);
    for (int pass = 0; pass < 2; ++pass) {
        float prevIn = clip.empty() ? 0.0f : clip.front();
        float prevOut = 0.0f;
        for (float& v : clip) {
            const float in = v;
            prevOut = a * (prevOut + in - prevIn);
            prevIn = in;
            v = prevOut;
        }
    }
}

void LowpassTwice(std::vector<float>& clip, float rate, float hz) {
    const float rc = 1.0f / (kTwoPi * hz);
    const float dt = 1.0f / rate;
    const float alpha = dt / (rc + dt);
    for (int pass = 0; pass < 2; ++pass) {
        float out = 0.0f;
        for (float& v : clip) {
            out += (v - out) * alpha;
            v = out;
        }
    }
}

void Measure(const std::vector<float>& clip, float& peak, float& rms) {
    peak = 0.0f;
    double sum = 0.0;
    for (float v : clip) { peak = std::max(peak, std::fabs(v)); sum += static_cast<double>(v) * v; }
    rms = clip.empty() ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(clip.size())));
}

} // namespace

void ExtendDecayTail(std::vector<float>& clip, std::uint32_t sampleRate,
                     float tailMs, float decayPerSecond, float grainMs) {
    if (clip.empty() || sampleRate < 8000) return;
    tailMs = Finite(tailMs, 0.0f);
    if (tailMs < 5.0f) return;
    const float rate = static_cast<float>(sampleRate);
    const auto tailSamples = static_cast<std::size_t>(std::min(tailMs, 2000.0f) * rate / 1000.0f);
    const auto grain = std::max<std::size_t>(64, static_cast<std::size_t>(
        std::clamp(Finite(grainMs, 18.0f), 4.0f, 80.0f) * rate / 1000.0f));
    // Grains come from the last third only: that region is ring, not attack.
    const std::size_t sourceBegin = clip.size() / 3;
    const std::size_t sourceSpan = clip.size() - sourceBegin;
    if (sourceSpan < grain * 2 || tailSamples < grain) return;

    // 1. Raw grain material, NO envelope yet. Deterministic offsets so a
    //    rendered cue is reproducible.
    std::vector<float> tail(tailSamples, 0.0f);
    Noise pick(0x51ED'270Bu);
    const std::size_t hop = grain / 2;
    for (std::size_t start = 0; start < tailSamples; start += hop) {
        const float jitter = 0.5f * (pick.Next() + 1.0f);
        const auto offset = sourceBegin +
            static_cast<std::size_t>(jitter * static_cast<float>(sourceSpan - grain));
        for (std::size_t i = 0; i < grain && start + i < tailSamples; ++i) {
            float w = 0.5f - 0.5f * std::cos(kTwoPi * static_cast<float>(i) /
                                             static_cast<float>(grain));
            // The FIRST grain must not fade in. Its rising Hann half started
            // at zero, so the ring faded up over half a grain (~9 ms) and left
            // a hole right at the join -- measured as a dip to 0.0075 between
            // 0.0156 and 0.0200. Only its falling half is windowed.
            if (start == 0 && i < grain / 2) w = 1.0f;
            tail[start + i] += clip[offset + i] * w;
        }
    }

    // 2. The envelope is IMPOSED, not multiplied in. Grains taken at random
    //    offsets are uncorrelated, so a Hann crossfade does not sum to a
    //    constant -- the raw material wanders by tens of percent, and a rise
    //    in the tail is exactly what would read as a second hit. So the
    //    measured envelope is divided out and the intended exponential put in
    //    its place, which makes the decay monotonic by construction.
    // 2. The envelope is IMPOSED per block, not multiplied in.
    //
    // Two weaker attempts first, both measured and both rejected:
    //  - multiplying the raw grains by exp(-decay*t): grains taken at random
    //    offsets are uncorrelated, so a Hann crossfade does not sum to a
    //    constant and the level wandered by tens of percent;
    //  - dividing by a running MAX envelope: a running max lags, holding a
    //    peak for the width of its window, so the result dipped after each
    //    peak left the window instead of flattening. Measured buckets still
    //    rose 0.00297 -> 0.00547.
    //
    // So each short block's PEAK is placed exactly on the intended
    // exponential, and the per-block gains are interpolated so no block
    // boundary is audible. A rise in the tail is what would read as a second
    // hit, and this makes one arithmetically impossible.
    const auto block = std::max<std::size_t>(16, static_cast<std::size_t>(0.005f * rate));
    const std::size_t blocks = (tailSamples + block - 1) / block;

    // Where the ring starts.
    //
    // The tail's envelope is imposed as a block PEAK, but what is heard at the
    // join is the short-term MEAN, and for ring material the peak sits two to
    // three times above it. Anchoring the peak to the clip's end peak put the
    // tail's mean well above the clip's (measured 0.0051 -> 0.0137), which is
    // audible as the ring stepping in. So the clip's end MEAN is matched, and
    // the required peak is derived from the tail material's own peak-to-mean
    // ratio. A 15 ms peak was tried first and was worse still (2.9x step).
    const std::size_t edgeSpan = std::min<std::size_t>(block, clip.size());
    float clipEndPeak = 0.0f;
    double clipEndSum = 0.0;
    for (std::size_t i = clip.size() - edgeSpan; i < clip.size(); ++i) {
        clipEndPeak = std::max(clipEndPeak, std::fabs(clip[i]));
        clipEndSum += std::fabs(clip[i]);
    }
    const float clipEndMean = static_cast<float>(clipEndSum / static_cast<double>(edgeSpan));

    float tailHeadPeak = 0.0f;
    double tailHeadSum = 0.0;
    const std::size_t headSpan = std::min<std::size_t>(block, tailSamples);
    for (std::size_t i = 0; i < headSpan; ++i) {
        tailHeadPeak = std::max(tailHeadPeak, std::fabs(tail[i]));
        tailHeadSum += std::fabs(tail[i]);
    }
    const float tailHeadMean = static_cast<float>(tailHeadSum / static_cast<double>(headSpan));
    const float peakOverMean = tailHeadMean > 1e-6f ? tailHeadPeak / tailHeadMean : 1.0f;
    // The envelope is imposed as a block PEAK, so the anchor has to be a peak
    // too -- but derived from the clip's end MEAN, because that is what is
    // heard at the join. Anchoring straight to the clip's end peak put the
    // ring's mean well above the recording's (0.0051 -> 0.0137).
    const float startLevel = std::min(clipEndMean * peakOverMean, clipEndPeak);

    const float decay = std::max(0.5f, Finite(decayPerSecond, 11.0f));
    std::vector<float> gains(blocks, 0.0f);
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t begin = b * block;
        const std::size_t end = std::min(begin + block, tailSamples);
        float peak = 0.0f;
        for (std::size_t i = begin; i < end; ++i) peak = std::max(peak, std::fabs(tail[i]));
        const float t = (static_cast<float>(begin) + 0.5f * static_cast<float>(block)) / rate;
        const float target = startLevel * std::exp(-decay * t);
        gains[b] = peak > 1e-6f ? target / peak : 0.0f;
    }

    const std::size_t original = clip.size();
    clip.resize(original + tailSamples, 0.0f);
    for (std::size_t i = 0; i < tailSamples; ++i) {
        // Interpolate between block centres so the gain curve is continuous.
        const float pos = static_cast<float>(i) / static_cast<float>(block) - 0.5f;
        const auto lower = static_cast<std::ptrdiff_t>(std::floor(pos));
        const float frac = pos - static_cast<float>(lower);
        const auto at = [&](std::ptrdiff_t k) {
            if (k < 0) return gains.front();
            if (static_cast<std::size_t>(k) >= blocks) return gains.back();
            return gains[static_cast<std::size_t>(k)];
        };
        clip[original + i] = tail[i] * (at(lower) * (1.0f - frac) + at(lower + 1) * frac);
    }
    // Reach exactly zero at the very end so the cue never ends on a step.
    const auto fade = std::min<std::size_t>(tailSamples, static_cast<std::size_t>(0.012f * rate));
    for (std::size_t i = 0; i < fade; ++i) {
        const float g = static_cast<float>(fade - i) / static_cast<float>(fade);
        clip[clip.size() - fade + i] *= g;
    }
}

SpeakerShapingReport ApplySpeakerShaping(std::vector<float>& clip, std::uint32_t sampleRate,
                                         const SpeakerCueProfile& profile) {
    SpeakerShapingReport report;
    if (clip.empty() || sampleRate < 8000) return report;
    Measure(clip, report.peakBefore, report.rmsBefore);
    const float rate = static_cast<float>(sampleRate);
    const float hp = Finite(profile.highpassHz, 0.0f);
    const float lp = Finite(profile.lowpassHz, 0.0f);
    if (hp > 20.0f && hp < rate * 0.45f) { HighpassTwice(clip, rate, hp); report.highpassApplied = true; }
    if (lp > 200.0f && lp < rate * 0.45f) { LowpassTwice(clip, rate, lp); report.lowpassApplied = true; }
    report.trimDb = std::clamp(Finite(profile.trimDb, 0.0f), -40.0f, 12.0f);
    if (report.trimDb != 0.0f) {
        const float scale = std::pow(10.0f, report.trimDb / 20.0f);
        for (float& v : clip) v *= scale;
    }
    Measure(clip, report.peakAfter, report.rmsAfter);
    return report;
}

float PeakAmplitude(const std::vector<float>& clip) {
    float peak = 0.0f;
    for (float s : clip) peak = std::max(peak, std::fabs(s));
    return peak;
}

namespace {

void ReadSpeaker(const json::JsonValue& v, SpeakerCueProfile& s) {
    if (const auto* clip = v.Find("clipPath"); clip && clip->IsString()) s.clipPath = clip->AsString();
    if (const auto* raw = v.Find("clipRaw"); raw) s.clipRaw = raw->IsBool() ? raw->AsBool()
                                                                           : Number(v, "clipRaw", 0.0) != 0.0;
    s.clipPeak = static_cast<float>(Number(v, "clipPeak", s.clipPeak));
    s.durationMs = static_cast<float>(Number(v, "durationMs", s.durationMs));
    s.attackMs = static_cast<float>(Number(v, "attackMs", s.attackMs));
    s.releaseMs = static_cast<float>(Number(v, "releaseMs", s.releaseMs));
    s.baseHz = static_cast<float>(Number(v, "baseHz", s.baseHz));
    s.strikeNoiseAmplitude = static_cast<float>(Number(v, "strikeNoiseAmplitude", s.strikeNoiseAmplitude));
    s.strikeNoiseDecayPerSecond = static_cast<float>(Number(v, "strikeNoiseDecayPerSecond", s.strikeNoiseDecayPerSecond));
    s.strikeNoiseHighpass = static_cast<float>(Number(v, "strikeNoiseHighpass", s.strikeNoiseHighpass));
    s.gain = static_cast<float>(Number(v, "gain", s.gain));
    s.trimDb = static_cast<float>(Number(v, "trimDb", s.trimDb));
    s.highpassHz = static_cast<float>(Number(v, "highpassHz", s.highpassHz));
    s.lowpassHz = static_cast<float>(Number(v, "lowpassHz", s.lowpassHz));
    s.tailMs = static_cast<float>(Number(v, "tailMs", s.tailMs));
    s.tailDecayPerSecond = static_cast<float>(Number(v, "tailDecayPerSecond", s.tailDecayPerSecond));
    s.tailGrainMs = static_cast<float>(Number(v, "tailGrainMs", s.tailGrainMs));
    if (const auto* parts = v.Find("partials"); parts && parts->IsArray()) {
        std::vector<CuePartial> list;
        for (const auto& p : parts->AsArray()) {
            if (!p.IsObject()) continue;
            CuePartial cp;
            cp.ratio = static_cast<float>(Number(p, "ratio", 1.0));
            cp.amplitude = static_cast<float>(Number(p, "amplitude", 0.0));
            cp.decayPerSecond = static_cast<float>(Number(p, "decayPerSecond", 20.0));
            list.push_back(cp);
        }
        if (!list.empty()) s.partials = std::move(list);
    }
}

void ReadLayer(const json::JsonValue& parent, const char* key, HapticLayer& layer) {
    const auto* v = parent.Find(key);
    if (!v || !v->IsObject()) return;
    layer.startMs = static_cast<float>(Number(*v, "startMs", layer.startMs));
    layer.endMs = static_cast<float>(Number(*v, "endMs", layer.endMs));
    layer.startHz = static_cast<float>(Number(*v, "startHz", layer.startHz));
    layer.endHz = static_cast<float>(Number(*v, "endHz", layer.endHz));
    layer.riseMs = static_cast<float>(Number(*v, "riseMs", layer.riseMs));
    layer.holdMs = static_cast<float>(Number(*v, "holdMs", layer.holdMs));
    layer.weight = static_cast<float>(Number(*v, "weight", layer.weight));
    layer.decayPerSecond = static_cast<float>(Number(*v, "decayPerSecond", layer.decayPerSecond));
}

void ReadHaptic(const json::JsonValue& v, HapticCueProfile& h) {
    h.totalMs = static_cast<float>(Number(v, "totalMs", h.totalMs));
    h.normalizePeak = static_cast<float>(Number(v, "normalizePeak", h.normalizePeak));
    h.noiseAmplitude = static_cast<float>(Number(v, "noiseAmplitude", h.noiseAmplitude));
    h.gain = static_cast<float>(Number(v, "gain", h.gain));
    ReadLayer(v, "contact", h.contact);
    ReadLayer(v, "body", h.body);
    ReadLayer(v, "ring", h.ring);
}

void ReadProfile(const json::JsonValue& v, GuardCueProfile& p) {
    if (const auto* s = v.Find("speaker"); s && s->IsObject()) ReadSpeaker(*s, p.speaker);
    if (const auto* h = v.Find("haptic"); h && h->IsObject()) ReadHaptic(*h, p.haptic);
    p.balance = static_cast<float>(Number(v, "balance", p.balance));
}

} // namespace

bool ParseGuardCueConfig(const std::string& text, GuardCueConfig& out, std::string& error) {
    auto parsed = json::ParseJson(text);
    if (!parsed.ok) { error = parsed.error; return false; }
    if (!parsed.value.IsObject()) { error = "guard cue config must be a JSON object"; return false; }
    GuardCueConfig config = DefaultGuardCueConfig();
    if (const auto* dev = parsed.value.Find("device"); dev && dev->IsObject()) {
        auto& d = config.device;
        d.audioEndpointIndex = static_cast<int>(Number(*dev, "audioEndpointIndex", d.audioEndpointIndex));
        d.speakerChannel = static_cast<int>(Number(*dev, "speakerChannel", d.speakerChannel));
        d.hapticLeftChannel = static_cast<int>(Number(*dev, "hapticLeftChannel", d.hapticLeftChannel));
        d.hapticRightChannel = static_cast<int>(Number(*dev, "hapticRightChannel", d.hapticRightChannel));
        d.controllerSpeakerVolume = static_cast<int>(Number(*dev, "controllerSpeakerVolume", d.controllerSpeakerVolume));
        d.audioOutputPath = static_cast<int>(Number(*dev, "audioOutputPath", d.audioOutputPath));
        d.speakerPreGain = static_cast<int>(Number(*dev, "speakerPreGain", d.speakerPreGain));
        d.windowsEndpointVolume = static_cast<float>(Number(*dev, "windowsEndpointVolume", d.windowsEndpointVolume));
    }
    if (const auto* d = parsed.value.Find("deflect"); d && d->IsObject()) ReadProfile(*d, config.deflect);
    if (const auto* b = parsed.value.Find("block"); b && b->IsObject()) ReadProfile(*b, config.block);
    config.speakerVolume = static_cast<float>(Number(parsed.value, "speakerVolume", config.speakerVolume));
    config.hapticStrength = static_cast<float>(Number(parsed.value, "hapticStrength", config.hapticStrength));
    config.retriggerDuck = static_cast<float>(Number(parsed.value, "retriggerDuck", config.retriggerDuck));
    config.retireFadeMs = static_cast<float>(Number(parsed.value, "retireFadeMs", config.retireFadeMs));
    config.limiterThreshold = static_cast<float>(Number(parsed.value, "limiterThreshold", config.limiterThreshold));
    config.speakerHeadroom = static_cast<float>(Number(parsed.value, "speakerHeadroom", config.speakerHeadroom));
    config.hapticHeadroom = static_cast<float>(Number(parsed.value, "hapticHeadroom", config.hapticHeadroom));
    config.maxOutputLatencyUs = static_cast<std::int64_t>(
        Number(parsed.value, "maxOutputLatencyUs", static_cast<double>(config.maxOutputLatencyUs)));
    config.voiceLimit = static_cast<std::size_t>(
        std::max(1.0, Number(parsed.value, "voiceLimit", static_cast<double>(config.voiceLimit))));
    out = std::move(config);
    return true;
}

std::string SerializeGuardCueConfig(const GuardCueConfig& c) {
    std::ostringstream out;
    out.precision(6);
    auto profile = [&out](const char* key, const GuardCueProfile& p, bool last) {
        out << "  \"" << key << "\": {\n    \"speaker\": {";
        // A recording, when one is configured. Empty means "use the synthesised
        // partials below instead", so the key is only written when it is set.
        if (!p.speaker.clipPath.empty())
            out << "\"clipPath\": \"" << p.speaker.clipPath << "\", ";
        if (!p.speaker.clipPath.empty())
            out << "\"clipRaw\": " << (p.speaker.clipRaw ? "true" : "false") << ", ";
        AppendNumber(out, "clipPeak", p.speaker.clipPeak);
        AppendNumber(out, "durationMs", p.speaker.durationMs);
        AppendNumber(out, "attackMs", p.speaker.attackMs);
        AppendNumber(out, "releaseMs", p.speaker.releaseMs);
        AppendNumber(out, "baseHz", p.speaker.baseHz);
        AppendNumber(out, "strikeNoiseAmplitude", p.speaker.strikeNoiseAmplitude);
        AppendNumber(out, "strikeNoiseDecayPerSecond", p.speaker.strikeNoiseDecayPerSecond);
        AppendNumber(out, "strikeNoiseHighpass", p.speaker.strikeNoiseHighpass);
        AppendNumber(out, "gain", p.speaker.gain);
        AppendNumber(out, "trimDb", p.speaker.trimDb);
        AppendNumber(out, "highpassHz", p.speaker.highpassHz);
        AppendNumber(out, "lowpassHz", p.speaker.lowpassHz);
        AppendNumber(out, "tailMs", p.speaker.tailMs);
        AppendNumber(out, "tailDecayPerSecond", p.speaker.tailDecayPerSecond);
        AppendNumber(out, "tailGrainMs", p.speaker.tailGrainMs);
        out << "\"partials\": [";
        for (std::size_t i = 0; i < p.speaker.partials.size(); ++i) {
            const auto& q = p.speaker.partials[i];
            out << "{\"ratio\": " << q.ratio << ", \"amplitude\": " << q.amplitude
                << ", \"decayPerSecond\": " << q.decayPerSecond << "}"
                << (i + 1 < p.speaker.partials.size() ? ", " : "");
        }
        out << "]},\n    \"haptic\": {";
        AppendNumber(out, "totalMs", p.haptic.totalMs);
        AppendNumber(out, "normalizePeak", p.haptic.normalizePeak);
        AppendNumber(out, "noiseAmplitude", p.haptic.noiseAmplitude);
        AppendNumber(out, "gain", p.haptic.gain);
        auto layer = [&out](const char* key, const HapticLayer& l, bool last) {
            out << "\"" << key << "\": {\"startMs\": " << l.startMs << ", \"endMs\": " << l.endMs
                << ", \"startHz\": " << l.startHz << ", \"endHz\": " << l.endHz
                << ", \"riseMs\": " << l.riseMs << ", \"holdMs\": " << l.holdMs
                << ", \"weight\": " << l.weight
                << ", \"decayPerSecond\": " << l.decayPerSecond << "}" << (last ? "" : ", ");
        };
        layer("contact", p.haptic.contact, false);
        layer("body", p.haptic.body, false);
        layer("ring", p.haptic.ring, true);
        out << "},\n    \"balance\": " << p.balance << "\n  }" << (last ? "" : ",") << "\n";
    };
    // The channel map is machine-specific and was established by listening and
    // by feel, not by reading a channel mask -- so it is persisted rather than
    // retyped, and re-checked with --channel-test on any other controller.
    out << "{\n  \"device\": {"
        << "\"audioEndpointIndex\": " << c.device.audioEndpointIndex
        << ", \"speakerChannel\": " << c.device.speakerChannel
        << ", \"hapticLeftChannel\": " << c.device.hapticLeftChannel
        << ", \"hapticRightChannel\": " << c.device.hapticRightChannel
        << ", \"controllerSpeakerVolume\": " << c.device.controllerSpeakerVolume
        << ", \"audioOutputPath\": " << c.device.audioOutputPath
        << ", \"speakerPreGain\": " << c.device.speakerPreGain
        << ", \"windowsEndpointVolume\": " << c.device.windowsEndpointVolume
        << "},\n";
    profile("deflect", c.deflect, false);
    profile("block", c.block, false);
    out << "  \"speakerVolume\": " << c.speakerVolume << ",\n"
        << "  \"hapticStrength\": " << c.hapticStrength << ",\n"
        << "  \"retriggerDuck\": " << c.retriggerDuck << ",\n"
        << "  \"retireFadeMs\": " << c.retireFadeMs << ",\n"
        << "  \"limiterThreshold\": " << c.limiterThreshold << ",\n"
        << "  \"speakerHeadroom\": " << c.speakerHeadroom << ",\n"
        << "  \"hapticHeadroom\": " << c.hapticHeadroom << ",\n"
        << "  \"maxOutputLatencyUs\": " << c.maxOutputLatencyUs << ",\n"
        << "  \"voiceLimit\": " << c.voiceLimit << "\n}\n";
    return out.str();
}

} // namespace sekiro_haptics
