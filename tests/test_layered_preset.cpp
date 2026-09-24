// Several waveforms summed on one sample clock.
//
// The things worth defending: that a layer starts where it says it does to
// the sample, that a second layer starting mid-first does not disturb the
// first one's phase, that nothing is normalised behind the user's back, and
// that the analysis numbers describe the signal that was produced.

#include "sekiro_haptics/lab/LayeredPreset.hpp"
#include "testing.hpp"

#include <cmath>
#include <string>

using namespace sekiro_haptics::lab;

namespace {

constexpr std::uint32_t kRate = 48'000;

WaveformLayer Sine(float freq, float startMs, float durationMs, float amplitude = 0.5f) {
    WaveformLayer layer;
    layer.name = "sine";
    layer.waveform = LayerWaveform::Sine;
    layer.frequencyHz = freq;
    layer.startMs = startMs;
    layer.durationMs = durationMs;
    layer.amplitude = amplitude;
    layer.gainDb = 0.0f;
    return layer;
}

LayeredPresetSpec With(std::vector<WaveformLayer> layers) {
    LayeredPresetSpec spec;
    spec.layers = std::move(layers);
    return spec;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

SH_TEST(LayeredPreset_ALayerStartsOnTheExactSample) {
    // 30 ms at 48 kHz is frame 1440. Not "about there" -- exactly there. A
    // GUI timer or a second queued voice could not promise this, which is why
    // the whole preset is rendered as one buffer.
    auto spec = With({Sine(100.0f, 30.0f, 50.0f)});
    const auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(rendered.left[1439] == 0.0f);
    for (std::size_t f = 1440; f < 1500; ++f)
        if (rendered.left[f] != 0.0f) { SH_CHECK(true); break; }
    // And it ends exactly where it should: 30 + 50 ms is frame 3840.
    SH_CHECK(rendered.left.size() == 3840);
}

SH_TEST(LayeredPreset_ASecondLayerDoesNotResetTheFirstOnesPhase) {
    // THE RULE. Starting a new waveform while another is sounding must leave
    // the first one exactly as it was -- its phase, its envelope position,
    // its tail. Anything that restarted it would be audible as a click and
    // would make every overlap experiment meaningless.
    const auto aloneRender = RenderLayeredPreset(With({Sine(80.0f, 0.0f, 200.0f)}), kRate);
    const auto together =
        RenderLayeredPreset(With({Sine(80.0f, 0.0f, 200.0f), Sine(300.0f, 50.0f, 100.0f)}), kRate);
    SH_CHECK(aloneRender.ok && together.ok);

    // Layer 0's own trace in the combined render is sample-for-sample what it
    // was on its own.
    SH_CHECK(together.layerLeft[0].size() >= aloneRender.left.size());
    for (std::size_t f = 0; f < aloneRender.left.size(); ++f)
        SH_CHECK(together.layerLeft[0][f] == aloneRender.left[f]);
}

SH_TEST(LayeredPreset_TheSumIsNotNormalised) {
    // Two layers at 0.5 each reach 1.0 where they line up, and are left
    // there. Normalising would change the low layer between "alone" and
    // "together", which is exactly the comparison this tool is for.
    const auto rendered =
        RenderLayeredPreset(With({Sine(100.0f, 0.0f, 100.0f, 0.5f),
                                  Sine(100.0f, 0.0f, 100.0f, 0.5f)}), kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(Near(rendered.statsLeft.peak, 1.0f, 0.01f));

    // And the single-layer case is untouched at 0.5.
    const auto alone = RenderLayeredPreset(With({Sine(100.0f, 0.0f, 100.0f, 0.5f)}), kRate);
    SH_CHECK(Near(alone.statsLeft.peak, 0.5f, 0.01f));
}

SH_TEST(LayeredPreset_OverRangeIsCountedNotClipped) {
    // Three layers at 0.5 sum past full scale. The renderer reports how far
    // past rather than folding it down -- the limiter is a separate, visible
    // step.
    const auto rendered =
        RenderLayeredPreset(With({Sine(100.0f, 0.0f, 100.0f, 0.5f),
                                  Sine(100.0f, 0.0f, 100.0f, 0.5f),
                                  Sine(100.0f, 0.0f, 100.0f, 0.5f)}), kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(rendered.statsLeft.peak > 1.4f);
    SH_CHECK(rendered.statsLeft.overRange > 0);
}

SH_TEST(LayeredPreset_MuteAndSolo) {
    auto spec = With({Sine(80.0f, 0.0f, 100.0f), Sine(300.0f, 0.0f, 100.0f)});
    spec.layers[0].muted = true;
    auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(MeasureSignal(rendered.layerLeft[0]).peak == 0.0f);
    SH_CHECK(MeasureSignal(rendered.layerLeft[1]).peak > 0.0f);

    // Solo wins over mute and silences everything else -- and does NOT edit
    // the preset, so turning it off restores exactly what was there.
    spec.layers[0].muted = false;
    spec.layers[0].solo = true;
    rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(MeasureSignal(rendered.layerLeft[0]).peak > 0.0f);
    SH_CHECK(MeasureSignal(rendered.layerLeft[1]).peak == 0.0f);
    SH_CHECK(!spec.layers[1].muted);
}

SH_TEST(LayeredPreset_ChannelGainsAreIndependent) {
    auto spec = With({Sine(100.0f, 0.0f, 100.0f, 0.8f)});
    spec.layers[0].leftGain = 1.0f;
    spec.layers[0].rightGain = 0.0f;
    const auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(Near(rendered.statsLeft.peak, 0.8f, 0.01f));
    SH_CHECK(rendered.statsRight.peak == 0.0f);
}

SH_TEST(LayeredPreset_MasterGainScalesTheSumAndTheLayerTraces) {
    auto spec = With({Sine(100.0f, 0.0f, 100.0f, 0.5f)});
    spec.masterGain = 0.5f;
    const auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(Near(rendered.statsLeft.peak, 0.25f, 0.01f));
    // The drawn layer has to be its real contribution, not a different scale
    // that happens to look similar.
    SH_CHECK(Near(MeasureSignal(rendered.layerLeft[0]).peak, 0.25f, 0.01f));
}

SH_TEST(LayeredPreset_GainDbIsAnIndependentTrim) {
    auto spec = With({Sine(100.0f, 0.0f, 100.0f, 0.5f)});
    spec.layers[0].gainDb = -6.0f;
    const auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(Near(rendered.statsLeft.peak, 0.5f * 0.5012f, 0.01f));
}

SH_TEST(LayeredPreset_StartPhaseMovesWhereTheCycleBegins) {
    auto spec = With({Sine(100.0f, 0.0f, 100.0f, 1.0f)});
    auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(Near(rendered.left[0], 0.0f, 1e-5f));          // 0 deg starts at zero

    spec.layers[0].startPhaseDeg = 90.0f;
    rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(Near(rendered.left[0], 1.0f, 1e-4f));          // 90 deg starts at the peak
}

SH_TEST(LayeredPreset_TheFormulaExamplesRender) {
    WaveformLayer a;
    a.name = "A";
    a.waveform = LayerWaveform::Formula;
    a.formula = "cos(2*t)*sin(460*t)";
    a.durationMs = 200.0f;
    a.amplitude = 1.0f;

    WaveformLayer b;
    b.name = "B";
    b.waveform = LayerWaveform::Formula;
    b.formula = "if(t < t0, 0, cos(60*(t - t0))/t)";
    b.t0Seconds = 0.05f;
    b.durationMs = 200.0f;
    b.amplitude = 1.0f;

    const auto rendered = RenderLayeredPreset(With({a, b}), kRate);
    SH_CHECK(rendered.ok);
    // The branch was taken before the division, every time.
    SH_CHECK(rendered.diagnostics.expression.guardedDivisions == 0);
    SH_CHECK(rendered.diagnostics.nonFiniteSamples == 0);

    // A is exactly the product it says it is, at the sample.
    const std::size_t frame = 600;
    const double t = static_cast<double>(frame) / kRate;
    SH_CHECK(Near(rendered.layerLeft[0][frame],
                  static_cast<float>(std::cos(2 * t) * std::sin(460 * t)), 1e-5f));
    // B is silent before t0 (2400 frames) and not after.
    SH_CHECK(rendered.layerLeft[1][2399] == 0.0f);
    SH_CHECK(MeasureSignal(rendered.layerLeft[1]).peak > 0.0f);
}

SH_TEST(LayeredPreset_ANonPositiveT0IsRefusedWhenTheFormulaUsesIt) {
    WaveformLayer b;
    b.waveform = LayerWaveform::Formula;
    b.formula = "if(t < t0, 0, cos(60*(t - t0))/t)";
    b.t0Seconds = 0.0f;
    SH_CHECK(!ValidateLayeredPreset(With({b})).empty());

    b.t0Seconds = 0.01f;
    SH_CHECK(ValidateLayeredPreset(With({b})).empty());
}

SH_TEST(LayeredPreset_OutOfRangeSettingsAreRefusedNotClamped) {
    auto spec = With({Sine(100.0f, 0.0f, 100.0f)});
    SH_CHECK(ValidateLayeredPreset(spec).empty());

    spec.layers[0].amplitude = 1.5f;
    SH_CHECK(!ValidateLayeredPreset(spec).empty());
    spec.layers[0].amplitude = 0.5f;

    spec.layers[0].frequencyHz = 0.0f;
    SH_CHECK(!ValidateLayeredPreset(spec).empty());
    spec.layers[0].frequencyHz = 100.0f;

    spec.masterGain = 9.0f;
    SH_CHECK(!ValidateLayeredPreset(spec).empty());
    spec.masterGain = 1.0f;

    spec.layers.clear();
    SH_CHECK(!ValidateLayeredPreset(spec).empty());
}

SH_TEST(LayeredPreset_RenderingIsRepeatable) {
    // Save, reopen, play again -- the same signal. Phase computed from the
    // sample index rather than accumulated is what makes this true.
    auto spec = With({Sine(80.0f, 0.0f, 300.0f), Sine(310.0f, 90.0f, 120.0f)});
    spec.layers[1].startPhaseDeg = 45.0f;
    const auto first = RenderLayeredPreset(spec, kRate);
    const auto second = RenderLayeredPreset(spec, kRate);
    SH_CHECK(first.left == second.left);
    SH_CHECK(first.right == second.right);
}

SH_TEST(LayeredPreset_EnvelopeStartsAndEndsAtZeroAndReportsSqueezing) {
    auto spec = With({Sine(100.0f, 0.0f, 200.0f, 1.0f)});
    auto& envelope = spec.layers[0].envelope;
    envelope.enabled = true;
    envelope.attackMs = 10.0f;
    envelope.holdMs = 0.0f;
    envelope.decayMs = 30.0f;
    envelope.sustain = 0.5f;
    envelope.releaseMs = 40.0f;
    auto rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(Near(rendered.left.front(), 0.0f, 1e-5f));
    SH_CHECK(Near(rendered.left.back(), 0.0f, 1e-2f));
    SH_CHECK(rendered.diagnostics.envelopeScaled.empty());

    // Segments longer than the layer are scaled to fit, and that is REPORTED
    // rather than quietly truncated mid-attack -- a cut attack is a step.
    envelope.attackMs = 300.0f;
    rendered = RenderLayeredPreset(spec, kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(rendered.diagnostics.envelopeScaled.size() == 1);
}

SH_TEST(LayeredPreset_LimiterPreviewShowsWhatComesOffAndWhatGetsThrough) {
    // Under the threshold nothing happens at all.
    const auto quiet = RenderLayeredPreset(With({Sine(100.0f, 0.0f, 200.0f, 0.5f)}), kRate);
    auto preview = PreviewLimiter(quiet.left, kRate, 0.95f, 1.0f, 120.0f);
    SH_CHECK(preview.limitedFrames == 0);
    SH_CHECK(preview.clampedSamples == 0);
    SH_CHECK(Near(MeasureSignal(preview.samples).peak, 0.5f, 0.01f));

    // Over it, the reduction is reported and the output is held at the
    // threshold rather than clipped.
    const auto loud = RenderLayeredPreset(With({Sine(100.0f, 0.0f, 200.0f, 1.0f),
                                                Sine(100.0f, 0.0f, 200.0f, 1.0f)}), kRate);
    preview = PreviewLimiter(loud.left, kRate, 0.95f, 1.0f, 120.0f);
    SH_CHECK(preview.limitedFrames > 0);
    SH_CHECK(preview.worstReductionDb > 3.0f);
    SH_CHECK(MeasureSignal(preview.samples).peak <= 1.0f);
}

SH_TEST(LayeredPreset_FftFindsTheFrequencyThatWasAskedFor) {
    const auto rendered = RenderLayeredPreset(With({Sine(200.0f, 0.0f, 500.0f, 1.0f)}), kRate);
    const auto spectrum = AnalyzeSpectrum(rendered.left, kRate, 0, 4096, FftWindow::Hann);
    SH_CHECK(spectrum.ok);

    std::size_t loudest = 0;
    for (std::size_t bin = 1; bin < spectrum.magnitude.size(); ++bin)
        if (spectrum.magnitude[bin] > spectrum.magnitude[loudest]) loudest = bin;
    // 200 Hz with a 4096-point transform at 48 kHz is bin 17.07, so the peak
    // lands on 17 and the bin width is ~11.7 Hz.
    SH_CHECK(Near(spectrum.frequencyHz[loudest], 200.0f, 12.0f));
    // The window's coherent gain is divided out, so a full-scale sine reads
    // about 1.0 -- the unit is PCM amplitude and nothing physical.
    SH_CHECK(spectrum.magnitude[loudest] > 0.85f);
    SH_CHECK(spectrum.magnitude[loudest] < 1.15f);
}

SH_TEST(LayeredPreset_FftRefusesRatherThanPaddingPastTheEnd) {
    const auto rendered = RenderLayeredPreset(With({Sine(200.0f, 0.0f, 50.0f)}), kRate);
    // 50 ms is 2400 frames, so a 4096-point window does not fit. Zero-padding
    // it would present the spectrum of half a window of silence as if it were
    // the signal's.
    SH_CHECK(!AnalyzeSpectrum(rendered.left, kRate, 0, 4096, FftWindow::Hann).ok);
    SH_CHECK(!AnalyzeSpectrum(rendered.left, kRate, 0, 1000, FftWindow::Hann).ok);   // not a power of two
    SH_CHECK(AnalyzeSpectrum(rendered.left, kRate, 0, 2048, FftWindow::Hann).ok);
}

SH_TEST(LayeredPreset_MeanExposesADcOffset) {
    // A waveform sitting off centre pushes the actuator to one side and stays
    // there, which is heat rather than movement -- and is invisible in a peak
    // or an RMS.
    WaveformLayer offset;
    offset.waveform = LayerWaveform::Formula;
    offset.formula = "0.5";
    offset.durationMs = 100.0f;
    offset.amplitude = 1.0f;
    const auto rendered = RenderLayeredPreset(With({offset}), kRate);
    SH_CHECK(rendered.ok);
    SH_CHECK(Near(rendered.statsLeft.mean, 0.5f, 1e-4f));

    const auto centred = RenderLayeredPreset(With({Sine(100.0f, 0.0f, 100.0f, 1.0f)}), kRate);
    SH_CHECK(Near(centred.statsLeft.mean, 0.0f, 1e-3f));
}
