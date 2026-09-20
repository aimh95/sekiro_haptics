// Unit tests for the deflect/block collision cues.
//
// These check the SHAPE of the synthesised signals -- that the two profiles are
// actually different in the ways that were asked for, that neither clips, and
// that config round-trips. They cannot tell you how anything FEELS: that needs
// the controller in your hands (see docs/11-guard-feedback.md).

#include "sekiro_haptics/GuardCue.hpp"
#include "testing.hpp"

#include <cmath>
#include <numeric>

using namespace sekiro_haptics;

namespace {

constexpr std::uint32_t kRate = 48'000;

float Rms(const std::vector<float>& c) {
    if (c.empty()) return 0.0f;
    double sum = 0;
    for (float s : c) sum += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(sum / static_cast<double>(c.size())));
}

/// Index of the first sample at or above `fraction` of the clip's peak --
/// a crude but sufficient measure of how fast the attack is.
std::size_t RiseIndex(const std::vector<float>& c, float fraction) {
    const float target = PeakAmplitude(c) * fraction;
    for (std::size_t i = 0; i < c.size(); ++i)
        if (std::fabs(c[i]) >= target) return i;
    return c.size();
}

/// Energy above `hz`, estimated by one-pole high-pass differencing.
float HighFrequencyEnergy(const std::vector<float>& c) {
    if (c.size() < 2) return 0.0f;
    double sum = 0;
    for (std::size_t i = 1; i < c.size(); ++i) {
        const double d = static_cast<double>(c[i]) - c[i - 1];
        sum += d * d;
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(c.size() - 1)));
}

} // namespace

SH_TEST(GuardCue_BothProfilesRenderNonEmptyAndDoNotClip) {
    const auto c = DefaultGuardCueConfig();
    for (const auto* p : {&c.deflect, &c.block}) {
        const auto speaker = SynthesizeGuardSpeaker(p->speaker, kRate, c.speakerVolume);
        const auto haptic = SynthesizeGuardHaptic(p->haptic, kRate, c.hapticStrength);
        SH_CHECK(!speaker.empty());
        SH_CHECK(!haptic.empty());
        SH_CHECK(PeakAmplitude(speaker) > 0.05f);
        SH_CHECK(PeakAmplitude(haptic) > 0.05f);
        SH_CHECK(PeakAmplitude(speaker) < 1.0f);   // headroom, never railed
        SH_CHECK(PeakAmplitude(haptic) < 1.0f);
    }
}

SH_TEST(GuardCue_DeflectSpeakerIsBrighterThanBlock) {
    // The difference must be in timbre, not merely loudness.
    const auto c = DefaultGuardCueConfig();
    const auto deflect = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    const auto block = SynthesizeGuardSpeaker(c.block.speaker, kRate, 1.0f);
    SH_CHECK(HighFrequencyEnergy(deflect) > HighFrequencyEnergy(block) * 1.3f);
}

SH_TEST(GuardCue_TheTwoProfilesAreNotDistinguishedByLoudnessAlone) {
    // RMS must stay within a factor of two, so "different" cannot be achieved
    // by simply making one louder.
    const auto c = DefaultGuardCueConfig();
    const auto deflect = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    const auto block = SynthesizeGuardSpeaker(c.block.speaker, kRate, 1.0f);
    const float ratio = Rms(deflect) / Rms(block);
    SH_CHECK(ratio > 0.5f && ratio < 2.0f);
}

SH_TEST(GuardCue_DeflectHapticRisesFasterThanBlockHaptic) {
    // The sharp contact layer is what makes a deflect read as a precise touch.
    const auto c = DefaultGuardCueConfig();
    const auto deflect = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    const auto block = SynthesizeGuardHaptic(c.block.haptic, kRate, 1.0f);
    SH_CHECK(RiseIndex(deflect, 0.5f) < RiseIndex(block, 0.5f));
}

SH_TEST(GuardCue_BothHapticsAreNormalisedToTheSameDigitalPeak) {
    // Equal digital peak is deliberate: the two must differ in shape, not in
    // level. It does NOT mean they feel equally strong -- that needs the pad.
    const auto c = DefaultGuardCueConfig();
    const auto deflect = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    const auto block = SynthesizeGuardHaptic(c.block.haptic, kRate, 1.0f);
    SH_CHECK(std::fabs(PeakAmplitude(deflect) - c.deflect.haptic.normalizePeak) < 0.01f);
    SH_CHECK(std::fabs(PeakAmplitude(block) - c.block.haptic.normalizePeak) < 0.01f);
}

SH_TEST(GuardCue_HapticEndsAtSilenceSoTheActuatorIsNeverLeftHeld) {
    const auto c = DefaultGuardCueConfig();
    for (const auto* h : {&c.deflect.haptic, &c.block.haptic}) {
        const auto clip = SynthesizeGuardHaptic(*h, kRate, 1.0f);
        SH_CHECK(!clip.empty());
        SH_CHECK(std::fabs(clip.back()) < 0.01f);
    }
}

SH_TEST(GuardCue_DeflectBodySitsHigherThanBlockBody) {
    // The deflect body sits above the block body -- that separation is the
    // main reason the two feel different. The margin used to be 1.5x (205 vs
    // 115 Hz), but the tuned presets are 190 vs 145 Hz: the gap narrowed on
    // purpose when the bodies were reshaped, so the test pins the invariant
    // that actually matters (clearly higher) rather than the old ratio.
    const auto c = DefaultGuardCueConfig();
    SH_CHECK(c.deflect.haptic.body.startHz > c.block.haptic.body.startHz * 1.25f);
    SH_CHECK(c.block.haptic.totalMs > c.deflect.haptic.totalMs);
}

SH_TEST(GuardCue_ContactLayerGlidesWithoutADiscontinuity) {
    // The contact layer sweeps 260 -> 220 Hz; a naive per-sample frequency
    // change would click.
    //
    // The bound is RELATIVE to the cue's peak. It used to be the absolute
    // 0.10, which only held while normalizePeak was 0.30 -- raising the cue's
    // strength scaled every step with it and tripped the check without
    // anything having gone wrong.
    //
    // The noise grain is switched off for the glide measurement on purpose.
    // White noise steps by up to twice its own amplitude between samples, so
    // leaving it in measures the grain, not the sweep this test is named for.
    auto c = DefaultGuardCueConfig();
    auto profile = c.deflect.haptic;
    profile.noiseAmplitude = 0.0f;
    const auto clip = SynthesizeGuardHaptic(profile, kRate, 1.0f);
    float peak = 0.0f, worst = 0.0f;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        peak = std::max(peak, std::fabs(clip[i]));
        if (i) worst = std::max(worst, std::fabs(clip[i] - clip[i - 1]));
    }
    // A 260 Hz sine at 48 kHz steps by at most peak * 2*pi*260/48000 = 0.034
    // of its peak per sample. 0.06 leaves room for the summed layers without
    // being loose enough to miss an actual step.
    SH_CHECK(peak > 0.0f);
    SH_CHECK(worst < peak * 0.06f);

    // With the grain back in, the whole cue must still be free of anything
    // that looks like a cut rather than texture.
    const auto grained = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    float grainWorst = 0.0f, grainPeak = 0.0f;
    for (std::size_t i = 0; i < grained.size(); ++i) {
        grainPeak = std::max(grainPeak, std::fabs(grained[i]));
        if (i) grainWorst = std::max(grainWorst, std::fabs(grained[i] - grained[i - 1]));
    }
    SH_CHECK(grainWorst < grainPeak * 0.25f);
}

SH_TEST(GuardCue_BlockHapticLastsLongerThanDeflectHaptic) {
    const auto c = DefaultGuardCueConfig();
    const auto deflect = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    const auto block = SynthesizeGuardHaptic(c.block.haptic, kRate, 1.0f);
    SH_CHECK(block.size() > deflect.size());
}

SH_TEST(GuardCue_HapticStaysLowFrequencyUnlikeTheSpeakerClip) {
    // The haptic clip must NOT be a copy of the kHz clang: its sample-to-sample
    // change is far smaller because its content is an order of magnitude lower.
    const auto c = DefaultGuardCueConfig();
    const auto speaker = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    const auto haptic = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    SH_CHECK(HighFrequencyEnergy(haptic) < HighFrequencyEnergy(speaker));
}

SH_TEST(GuardCue_SynthesisIsDeterministic) {
    const auto c = DefaultGuardCueConfig();
    const auto a = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    const auto b = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    SH_CHECK(a == b);
    const auto h1 = SynthesizeGuardHaptic(c.block.haptic, kRate, 1.0f);
    const auto h2 = SynthesizeGuardHaptic(c.block.haptic, kRate, 1.0f);
    SH_CHECK(h1 == h2);
}

SH_TEST(GuardCue_MasterScalesReduceAmplitudeIndependently) {
    const auto c = DefaultGuardCueConfig();
    const auto loud = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    const auto quiet = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 0.25f);
    SH_CHECK(PeakAmplitude(quiet) < PeakAmplitude(loud));
    const auto strong = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f);
    const auto weak = SynthesizeGuardHaptic(c.deflect.haptic, kRate, 0.25f);
    SH_CHECK(PeakAmplitude(weak) < PeakAmplitude(strong));
    // Turning the speaker down must not touch the haptic clip.
    SH_CHECK(SynthesizeGuardHaptic(c.deflect.haptic, kRate, 1.0f) == strong);
}

SH_TEST(GuardCue_ConfigRoundTripsThroughJson) {
    auto original = DefaultGuardCueConfig();
    original.deflect.speaker.baseHz = 1999.0f;
    original.block.haptic.body.startHz = 97.0f;
    original.retriggerDuck = 0.3f;
    const std::string text = SerializeGuardCueConfig(original);
    GuardCueConfig parsed;
    std::string error;
    SH_CHECK(ParseGuardCueConfig(text, parsed, error));
    SH_CHECK(error.empty());
    SH_CHECK(std::fabs(parsed.deflect.speaker.baseHz - 1999.0f) < 0.5f);
    SH_CHECK(std::fabs(parsed.block.haptic.body.startHz - 97.0f) < 0.5f);
    SH_CHECK(std::fabs(parsed.retriggerDuck - 0.3f) < 0.01f);
    SH_CHECK(parsed.deflect.speaker.partials.size() == original.deflect.speaker.partials.size());
}

SH_TEST(GuardCue_BadConfigLeavesTheExistingOneUntouched) {
    GuardCueConfig kept = DefaultGuardCueConfig();
    kept.deflect.speaker.baseHz = 1234.0f;
    std::string error;
    SH_CHECK(!ParseGuardCueConfig("{ not json", kept, error));
    SH_CHECK(!error.empty());
    SH_CHECK(std::fabs(kept.deflect.speaker.baseHz - 1234.0f) < 0.5f);
}

SH_TEST(GuardCue_PartialsAboveNyquistAreSkippedNotAliased) {
    SpeakerCueProfile p;
    p.baseHz = 20'000.0f;
    p.partials = {{1.0f, 0.5f, 10.0f}, {3.0f, 0.5f, 10.0f}};  // 60 kHz would alias
    p.strikeNoiseAmplitude = 0.0f;
    const auto clip = SynthesizeGuardSpeaker(p, kRate, 1.0f);
    SH_CHECK(!clip.empty());
    SH_CHECK(PeakAmplitude(clip) < 1.0f);
}

SH_TEST(GuardCue_UnsupportedSampleRateReturnsEmptyRatherThanGarbage) {
    const auto c = DefaultGuardCueConfig();
    SH_CHECK(SynthesizeGuardSpeaker(c.deflect.speaker, 1'000, 1.0f).empty());
    SH_CHECK(SynthesizeGuardHaptic(c.deflect.haptic, 1'000'000, 1.0f).empty());
}

SH_TEST(GuardCue_ExtendDecayTailAddsARingWithoutASecondAttack) {
    // The supplied recordings have another sword strike ~66-70 ms after the
    // one being used, so a single-impact slice can only be ~66 ms and sounds
    // cut off. ExtendDecayTail rebuilds the ring from the slice's own late
    // material. The property that matters is that it adds NO new attack: the
    // envelope after the original clip must only ever fall.
    auto c = DefaultGuardCueConfig();
    auto dry = SynthesizeGuardSpeaker(c.deflect.speaker, kRate, 1.0f);
    SH_CHECK(!dry.empty());
    auto wet = dry;
    ExtendDecayTail(wet, kRate, 240.0f, 13.0f, 18.0f);

    // It grew, and the original part is untouched.
    SH_CHECK(wet.size() > dry.size());
    for (std::size_t i = 0; i < dry.size(); ++i) SH_CHECK(wet[i] == dry[i]);

    // Coarse envelope over the appended region, 10 ms per bucket. Each bucket
    // must be no louder than the one before it -- a rise would be a new hit.
    const std::size_t bucket = kRate / 100;
    float previous = 0.0f;
    bool first = true;
    std::size_t buckets = 0;
    for (std::size_t start = dry.size(); start + bucket <= wet.size(); start += bucket) {
        float peak = 0.0f;
        for (std::size_t i = start; i < start + bucket; ++i) peak = std::max(peak, std::fabs(wet[i]));
        if (!first) SH_CHECK(peak <= previous * 1.15f + 1e-4f);   // 15% slack for grain overlap
        previous = peak;
        first = false;
        ++buckets;
    }
    SH_CHECK(buckets >= 20);            // ~240 ms of tail actually present
    SH_CHECK(previous < 0.02f);         // and it has died away by the end

    // Ends at silence, so a cue never leaves a step on the speaker.
    SH_CHECK(std::fabs(wet.back()) < 0.005f);

    // Off by default unless a profile asks for it.
    auto untouched = dry;
    ExtendDecayTail(untouched, kRate, 0.0f, 13.0f, 18.0f);
    SH_CHECK(untouched.size() == dry.size());
}
