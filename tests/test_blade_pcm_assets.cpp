// The authored blade waveforms, decoded the way the app decodes them.
//
// WHY THESE READ THE REAL FILES
// -----------------------------
// The thing that can go wrong here is not arithmetic, it is a de-interleave
// that reads the wrong stride, swaps the two sides, or quietly hands back a
// downmix. None of those show up against a synthetic buffer this file made
// itself -- they show up against a file whose two channels were measured by
// whoever authored it. So the numbers below come from the pack's own
// validation.json, and a test failing here means the decode no longer agrees
// with the asset, which is exactly the question worth asking.
//
// Windows-only: the loader is Media Foundation.

#include "sekiro_haptics/AudioClipLoader.hpp"
#include "testing.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace sekiro_haptics;

namespace {

constexpr std::uint32_t kRate = 48'000;   // the pack's own rate: no resampling

std::filesystem::path Wav(const char* name) {
    return std::filesystem::path(SH_BLADE_PCM_DIR) / name;
}

float Peak(const std::vector<float>& v) {
    float peak = 0.0f;
    for (float s : v) peak = std::max(peak, std::fabs(s));
    return peak;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

SH_TEST(BladePcm_DeflectDecodesToTheFramesAndPeaksTheAssetDeclares) {
    std::vector<float> left, right;
    AudioClipInfo info;
    LoadAudioClipStereo(Wav("blade_deflect.wav"), kRate, left, right, info);

    SH_CHECK(info.ok);
    SH_CHECK(info.error.empty());
    SH_CHECK(info.sourceChannels == 2);
    SH_CHECK(info.sourceSampleRate == kRate);
    // frames, not samples: 5760 frames is 120 ms at 48 kHz.
    SH_CHECK(info.frames == 5760);
    SH_CHECK(left.size() == 5760);
    SH_CHECK(right.size() == 5760);

    // validation.json: peakLR [0.6771, 0.7128]. A swapped or mis-strided
    // de-interleave would put the wrong one of these on each side.
    SH_CHECK(Near(Peak(left), 0.6771f, 0.002f));
    SH_CHECK(Near(Peak(right), 0.7128f, 0.002f));
}

SH_TEST(BladePcm_BlockDecodesToTheFramesAndPeaksTheAssetDeclares) {
    std::vector<float> left, right;
    AudioClipInfo info;
    LoadAudioClipStereo(Wav("blade_block.wav"), kRate, left, right, info);

    SH_CHECK(info.ok);
    SH_CHECK(info.frames == 7920);          // 165 ms
    SH_CHECK(left.size() == 7920);
    SH_CHECK(right.size() == 7920);
    SH_CHECK(Near(Peak(left), 0.4750f, 0.002f));
    SH_CHECK(Near(Peak(right), 0.5000f, 0.002f));
}

SH_TEST(BladePcm_TheTwoSidesAreNotTheSameWaveform) {
    // The whole reason for a stereo path. If these two ever came back
    // identical, something upstream downmixed and the pair is being thrown
    // away -- which is silent, and would otherwise only be noticed by hand.
    std::vector<float> left, right;
    AudioClipInfo info;
    LoadAudioClipStereo(Wav("blade_deflect.wav"), kRate, left, right, info);
    SH_CHECK(info.ok);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i] != right[i]) ++differing;
    SH_CHECK(differing > left.size() / 2);
}

SH_TEST(BladePcm_AMissingFileFailsAndLeavesBothSidesEmpty) {
    std::vector<float> left{1.0f, 2.0f}, right{3.0f};
    AudioClipInfo info;
    LoadAudioClipStereo(Wav("no_such_clip.wav"), kRate, left, right, info);

    SH_CHECK(!info.ok);
    SH_CHECK(!info.error.empty());
    // Cleared, not left holding the caller's previous contents: the app
    // decides to fall back on `ok`, and half-filled vectors behind a false
    // would be registered as a cue.
    SH_CHECK(left.empty());
    SH_CHECK(right.empty());
}

SH_TEST(BladePcm_TheMonoLoaderStillWorksOnTheSameFile) {
    // The mono path was refactored to share the stereo one's decode. It is
    // what every speaker recording still goes through, so it is checked on a
    // file whose frame count is known independently.
    AudioClipInfo info;
    const auto mono = LoadAudioClipMono(Wav("blade_deflect.wav"), kRate, info);

    SH_CHECK(info.ok);
    SH_CHECK(info.sourceChannels == 2);     // what the FILE is
    SH_CHECK(info.frames == 5760);          // what came back, downmixed
    SH_CHECK(mono.size() == 5760);
}
