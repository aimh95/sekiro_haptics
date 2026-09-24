// The sine burst behind the game-free haptic bench.
//
// Each test names the thing it would catch. The point of the bench is that a
// number on screen is the number that was played, so the generator is the one
// place where a silent correction -- a clamp, a normalisation, a fade that
// eats the amplitude -- would invalidate every comparison made with it.

#include "sekiro_haptics/lab/ToneBurst.hpp"
#include "testing.hpp"

#include <algorithm>
#include <cmath>
#include <string>

using namespace sekiro_haptics::lab;

namespace {

constexpr std::uint32_t kRate = 48'000;

ToneBurstSpec Basic() {
    ToneBurstSpec spec;
    spec.frequencyHz = 100.0f;
    spec.amplitude = 0.5f;
    spec.lengthMs = 200.0f;
    spec.fadeInMs = 0.0f;
    spec.fadeOutMs = 0.0f;
    spec.leftGain = 1.0f;
    spec.rightGain = 1.0f;
    return spec;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

SH_TEST(ToneBurst_LengthAndFrequencyAreWhatWasAskedFor) {
    auto spec = Basic();
    std::vector<float> left, right;
    RenderToneBurst(spec, kRate, left, right);

    SH_CHECK(left.size() == 9600);              // 200 ms at 48 kHz
    SH_CHECK(right.size() == left.size());

    // Count zero crossings: 100 Hz over 200 ms is 20 cycles, so 40 crossings
    // give or take the endpoints. This is what catches a factor-of-two error
    // in the phase step, which a peak check would not see.
    int crossings = 0;
    for (std::size_t i = 1; i < left.size(); ++i)
        if ((left[i - 1] < 0.0f) != (left[i] < 0.0f)) ++crossings;
    SH_CHECK(crossings >= 39 && crossings <= 41);
}

SH_TEST(ToneBurst_SwingsBothWays) {
    // A voice coil driven from PCM moves in both directions. A rumble motor's
    // single-sided magnitude does not, and a generator that accidentally
    // rectified or offset the wave would still "work" and feel wrong.
    std::vector<float> left, right;
    RenderToneBurst(Basic(), kRate, left, right);

    const float lowest = *std::min_element(left.begin(), left.end());
    const float highest = *std::max_element(left.begin(), left.end());
    SH_CHECK(lowest < -0.49f);
    SH_CHECK(highest > 0.49f);
    // And it is centred: a DC offset would push the coil to one side and sit
    // there, which is heat rather than vibration.
    double sum = 0.0;
    for (float v : left) sum += v;
    SH_CHECK(std::fabs(sum / static_cast<double>(left.size())) < 0.001);
}

SH_TEST(ToneBurst_AmplitudeIsNotNormalisedOrClamped) {
    // THE RULE. The bench compares levels, so the amplitude asked for is the
    // peak produced -- no normalisation to full scale, no quiet boost.
    for (float amplitude : {0.1f, 0.25f, 0.8f, 1.0f}) {
        auto spec = Basic();
        spec.amplitude = amplitude;
        std::vector<float> left, right;
        RenderToneBurst(spec, kRate, left, right);
        const auto stats = MeasureToneBurst(left, right);
        SH_CHECK(Near(stats.peakLeft, amplitude, 0.002f));
    }
}

SH_TEST(ToneBurst_ChannelGainsAreIndependent) {
    auto spec = Basic();
    spec.leftGain = 1.0f;
    spec.rightGain = 0.25f;
    std::vector<float> left, right;
    RenderToneBurst(spec, kRate, left, right);
    const auto stats = MeasureToneBurst(left, right);

    SH_CHECK(Near(stats.peakLeft, 0.5f, 0.002f));
    SH_CHECK(Near(stats.peakRight, 0.125f, 0.002f));

    // Driving one side alone must leave the other actually silent, not merely
    // quieter -- that is the test for "which palm is this?".
    spec.rightGain = 0.0f;
    RenderToneBurst(spec, kRate, left, right);
    SH_CHECK(MeasureToneBurst(left, right).peakRight == 0.0f);
}

SH_TEST(ToneBurst_FadesStartAndEndAtZero) {
    auto spec = Basic();
    spec.fadeInMs = 10.0f;
    spec.fadeOutMs = 20.0f;
    std::vector<float> left, right;
    RenderToneBurst(spec, kRate, left, right);

    SH_CHECK(std::fabs(left.front()) < 1e-6f);
    SH_CHECK(std::fabs(left.back()) < 1e-3f);
    // The body still reaches the amplitude: a fade that ate into the middle
    // would quietly make every measurement lower than the number on screen.
    SH_CHECK(Near(MeasureToneBurst(left, right).peakLeft, 0.5f, 0.01f));
}

SH_TEST(ToneBurst_OverlappingFadesAreRejectedRatherThanClamped) {
    // Two fades longer than the burst would multiply each other and the burst
    // would never reach its amplitude. Refusing says so; clamping would not.
    auto spec = Basic();
    spec.lengthMs = 50.0f;
    spec.fadeInMs = 30.0f;
    spec.fadeOutMs = 30.0f;
    SH_CHECK(!ValidateToneBurst(spec).empty());

    std::vector<float> left, right;
    RenderToneBurst(spec, kRate, left, right);
    SH_CHECK(left.empty());
    SH_CHECK(right.empty());
}

SH_TEST(ToneBurst_OutOfRangeValuesAreRefused) {
    auto spec = Basic();
    spec.frequencyHz = 19.0f;
    SH_CHECK(!ValidateToneBurst(spec).empty());
    spec.frequencyHz = 501.0f;
    SH_CHECK(!ValidateToneBurst(spec).empty());

    spec = Basic();
    spec.amplitude = 1.5f;      // no clamp to 1.0: it would report a level never played
    SH_CHECK(!ValidateToneBurst(spec).empty());

    spec = Basic();
    spec.lengthMs = 10.0f;
    SH_CHECK(!ValidateToneBurst(spec).empty());
    spec.lengthMs = 1001.0f;
    SH_CHECK(!ValidateToneBurst(spec).empty());

    spec = Basic();
    spec.rightGain = -0.1f;
    SH_CHECK(!ValidateToneBurst(spec).empty());

    // And the edges of each range ARE allowed.
    spec = Basic();
    spec.frequencyHz = kMinFrequencyHz;
    SH_CHECK(ValidateToneBurst(spec).empty());
    spec.frequencyHz = kMaxFrequencyHz;
    spec.lengthMs = kMaxLengthMs;
    spec.amplitude = 1.0f;
    SH_CHECK(ValidateToneBurst(spec).empty());
}

SH_TEST(ToneBurst_RenderingIsRepeatable) {
    // An exported WAV has to be the waveform that was felt. Phase computed
    // from an accumulator drifts; computed from the sample index it does not.
    std::vector<float> firstLeft, firstRight, secondLeft, secondRight;
    auto spec = Basic();
    spec.lengthMs = 1000.0f;
    RenderToneBurst(spec, kRate, firstLeft, firstRight);
    RenderToneBurst(spec, kRate, secondLeft, secondRight);
    SH_CHECK(firstLeft == secondLeft);
    SH_CHECK(firstRight == secondRight);
}

SH_TEST(ToneBurst_RmsOfAFullSineIsTheExpectedFraction) {
    // A sine's RMS is its peak over root two. This is the check that the
    // measurement the GUI prints is the ordinary one and not something with a
    // window or a weighting hidden in it.
    auto spec = Basic();
    spec.amplitude = 1.0f;
    std::vector<float> left, right;
    RenderToneBurst(spec, kRate, left, right);
    SH_CHECK(Near(MeasureToneBurst(left, right).rmsLeft, 0.7071f, 0.005f));
}

SH_TEST(ToneBurst_InterleaveKeepsTheSidesInOrder) {
    std::vector<float> left{1.0f, 3.0f, 5.0f};
    std::vector<float> right{2.0f, 4.0f, 6.0f};
    const auto interleaved = InterleaveStereo(left, right);
    SH_CHECK(interleaved.size() == 6);
    for (std::size_t i = 0; i < 6; ++i) SH_CHECK(interleaved[i] == static_cast<float>(i + 1));
}
