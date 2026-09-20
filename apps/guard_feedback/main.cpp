// Deflect / block collision feedback on a real DualSense: built-in speaker
// clang plus a separately-designed PCM haptic impact.
//
// Scope: identify the controller's audio endpoint and its channels, render the
// two cues, and let a human compare them side by side. Channel indices are
// NEVER assumed -- run `--channel-test` and listen/feel which one is which.

#include "sekiro_haptics/AudioClipLoader.hpp"
#include "sekiro_haptics/DualSenseAudioDevice.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "sekiro_haptics/GuardCue.hpp"
#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/GuardOutcomeEventDetector.hpp"
#include "sekiro_haptics/process/SekiroPlayerGuardReader.hpp"
#include "sekiro_haptics/process/SekiroEnemyGuardReader.hpp"
#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"

#include <windows.h>
#include <hidsdi.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sekiro_haptics;

namespace {

std::int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void Help() {
    std::cout <<
R"(guard_feedback -- deflect/block collision feedback on a real DualSense

DEVICE SETUP (do this first, once)
  --list-devices                    HID controllers and audio render endpoints
  --probe-format   --audio N        GetMixFormat + IsFormatSupported for endpoint N
  --channel-test   --audio N        play a burst on EVERY channel, one at a time,
                                    announcing each -- you identify which is the
                                    speaker and which buzzes in your palms
  --channel-test   --audio N --channel K   only channel K

COMPARING THE TWO FEELS
  --play deflect|block|alternate|repeat|mixed
  --count N            how many hits (default 3)
  --interval-ms N      spacing; try 500, 200, 100 (default 400)
  --mode speaker|haptic|both        default both
  --audio N            required for any output
  --speaker-channel K  --haptic-left K  --haptic-right K
  --volume F  --strength F          master scales, 0..1
  --speaker-volume N                controller speaker level byte 0..255 (default 255)
  --endpoint-volume F               ALSO set Windows' level for THIS endpoint, 0..1
                                    (explicit opt-in; the default device is untouched)
  --probe-gain F                    channel-test tone level, 0..1
  --probe-hz F                      one explicit probe frequency instead of the
                                    880/150 pair (use ~60-70 to separate FELT
                                    vibration from HEARD sound)
  --probe-seconds F                 how long each probe lasts
  --audio-path N                    DualSense output-path selector 0..3 (default 3).
                                    Which value reaches the built-in speaker is not
                                    assumed -- sweep it and listen.
  --hid-info            what the controller's HID descriptor actually declares:
                        output report length, feature/input lengths, and a write
                        attempt at each plausible length
  --report-length N     force the output report length used everywhere else
  --speaker-diag --audio N --speaker-channel K
                        full volume audit + HID setting sweep with the haptic
                        channels held at zero, so only the speaker can be heard
      --pre-gain N       audio_flags2 speaker pre-gain 0..7
      --valid0 N --valid1 N   override the apply-flag bytes
      --dump-submit WAV  write what was actually submitted to WASAPI
  --path-sweep --audio N --channel K   play a tone on channel K once per output-path
                                    value, announcing each, to find the built-in speaker
  --config PATH        guard cue profiles (default config/guard_cues.json)
  --no-speaker-routing do not send the HID report that routes audio to the
                       built-in speaker (use when testing via headphones)

LIVE -- drive the cues from the running game
  --live --pid N       attach read-only to sekiro.exe and play a cue on every
                       guard resolution it detects (needs the same --audio and
                       channel flags as --play)
  --live-seconds N     stop after N seconds (default: until Ctrl+C)

SUPPLIED RECORDINGS
  --inspect-audio PATH  decode a file (or every file in a folder) and report
                        rate/channels/duration/peak plus a rough band balance
  --decode-to DIR       also write each decoded clip as 48k mono WAV
  --extract-hit PATH --out WAV   slice ONE impact out of a multi-hit recording
      --onset N          which impact (0 = first)
      --hit-length-ms M  how much to keep (default 170)
      --fade-ms M        fade-out so the cut cannot click (default 30)
      --extract-peak F   peak-normalise the slice (default 0.92)
      --align-only       do NOT slice: remove only the leading silence and
                         normalise, keeping the whole tail. Use this first to
                         check the playback path before editing anything.
      --align-peak-db F  headroom for --align-only (default -3 dBFS)

INSPECTING WAVEFORMS WITHOUT HARDWARE
  --write-config PATH  write the effective config out in the current schema
  --enemy              also cue when an ENEMY blocks/deflects. The signal says
                       "this character guarded", NOT "it guarded my attack"
  --enemy-rediscover N seconds between rebuilds of the tracked enemy set (6)
  --overlap-test       single shot, then 400/200/100 ms repeats and a mixed run;
                       measures the submitted mix for cut-short cues and gaps
  --dump-mix DIR       write each overlap-test phase's submitted mix as a WAV
  --audio-diag         speaker-only / haptic-only / together / fast repeats, with
                       feed gap, buffer padding and voice end reasons per phase
  --haptic-variant X   a = shipped preset, b = reinforced body at the same peak,
                       c = b at peak 0.45 (a DIGITAL amplitude, not "45% force")
  --speaker-trim-db N  level trim on the SPEAKER clip only (try -6 and -12)
  --speaker-highpass N / --speaker-lowpass N   speaker-only EQ, 0 = off
  --audio-buffer-ms N  shared-mode buffer: dropout margin AND added latency
  --duck-fade-ms N     how long a previous tail takes to fade when retriggered
  --dump-cues          print duration/peak/RMS of both speaker and haptic clips
  --write-wav DIR      write deflect/block speaker+haptic clips as 48k mono WAV

Nothing here is a game detector. Manual playback says nothing about in-game
detection accuracy.
)";
}

struct Options {
    bool list = false, probe = false, channelTest = false, dumpCues = false;
    bool speakerRouting = true;
    int audio = -1, channel = -1;
    int speakerChannel = -1, hapticLeft = -1, hapticRight = -1;
    int count = 3, intervalMs = 400;
    std::string pattern, mode = "both", config = "config/guard_cues.json", writeWav;
    float volume = 1.0f, strength = 1.0f;
    bool live = false;
    int pid = 0, liveSeconds = 0;
    int speakerVolume = 100;    // built-in speaker level byte (NOT 0..255)
    bool speakerVolumeGiven = false, audioPathGiven = false, preGainGiven = false;
    float probeGain = 1.0f;
    float endpointVolume = -1.0f;  // <0 = leave Windows' level alone
    std::string inspectAudio, decodeTo, extractFrom, extractOut;
    int onsetIndex = 0, hitLengthMs = 170, fadeMs = 30;
    float extractPeak = 0.92f;
    bool alignOnly = false;        // keep the whole tail, only remove the head
    float alignPeakDb = -3.0f;     // headroom measured on a 4x oversampled peak
    float probeHz = 0.0f;      // 0 = run the standard two-tone pass
    float probeSeconds = 0.55f;
    int audioPath = 3;   // DualSense output-path selector; sweep with --audio-path
    bool pathSweep = false;
    bool speakerDiag = false;
    int preGain = 7;
    int validFlag0 = -1;   // <0 = use the struct default
    int validFlag1 = -1;
    std::string dumpSubmit, writeConfig;
    bool hidInfo = false;
    int forceLength = 0;
    /// Haptic waveform under test. The default is '-', meaning "use the config
    /// file as it stands" -- a/b/c are explicit comparison presets and must be
    /// asked for, so nothing silently overrides the tuned values.
    char hapticVariant = '-';
    float speakerTrimDb = 0.0f;
    bool speakerTrimGiven = false;
    float speakerHighpass = -1.0f, speakerLowpass = -1.0f;
    float bufferMs = 20.0f;
    float duckFadeMs = 5.0f;
    bool audioDiag = false;
    bool overlapTest = false;
    std::string dumpMix;
    /// Also fire a cue when an ENEMY blocks or deflects. Off by default: the
    /// signal says "this character guarded", not "it guarded MY attack", so
    /// turning it on is an explicit choice (docs/astra/results/DEFLECT_STATUS.md 8.3).
    bool enemy = false;
    /// Seconds between rebuilds of the tracked enemy set.
    ///
    /// Was 6 s, which made detection "not work at first and then suddenly
    /// start": the set and the offset path are derived at startup, when the
    /// right characters may not be loaded yet, and only a later pass fixes
    /// them. The walk now costs ~134 ms, so waiting 6 s bought nothing.
    float enemyRediscoverSeconds = 2.0f;
};

/// The three haptic waveforms being compared, as data.
///
/// A is whatever the config holds -- untouched, so the thing the user has
/// already felt stays available as the reference. B and C change the SHAPE:
/// the body layer gets a real rise/hold span instead of an exponential that
/// starts dying at t=0, which is why the old cue was only felt for an instant.
/// C is B at a higher digital peak. 0.45 is an AMPLITUDE, not "45% of the
/// force" -- the actuator's output at 145 Hz is not a linear function of it.
void ApplyHapticVariant(GuardCueConfig& config, char variant) {
    if (variant == 'a') {
        // The ORIGINAL short, quiet cue, spelled out rather than meaning
        // "whatever the config happens to hold" -- the config default has
        // since become the long/strong one, so 'a' has to name its own
        // numbers to stay a usable reference point.
        auto& d = config.deflect.haptic;
        d.totalMs = 72.0f; d.normalizePeak = 0.30f; d.noiseAmplitude = 0.10f;
        d.contact = HapticLayer{0.0f, 10.0f, 260.0f, 220.0f, 0.7f, 0.0f, 1.00f, 150.0f};
        d.body    = HapticLayer{0.0f, 30.0f, 205.0f, 205.0f, 1.2f, 0.0f, 0.55f,  70.0f};
        d.ring    = HapticLayer{5.0f, 72.0f, 250.0f, 250.0f, 3.0f, 0.0f, 0.14f,  38.0f};
        auto& b = config.block.haptic;
        b.totalMs = 92.0f; b.normalizePeak = 0.30f; b.noiseAmplitude = 0.06f;
        b.contact = HapticLayer{ 0.0f, 14.0f, 185.0f, 185.0f, 1.6f, 0.0f, 0.50f, 110.0f};
        b.body    = HapticLayer{ 0.0f, 55.0f, 115.0f, 115.0f, 4.0f, 0.0f, 1.00f,  42.0f};
        b.ring    = HapticLayer{12.0f, 92.0f, 210.0f, 210.0f, 4.0f, 0.0f, 0.11f,  30.0f};
        return;
    }
    if (variant != 'b' && variant != 'c') return;   // anything else: config as-is
    const float peak = (variant == 'c') ? 0.45f : 0.30f;

    auto& d = config.deflect.haptic;
    d.totalMs = 85.0f;
    d.normalizePeak = peak;
    d.noiseAmplitude = 0.10f;
    //                start  end   f0     f1    rise  hold  weight decay
    d.contact = HapticLayer{ 0.0f,  8.0f, 260.0f, 220.0f, 0.7f,  0.0f, 0.90f, 170.0f};
    d.body    = HapticLayer{ 4.0f, 40.0f, 190.0f, 190.0f, 3.0f, 12.0f, 1.00f,  55.0f};
    d.ring    = HapticLayer{30.0f, 85.0f, 250.0f, 250.0f, 4.0f,  0.0f, 0.12f,  30.0f};

    auto& b = config.block.haptic;
    b.totalMs = 105.0f;
    b.normalizePeak = peak;
    b.noiseAmplitude = 0.06f;
    b.contact = HapticLayer{ 0.0f,  12.0f, 180.0f, 180.0f, 1.6f,  0.0f, 0.80f, 130.0f};
    b.body    = HapticLayer{ 4.0f,  60.0f, 145.0f, 145.0f, 4.0f, 14.0f, 1.00f,  38.0f};
    b.ring    = HapticLayer{45.0f, 105.0f, 210.0f, 210.0f, 5.0f,  0.0f, 0.12f,  26.0f};
}

/// Speaker-only level/EQ overrides from the CLI. Kept strictly apart from
/// anything haptic: these filters sit in the band the voice coils work in, so
/// applying them there would be changing the thing under test.
void ApplySpeakerOverrides(GuardCueConfig& config, const Options& o) {
    for (auto* profile : {&config.deflect, &config.block}) {
        if (o.speakerTrimGiven) profile->speaker.trimDb = o.speakerTrimDb;
        if (o.speakerHighpass >= 0.0f) profile->speaker.highpassHz = o.speakerHighpass;
        if (o.speakerLowpass >= 0.0f) profile->speaker.lowpassHz = o.speakerLowpass;
    }
}

/// CLI flags win; anything not given falls back to the config's device block,
/// so the channel map found by --channel-test only has to be typed once.
void ApplyDeviceDefaults(const GuardCueConfig& config, Options& o) {
    const auto& d = config.device;
    if (o.audio < 0) o.audio = d.audioEndpointIndex;
    if (o.speakerChannel < 0) o.speakerChannel = d.speakerChannel;
    if (o.hapticLeft < 0) o.hapticLeft = d.hapticLeftChannel;
    if (o.hapticRight < 0) o.hapticRight = d.hapticRightChannel;
    if (!o.speakerVolumeGiven) o.speakerVolume = d.controllerSpeakerVolume;
    if (!o.audioPathGiven) o.audioPath = d.audioOutputPath;
    if (!o.preGainGiven) o.preGain = d.speakerPreGain;
    if (o.endpointVolume < 0.0f) o.endpointVolume = d.windowsEndpointVolume;
}

GuardCueConfig LoadConfig(const Options& o) {
    GuardCueConfig config = DefaultGuardCueConfig();
    if (std::ifstream in(o.config); in) {
        std::stringstream buffer; buffer << in.rdbuf();
        std::string error;
        if (!ParseGuardCueConfig(buffer.str(), config, error))
            std::cout << "config error in " << o.config << " (using defaults): " << error << "\n";
        else
            std::cout << "config: " << o.config << "\n";
    } else {
        std::cout << "config: (built-in defaults)\n";
    }
    config.speakerVolume *= o.volume;
    config.hapticStrength *= o.strength;
    return config;
}

void WriteWav(const std::string& path, const std::vector<float>& mono, std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cout << "  could not write " << path << "\n"; return; }
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(mono.size() * 2);
    auto u32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    out.write("data", 4); u32(dataBytes);
    for (float s : mono) {
        const auto v = static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        out.write(reinterpret_cast<const char*>(&v), 2);
    }
    std::cout << "  wrote " << path << " (" << mono.size() << " samples)\n";
}

float Rms(const std::vector<float>& c) {
    if (c.empty()) return 0.0f;
    double sum = 0;
    for (float s : c) sum += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(sum / static_cast<double>(c.size())));
}


/// True peak estimated with 4x linear oversampling. A sample-peak of 1.0 can
/// hide inter-sample peaks well above 0 dBFS, which is exactly what the source
/// MP3s do -- normalising on the sample peak alone would still clip on replay.
float OversampledPeak(const std::vector<float>& clip, int factor) {
    if (clip.empty()) return 0.0f;
    float peak = 0.0f;
    for (std::size_t i = 0; i + 1 < clip.size(); ++i) {
        for (int k = 0; k < factor; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(factor);
            peak = std::max(peak, std::fabs(clip[i] + (clip[i + 1] - clip[i]) * t));
        }
    }
    return std::max(peak, std::fabs(clip.back()));
}

/// First sample where a 5 ms sliding RMS crosses `fraction` of its own maximum.
/// This is the "sound starts here" measure, matching how the source files were
/// characterised externally.
std::size_t RmsOnset(const std::vector<float>& clip, std::uint32_t rate, float fraction) {
    if (clip.empty()) return 0;
    const auto window = std::max<std::size_t>(1, static_cast<std::size_t>(0.005 * rate));
    std::vector<float> rms(clip.size(), 0.0f);
    double sum = 0;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        sum += static_cast<double>(clip[i]) * clip[i];
        if (i >= window) sum -= static_cast<double>(clip[i - window]) * clip[i - window];
        rms[i] = static_cast<float>(std::sqrt(sum / static_cast<double>(std::min(i + 1, window))));
    }
    const float peak = *std::max_element(rms.begin(), rms.end());
    const float threshold = peak * fraction;
    for (std::size_t i = 0; i < rms.size(); ++i)
        if (rms[i] >= threshold) return i > window ? i - window : 0;
    return 0;
}

/// Impact starts, found on a smoothed envelope. Used both to warn that a
/// recording contains several hits and to slice one of them out.
std::vector<std::size_t> FindOnsets(const std::vector<float>& clip, std::uint32_t rate,
                                    float relativeThreshold, float minSeparationMs) {
    std::vector<std::size_t> onsets;
    if (clip.empty()) return onsets;
    const auto window = std::max<std::size_t>(1, static_cast<std::size_t>(0.005 * rate));
    std::vector<float> envelope(clip.size(), 0.0f);
    double running = 0;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        running += std::fabs(clip[i]);
        if (i >= window) running -= std::fabs(clip[i - window]);
        envelope[i] = static_cast<float>(running / static_cast<double>(std::min(i + 1, window)));
    }
    const float peak = *std::max_element(envelope.begin(), envelope.end());
    const float threshold = peak * relativeThreshold;
    const auto separation = static_cast<std::size_t>(minSeparationMs / 1000.0f * static_cast<float>(rate));
    std::size_t i = 0;
    while (i < envelope.size()) {
        if (envelope[i] >= threshold) {
            const auto limit = std::min(envelope.size(), i + separation);
            const auto local = static_cast<std::size_t>(
                std::max_element(envelope.begin() + static_cast<std::ptrdiff_t>(i),
                                 envelope.begin() + static_cast<std::ptrdiff_t>(limit)) - envelope.begin());
            onsets.push_back(local);
            i = local + separation;
        } else {
            ++i;
        }
    }
    return onsets;
}


/// Keeps only the onsets that are genuine ATTACKS.
///
/// FindOnsets reports one local maximum per `separation` for as long as the
/// envelope stays above the threshold, which is right for finding strikes in a
/// recording but wrong for a sustained ring: a synthesised tail that starts
/// near the recording's end level sits above 35% of peak for a while and gets
/// reported as extra "impacts" even though it only ever decays. That showed up
/// as `contains 2 impacts` on a clip whose tail is monotonic by construction
/// (see the ExtendDecayTail unit test).
///
/// So an onset is kept only if the envelope actually ROSE into it. A real
/// second strike rises; a decaying tail cannot. This keeps the guard that
/// caught a genuine 4-impact clip while dropping the false positives.
std::vector<std::size_t> KeepAttacks(const std::vector<float>& clip, std::uint32_t rate,
                                     const std::vector<std::size_t>& onsets, float riseFactor) {
    std::vector<std::size_t> kept;
    if (clip.empty() || onsets.empty()) return kept;
    const auto window = std::max<std::size_t>(1, static_cast<std::size_t>(0.005 * rate));
    std::vector<float> envelope(clip.size(), 0.0f);
    double running = 0;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        running += std::fabs(clip[i]);
        if (i >= window) running -= std::fabs(clip[i - window]);
        envelope[i] = static_cast<float>(running / static_cast<double>(std::min(i + 1, window)));
    }
    const auto lookBack = static_cast<std::size_t>(0.015 * rate);
    for (std::size_t onset : onsets) {
        if (onset < lookBack) { kept.push_back(onset); continue; }   // the first hit
        const float before = envelope[onset - lookBack];
        if (envelope[onset] >= before * riseFactor) kept.push_back(onset);
    }
    return kept;
}

/// The speaker cue: a supplied single-hit recording when the profile names
/// one, otherwise the synthesised metal clang. Loading happens here (the core
/// library stays OS-independent) and ALWAYS before the playback loop starts.
std::vector<float> RenderSpeakerCue(const GuardCueProfile& profile, std::uint32_t rate,
                                    float masterVolume, std::string& sourceLabel) {
    if (!profile.speaker.clipPath.empty()) {
        AudioClipInfo info;
        auto clip = LoadAudioClipMono(profile.speaker.clipPath, rate, info);
        if (info.ok && profile.speaker.clipRaw) {
            // As recorded. Only the leading silence goes (256 ms on the
            // deflect file, 515 ms on the parry file -- left in, the clang
            // would arrive that late), and only trimDb scales it, because
            // these files decode above 0 dBFS and would otherwise clip.
            TrimLeadingSilence(clip, 0.003f);
            const auto shaped = ApplySpeakerShaping(clip, rate, profile.speaker);
            const auto attacks = KeepAttacks(clip, rate, FindOnsets(clip, rate, 0.35f, 60.0f), 1.6f);
            sourceLabel = profile.speaker.clipPath + " (AS RECORDED, " +
                          std::to_string(clip.size() * 1000 / rate) + "ms, " +
                          std::to_string(attacks.size()) + " impacts, trim " +
                          std::to_string(static_cast<int>(shaped.trimDb)) + "dB)";
            return clip;
        }
        if (info.ok) {
            TrimLeadingSilence(clip, 0.003f);
            NormalizePeak(clip, profile.speaker.clipPeak);
            // Rebuild the ring the 66 ms single-impact slice had to discard.
            //
            // BEFORE the fade-out, not after. Fading the slice to zero and
            // then starting a tail at the slice's own level puts a step at the
            // join -- a real re-attack, which the onset check correctly
            // reported as a second impact. The tail brings its own end fade,
            // so the slice only needs one when there is no tail.
            const bool hasTail = profile.speaker.tailMs >= 5.0f;
            if (!hasTail) ApplyFadeOut(clip, rate, 8.0f);
            ExtendDecayTail(clip, rate, profile.speaker.tailMs,
                            profile.speaker.tailDecayPerSecond, profile.speaker.tailGrainMs);
            for (float& sample : clip) sample *= std::clamp(masterVolume, 0.0f, 1.0f);
            // Trim/EQ last, and report what it cost in level: a filtered clip
            // that is also quieter cannot be compared to an unfiltered one by
            // ear without knowing that.
            const auto shaped = ApplySpeakerShaping(clip, rate, profile.speaker);
            if (shaped.highpassApplied || shaped.lowpassApplied || shaped.trimDb != 0.0f)
                std::cout << "  speaker shaping (" << profile.name << "):"
                          << (shaped.highpassApplied ? " HP" + std::to_string(int(profile.speaker.highpassHz)) + "Hz" : "")
                          << (shaped.lowpassApplied ? " LP" + std::to_string(int(profile.speaker.lowpassHz)) + "Hz" : "")
                          << " trim=" << shaped.trimDb << "dB"
                          << "  peak " << shaped.peakBefore << " -> " << shaped.peakAfter
                          << "  rms " << shaped.rmsBefore << " -> " << shaped.rmsAfter
                          << " (" << 20.0f * std::log10(std::max(shaped.rmsAfter, 1e-9f) /
                                                        std::max(shaped.rmsBefore, 1e-9f))
                          << " dB)\n";
            const auto attacks = KeepAttacks(clip, rate, FindOnsets(clip, rate, 0.35f, 60.0f), 1.6f);
            sourceLabel = profile.speaker.clipPath + " (" +
                          std::to_string(clip.size() * 1000 / rate) + "ms, " +
                          std::to_string(attacks.size()) + " impact)";
            if (attacks.size() > 1)
                std::cout << "  WARNING: " << profile.speaker.clipPath << " contains "
                          << attacks.size() << " impacts -- one event will play all of them.\n"
                             "           Slice a single hit with --extract-hit.\n";
            return clip;
        }
        std::cout << "  could not load " << profile.speaker.clipPath << " (" << info.error
                  << "); falling back to the synthesised cue\n";
    }
    sourceLabel = "synthesised";
    return SynthesizeGuardSpeaker(profile.speaker, rate, masterVolume);
}

/// Routes controller audio to the built-in speaker over HID, at an explicit
/// level. Never changes any Windows audio setting. Returns true when the
/// report was actually written.
/// Builds the settings the CLI flags describe. The speaker volume byte is NOT
/// a 0..255 scale (see DualSenseUsbReport.hpp) -- values above 0x64 are outside
/// what hid-playstation ever sends, so they are reported rather than silently
/// accepted.
dualsense_protocol::SpeakerOutputSettings SpeakerSettingsFrom(const Options& o) {
    dualsense_protocol::SpeakerOutputSettings settings;
    settings.speakerVolume = static_cast<std::uint8_t>(std::clamp(o.speakerVolume, 0, 255));
    settings.headphoneVolume = settings.speakerVolume;
    settings.outputPath = static_cast<std::uint8_t>(std::clamp(o.audioPath, 0, 3));
    settings.speakerPreGain = static_cast<std::uint8_t>(std::clamp(o.preGain, 0, 7));
    if (o.validFlag0 >= 0) settings.validFlag0 = static_cast<std::uint8_t>(o.validFlag0 & 0xFF);
    if (o.validFlag1 >= 0) settings.validFlag1 = static_cast<std::uint8_t>(o.validFlag1 & 0xFF);
    return settings;
}

bool WriteSpeakerReport(HidApiDualSenseTransport& transport,
                        const dualsense_protocol::SpeakerOutputSettings& settings, bool verbose) {
    const auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) return false;
    if (!transport.IsOpen() && transport.Open(candidates.front().path) != TransportResult::Success) return false;
    const auto report = dualsense_protocol::BuildSpeakerOutputReport(settings);
    const auto result = transport.WriteOutputReport(report.data(), report.size());
    if (verbose) {
        std::cout << "    report id=0x" << std::hex << int(report[0])
                  << " valid0=0x" << int(report[1]) << " valid1=0x" << int(report[2])
                  << " hpVol=0x" << int(report[5]) << " spkVol=0x" << int(report[6])
                  << " audioFlags=0x" << int(report[8]) << " audioFlags2=0x" << int(report[38])
                  << std::dec << "  len=" << report.size()
                  << "  write=" << ToString(result) << "\n";
    }
    return result == TransportResult::Success;
}

bool EnableSpeakerRouting(HidApiDualSenseTransport& transport, int volume, int outputPath = 3) {
    dualsense_protocol::SpeakerOutputSettings settings;
    settings.speakerVolume = static_cast<std::uint8_t>(std::clamp(volume, 0, 255));
    settings.headphoneVolume = settings.speakerVolume;
    settings.outputPath = static_cast<std::uint8_t>(std::clamp(outputPath, 0, 3));
    return WriteSpeakerReport(transport, settings, false);
}

void ReleaseSpeakerRouting(HidApiDualSenseTransport& transport) {
    dualsense_protocol::SpeakerOutputSettings settings;
    settings.enable = false;
    const auto off = dualsense_protocol::BuildSpeakerOutputReport(settings);
    transport.WriteOutputReport(off.data(), off.size());
    transport.Close();
}

int ListDevices() {
    std::cout << "HID candidates (USB DualSense):\n";
    HidApiDualSenseTransport transport;
    const auto hids = transport.EnumerateCandidates();
    if (hids.empty()) std::cout << "  (none -- is the controller plugged in over USB?)\n";
    for (std::size_t i = 0; i < hids.size(); ++i) {
        std::cout << "  [" << i << "] vid=0x" << std::hex << hids[i].vendorId
                  << " pid=0x" << hids[i].productId << std::dec
                  << " product=\"" << Narrow(hids[i].product) << "\"\n        path=" << hids[i].path << "\n";
    }
    std::cout << "\nAudio render endpoints:\n";
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  [" << i << "] " << Narrow(endpoints[i].name)
                  << (endpoints[i].looksLikeDualSense ? "   <-- looks like the controller" : "") << "\n";
    }
    std::cout << "\nPick the DualSense render endpoint index and pass it as --audio N.\n"
                 "Windows' own default output device is never changed by this tool.\n";
    return 0;
}

int ProbeFormat(const Options& o) {
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (o.audio < 0 || o.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    const auto& ep = endpoints[static_cast<std::size_t>(o.audio)];
    std::cout << "endpoint: " << Narrow(ep.name) << "\n";
    const auto r = DualSenseAudioDevice::Describe(ep.id);
    if (!r.queried) { std::cout << "  failed: " << r.error << "\n"; return 1; }
    std::cout << "  GetMixFormat        : " << r.sampleRate << " Hz, " << r.channels
              << " ch, " << r.bitsPerSample << " bit (" << r.validBitsPerSample << " valid), "
              << r.formatTag << "/" << r.subFormat << "\n"
              << "  channel mask        : 0x" << std::hex << r.channelMask << std::dec << "\n"
              << "  IsFormatSupported   : shared=" << r.sharedSupport
              << "  exclusive=" << r.exclusiveSupport << "\n"
              << "  Windows endpoint vol: "
              << (r.volumeKnown ? std::to_string(static_cast<int>(r.endpointVolume * 100)) + "%" : "unknown")
              << (r.endpointMuted ? "   <-- MUTED" : "") << "\n\n"
              << "This tool writes the mix format unchanged in shared mode. It does not\n"
                 "substitute a different channel layout -- if shared were not S_OK it refuses.\n\n"
              << "Channel roles are NOT assumed. Run:\n"
              << "  --channel-test --audio " << o.audio << "\n";
    return 0;
}


/// Plays the same tone on one channel once per output-path value. Which value
/// reaches the built-in speaker is not documented anywhere this project can
/// verify, so it is found by listening rather than asserted.

/// Prints every level that sits between a sample value and the speaker cone.
void ReportVolumes(const AudioFormatReport& r) {
    std::cout << "  endpoint master : "
              << (r.volumeKnown ? std::to_string(static_cast<int>(r.endpointVolume * 100)) + "% scalar"
                                : std::string("unknown"))
              << ", " << r.endpointVolumeDb << " dB"
              << " (range " << r.endpointVolumeMinDb << ".." << r.endpointVolumeMaxDb << " dB)"
              << (r.endpointMuted ? "   <-- MUTED" : "") << "\n";
    std::cout << "      NOTE: the scalar is a taper, not an amplitude. 0.8 scalar is not 80%\n"
                 "            of the sample values -- the dB figure is what predicts loudness.\n";
    if (!r.endpointChannelVolumes.empty()) {
        std::cout << "  endpoint channels:";
        for (float v : r.endpointChannelVolumes) std::cout << " " << static_cast<int>(v * 100) << "%";
        std::cout << "\n";
    }
    std::cout << "  app session     : "
              << (r.sessionVolumeKnown ? std::to_string(static_cast<int>(r.sessionVolume * 100)) + "%"
                                       : std::string("unknown"))
              << (r.sessionMuted ? "   <-- MUTED" : "") << "\n";
    if (!r.streamChannelVolumes.empty()) {
        std::cout << "  stream channels :";
        for (float v : r.streamChannelVolumes) std::cout << " " << static_cast<int>(v * 100) << "%";
        std::cout << "\n";
    }
}

void ReportClip(const char* stage, const std::vector<float>& clip, std::uint32_t rate, unsigned channels) {
    if (clip.empty()) { std::cout << "    " << stage << ": empty\n"; return; }
    double sum = 0;
    float peak = 0;
    for (float v : clip) { sum += static_cast<double>(v) * v; peak = std::max(peak, std::fabs(v)); }
    const double rms = std::sqrt(sum / static_cast<double>(clip.size()));
    const auto frames = clip.size() / std::max(1u, channels);
    std::cout << "    " << stage << ": frames=" << frames << " (" << (frames * 1000 / rate) << " ms)"
              << " samples=" << clip.size() << " ch=" << channels
              << " peak=" << peak << " (" << 20.0 * std::log10(std::max(peak, 1e-9f)) << " dBFS)"
              << " rms=" << rms << "\n";
}


/// Windows knows the exact byte length this device's output reports must be.
/// A write of any other length fails, and hid_write() reports that failure the
/// same way it reports "someone else owns the device" -- so the length has to
/// be measured rather than guessed. 64 was an assumption in this code.
int HidInfo(const Options& o) {
    (void)o;
    HidApiDualSenseTransport transport;
    const auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) { std::cout << "no DualSense HID device found\n"; return 1; }

    for (const auto& candidate : candidates) {
        std::cout << "\ndevice: " << Narrow(candidate.product) << "\n  path: " << candidate.path << "\n";
        const HANDLE handle = CreateFileA(candidate.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            std::cout << "  CreateFile(read+write) failed: " << GetLastError()
                      << "   <-- another process may hold this device\n";
            const HANDLE ro = CreateFileA(candidate.path.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
            std::cout << "  CreateFile(read-only)  : "
                      << (ro == INVALID_HANDLE_VALUE ? "also failed" : "succeeded") << "\n";
            if (ro != INVALID_HANDLE_VALUE) CloseHandle(ro);
            continue;
        }
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(handle, &preparsed)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                std::cout << "  declared report lengths:  output=" << caps.OutputReportByteLength
                          << "  input=" << caps.InputReportByteLength
                          << "  feature=" << caps.FeatureReportByteLength << "\n"
                          << "  usage page=0x" << std::hex << caps.UsagePage
                          << " usage=0x" << caps.Usage << std::dec << "\n";
                if (caps.OutputReportByteLength != 64)
                    std::cout << "  *** this is NOT 64 -- writing 64 bytes cannot succeed ***\n";
            }
            HidD_FreePreparsedData(preparsed);
        } else {
            std::cout << "  HidD_GetPreparsedData failed: " << GetLastError() << "\n";
        }
        CloseHandle(handle);
    }

    // Try the write at each plausible length so the working one is identified
    // by evidence rather than by reading a spec.
    std::cout << "\nwrite attempts (a harmless audio report, motors untouched):\n";
    if (transport.Open(candidates.front().path) != TransportResult::Success) {
        std::cout << "  could not open for writing\n";
        return 1;
    }
    dualsense_protocol::SpeakerOutputSettings settings;
    const auto report = dualsense_protocol::BuildSpeakerOutputReport(settings);
    for (std::size_t length : {std::size_t(48), std::size_t(49), std::size_t(63),
                               std::size_t(64), std::size_t(78)}) {
        if (length > report.size()) {
            std::cout << "  " << length << " bytes: skipped (buffer is only " << report.size() << ")\n";
            continue;
        }
        const auto result = transport.WriteOutputReport(report.data(), length);
        std::cout << "  " << length << " bytes -> " << ToString(result) << "\n";
    }
    transport.Close();
    std::cout << "\nIf every length fails, the device is being held by another process\n"
                 "(Steam Input is the usual one). If exactly one length succeeds, that is\n"
                 "the length this code should be using.\n";
    return 0;
}

int SpeakerDiag(const Options& o) {
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (o.audio < 0 || o.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    const auto& ep = endpoints[static_cast<std::size_t>(o.audio)];
    std::cout << "endpoint [" << o.audio << "] " << Narrow(ep.name) << "\n";
    // The index is positional and can move; the id is what actually selects it.
    std::wcout << L"  endpoint id: " << ep.id << L"\n";
    if (!ep.looksLikeDualSense)
        std::cout << "  WARNING: this endpoint's name does not look like a DualSense.\n";

    HidApiDualSenseTransport transport;
    const auto hids = transport.EnumerateCandidates();
    std::cout << "  HID controllers seen: " << hids.size();
    if (!hids.empty()) std::cout << "  (using \"" << Narrow(hids.front().product) << "\")";
    std::cout << "\n\n";

    if (o.endpointVolume >= 0.0f) {
        const auto set = DualSenseAudioDevice::SetEndpointVolume(ep.id, o.endpointVolume);
        std::cout << "  SetEndpointVolume(" << o.endpointVolume << "): " << set.hresult << "\n"
                  << "    read back: "
                  << (set.readBackOk ? std::to_string(static_cast<int>(set.readBackScalar * 100)) + "% / " +
                                       std::to_string(set.readBackDb) + " dB"
                                     : std::string("read failed"))
                  << (set.readBackMuted ? "  STILL MUTED" : "") << "\n"
                  << "    (this endpoint only; other apps using this device are affected,\n"
                     "     the Windows default playback device is not changed)\n";
    }

    DualSenseAudioDevice device;
    if (!device.Open(ep.id)) { std::cout << "open failed: " << device.Error() << "\n"; return 1; }
    device.RefreshSessionVolumes();
    const auto& fmt = device.Format();
    std::cout << "\n  format: " << fmt.sampleRate << " Hz, " << fmt.channels << " ch, "
              << fmt.subFormat << "/" << fmt.bitsPerSample
              << ", IsFormatSupported(shared)=" << fmt.sharedSupport << "\n";
    ReportVolumes(fmt);

    const auto rate = fmt.sampleRate;
    const int speakerCh = o.speakerChannel >= 0 ? o.speakerChannel : 0;
    if (speakerCh >= static_cast<int>(fmt.channels)) { std::cout << "--speaker-channel out of range\n"; return 2; }

    // A known signal, so every later number can be compared against it.
    const float seconds = std::clamp(o.probeSeconds, 0.2f, 5.0f);
    const float hz = o.probeHz > 0.0f ? o.probeHz : 1000.0f;
    std::vector<float> tone(static_cast<std::size_t>(seconds * static_cast<float>(rate)));
    for (std::size_t i = 0; i < tone.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float env = std::min(1.0f, t / 0.005f) * std::min(1.0f, (seconds - t) / 0.02f);
        tone[i] = 0.5f * std::sin(6.2831853f * hz * t) * std::max(0.0f, env);
    }
    std::cout << "\n  test signal: " << hz << " Hz, " << seconds << " s, amplitude 0.5\n";
    ReportClip("after synthesis   ", tone, rate, 1);

    std::cout << "\n  HAPTIC CHANNELS ARE LEFT AT ZERO for this test, so a buzz from the\n"
                 "  actuators cannot be mistaken for the built-in speaker.\n"
                 "  Playing ONLY on channel " << speakerCh << ".\n\n";

    struct Step { const char* label; dualsense_protocol::SpeakerOutputSettings settings; };
    std::vector<Step> steps;
    auto base = SpeakerSettingsFrom(o);
    if (o.speakerVolume > 0x64)
        std::cout << "  NOTE: --speaker-volume 0x" << std::hex << o.speakerVolume << std::dec
                  << " is above 0x64, which is hid-playstation's 100%. Sweeping anyway.\n\n";

    // One variable at a time, so whatever changes the result is identifiable.
    steps.push_back({"no HID report at all (endpoint only)", {}});
    steps.back().settings.enable = false;
    for (int path = 0; path <= 3; ++path) {
        auto st = base; st.outputPath = static_cast<std::uint8_t>(path);
        steps.push_back({"path sweep", st});
    }
    for (std::uint8_t vol : {std::uint8_t(0x3D), std::uint8_t(0x50), std::uint8_t(0x64), std::uint8_t(0xFF)}) {
        auto st = base; st.speakerVolume = vol; st.headphoneVolume = vol;
        steps.push_back({"volume sweep", st});
    }
    for (std::uint8_t pre : {std::uint8_t(0), std::uint8_t(3), std::uint8_t(7)}) {
        auto st = base; st.speakerPreGain = pre;
        steps.push_back({"pre-gain sweep", st});
    }
    for (std::uint8_t v1 : {std::uint8_t(0x00), std::uint8_t(0x40), std::uint8_t(0x80), std::uint8_t(0xFF)}) {
        auto st = base; st.validFlag1 = v1;
        steps.push_back({"valid_flag1 sweep", st});
    }

    std::size_t index = 0;
    for (const auto& step : steps) {
        std::cout << "  [" << index++ << "] " << step.label
                  << "  path=" << int(step.settings.outputPath)
                  << " vol=0x" << std::hex << int(step.settings.speakerVolume)
                  << " pre=" << std::dec << int(step.settings.speakerPreGain)
                  << " valid0=0x" << std::hex << int(step.settings.validFlag0)
                  << " valid1=0x" << int(step.settings.validFlag1) << std::dec
                  << (step.settings.enable ? "" : "   (report disabled)") << std::endl;
        if (step.settings.enable) WriteSpeakerReport(transport, step.settings, true);
        device.DropPending();
        if (index == 1) device.BeginSubmitCapture(static_cast<std::size_t>(seconds * rate) + rate / 10);
        device.Queue(tone, speakerCh, 1.0f);
        const auto until = NowUs() + static_cast<std::int64_t>(seconds * 1.35f * 1'000'000);
        while (NowUs() < until) {
            if (!device.Pump()) { std::cout << "    pump failed: " << device.Error() << "\n"; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        if (index == 1) {
            device.EndSubmitCapture();
            ReportClip("submitted to WASAPI", device.SubmitCapture(), rate, device.SubmitCaptureChannels());
            if (!o.dumpSubmit.empty()) {
                // De-interleave the speaker channel so the written file is what
                // that one channel actually carried.
                const auto ch = device.SubmitCaptureChannels();
                std::vector<float> mono;
                const auto& cap = device.SubmitCapture();
                for (std::size_t i = static_cast<std::size_t>(speakerCh); i < cap.size(); i += ch)
                    mono.push_back(cap[i]);
                ReportClip("speaker channel only", mono, rate, 1);
                std::filesystem::path out(o.dumpSubmit);
                std::error_code ec;
                if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
                WriteWav(out.string(), mono, rate);
            }
        }
    }
    device.DropPending();
    device.Close();
    ReleaseSpeakerRouting(transport);
    std::cout << "\nWhich step number was clearly LOUDER from the controller's own speaker?\n"
                 "If none was, the speaker may not be reachable through this endpoint at all --\n"
                 "that is a result, not a failure to report.\n";
    return 0;
}

int PathSweep(const Options& o) {
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (o.audio < 0 || o.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    if (o.endpointVolume >= 0.0f)
        DualSenseAudioDevice::SetEndpointVolume(endpoints[static_cast<std::size_t>(o.audio)].id,
                                                o.endpointVolume);
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(o.audio)].id)) {
        std::cout << "open failed: " << device.Error() << "\n";
        return 1;
    }
    const auto rate = device.Format().sampleRate;
    const int channels = device.Format().channels;
    const int channel = o.channel >= 0 ? o.channel : 0;
    if (channel >= channels) { std::cout << "--channel out of range\n"; return 2; }

    const float hz = o.probeHz > 0.0f ? o.probeHz : 880.0f;
    const float seconds = std::clamp(o.probeSeconds, 0.05f, 5.0f);
    std::vector<float> clip(static_cast<std::size_t>(seconds * static_cast<float>(rate)));
    for (std::size_t i = 0; i < clip.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float env = std::min(1.0f, t / 0.005f) * std::min(1.0f, (seconds - t) / 0.02f);
        clip[i] = 0.8f * std::clamp(o.probeGain, 0.0f, 1.0f) *
                  std::sin(6.2831853f * hz * t) * std::max(0.0f, env);
    }

    HidApiDualSenseTransport transport;
    std::cout << "sweeping output paths on channel " << channel << " at " << hz << " Hz\n"
              << "listen for the CONTROLLER'S OWN SPEAKER (not your PC speakers)\n\n";
    for (int path = 0; path <= 3; ++path) {
        const bool sent = EnableSpeakerRouting(transport, o.speakerVolume, path);
        std::cout << "  --audio-path " << path << "   routing report "
                  << (sent ? "sent" : "NOT SENT") << std::endl;
        device.DropPending();
        device.Queue(clip, channel, 1.0f);
        const auto until = NowUs() + static_cast<std::int64_t>(seconds * 1.4f * 1'000'000);
        while (NowUs() < until) {
            if (!device.Pump()) { std::cout << "    pump failed: " << device.Error() << "\n"; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    device.DropPending();
    device.Close();
    ReleaseSpeakerRouting(transport);
    std::cout << "\nWhich --audio-path value made the controller's built-in speaker sound?\n"
                 "If none did on this channel, try --channel 1.\n";
    return 0;
}

int ChannelTest(const Options& o) {
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (o.audio < 0 || o.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(o.audio)].id)) {
        std::cout << "open failed: " << device.Error() << "\n";
        return 1;
    }
    const auto& f = device.Format();
    std::cout << "opened " << Narrow(endpoints[static_cast<std::size_t>(o.audio)].name)
              << ": " << f.sampleRate << " Hz, " << f.channels << " ch, "
              << f.subFormat << "/" << f.bitsPerSample << "\n\n";

    // Two very different probes so the role is obvious: an audible mid tone,
    // then a low buzz that a voice coil reproduces as vibration.
    auto tone = [&](float hz, float seconds, float amp) {
        std::vector<float> clip(static_cast<std::size_t>(seconds * static_cast<float>(f.sampleRate)));
        for (std::size_t i = 0; i < clip.size(); ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(f.sampleRate);
            const float env = std::min(1.0f, t / 0.005f) * std::min(1.0f, (seconds - t) / 0.02f);
            clip[i] = amp * std::sin(6.2831853f * hz * t) * std::max(0.0f, env);
        }
        return clip;
    };
    const float g = std::clamp(o.probeGain, 0.0f, 1.0f);
    const float seconds = std::clamp(o.probeSeconds, 0.05f, 5.0f);
    // A single explicit frequency when asked for. 60-80 Hz is the useful case:
    // a voice coil reproduces it as a clear shake, while a tiny speaker barely
    // makes it audible -- so "felt" and "heard" stop being confusable.
    const bool single = o.probeHz > 0.0f;
    const auto audible = tone(single ? o.probeHz : 880.0f, seconds, 0.80f * g);
    const auto buzz = tone(single ? o.probeHz : 150.0f, seconds, 0.95f * g);

    const int first = o.channel >= 0 ? o.channel : 0;
    const int last = o.channel >= 0 ? o.channel : static_cast<int>(f.channels) - 1;
    for (int ch = first; ch <= last; ++ch) {
        for (int pass = 0; pass < (single ? 1 : 2); ++pass) {
            const bool low = pass == 1;
            if (single)
                std::cout << "  channel " << ch << " : " << o.probeHz
                          << " Hz  -- do you FEEL it in your palms?" << std::endl;
            else
                std::cout << "  channel " << ch << " : " << (low ? "150 Hz buzz  (a haptic actuator VIBRATES)"
                                                                 : "880 Hz tone  (a speaker SOUNDS)") << std::endl;
            device.DropPending();
            device.Queue(low ? buzz : audible, ch, 1.0f);
            const auto until = NowUs() + static_cast<std::int64_t>(seconds * 1.3f * 1'000'000);
            while (NowUs() < until) {
                if (!device.Pump()) { std::cout << "    pump failed: " << device.Error() << "\n"; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        }
        std::cout << "  ---- end of channel " << ch << " ----\n";
    }
    device.DropPending();
    device.Close();
    std::cout << "\nNote which channel SOUNDED and which VIBRATED, then pass them as\n"
                 "  --speaker-channel K --haptic-left K --haptic-right K\n";
    return 0;
}

struct Hit { bool deflect; };

std::vector<Hit> BuildPattern(const std::string& pattern, int count) {
    std::vector<Hit> hits;
    if (pattern == "deflect") for (int i = 0; i < count; ++i) hits.push_back({true});
    else if (pattern == "block") for (int i = 0; i < count; ++i) hits.push_back({false});
    else if (pattern == "alternate") for (int i = 0; i < count; ++i) hits.push_back({i % 2 == 0});
    else if (pattern == "repeat") { // same kind repeated, then the other kind repeated
        for (int i = 0; i < count; ++i) hits.push_back({true});
        for (int i = 0; i < count; ++i) hits.push_back({false});
    } else if (pattern == "mixed") {
        const bool seq[] = {true, true, false, false, true};
        for (bool d : seq) hits.push_back({d});
    }
    return hits;
}

/// Everything the render path recorded, with the one distinction that matters
/// spelled out: padding at zero during silence is normal, padding at zero
/// while something was sounding is a dropout.
/// Voice cap and summed-overlap protection, applied identically everywhere so
/// a test and the live path cannot drift apart.
void ConfigureMixer(DualSenseAudioDevice& device, const GuardCueConfig& config, int speakerChannel) {
    device.SetVoiceLimit(config.voiceLimit, config.retireFadeMs);
    device.SetSpeakerChannel(speakerChannel);
    LimiterSettings limiter;
    limiter.threshold = config.limiterThreshold;
    limiter.speakerHeadroom = config.speakerHeadroom;
    limiter.hapticHeadroom = config.hapticHeadroom;
    device.SetLimiter(limiter);
}

void ReportRenderStats(const AudioRenderStats& r) {
    const double gapMs = static_cast<double>(r.worstFeedGapUs) / 1000.0;
    std::cout << "\n--- audio render ---\n"
              << "  buffer        : " << r.bufferFrames << " frames (" << r.bufferMs
              << " ms)   worst-case added latency " << r.writeAheadMs << " ms\n"
              << "  feeder        : "
              << (r.eventDriven ? "event-driven (the audio engine wakes the feeder)"
                                : "TIMED FALLBACK -- expect ~15.6 ms wakeups, i.e. dropouts")
              << "\n"
              << "  pumps         : " << r.pumps << "  framesSubmitted=" << r.framesSubmitted << "\n"
              << "  feed gap      : worst " << gapMs << " ms"
              << (gapMs > r.bufferMs ? "   <-- LONGER THAN THE BUFFER: this is a dropout"
                                     : "   (under the buffer, so no starvation from feeding)")
              << "\n"
              << "  padding       : max " << r.maxPadding << " frames"
              << "   zero-while-sounding=" << r.starvedWhileActive
              << "   zero-while-silent=" << r.idleZeroPadding
              << " (silent zeros are not underruns)\n"
              << "  voices        : started=" << r.voicesStarted
              << " playedToTheEnd=" << r.voicesFinished
              << " retiredAtCap=" << r.voicesRetired
              << " hardDropped=" << r.voicesEvicted
              << "   maxConcurrent=" << r.maxConcurrentVoices << "\n"
              << "  pre-limit peak : speaker=" << r.peakSpeakerBeforeLimit
              << " haptic=" << r.peakHapticBeforeLimit
              << "   (the SUM, before headroom did its job)\n"
              << "  limiter        : speakerFrames=" << r.limitedSpeakerFrames
              << " (worst -" << r.worstSpeakerReductionDb << " dB)"
              << "  hapticFrames=" << r.limitedHapticFrames
              << " (worst -" << r.worstHapticReductionDb << " dB)\n"
              << "  clipped samples: speaker=" << r.clippedSpeakerSamples
              << "  haptic=" << r.clippedHapticSamples
              << "   (per channel; the limiter is per channel too, never shared)\n";
}

/// The comparison the breakup actually needs: single shots far enough apart
/// that nothing can overlap, then each output on its own, then together, then
/// a fast run. Anything that only breaks up in one of these phases points at
/// that phase's difference and nothing else.
int AudioDiag(const Options& o) {
    GuardCueConfig config = LoadConfig(o);
    Options opts = o;
    ApplyDeviceDefaults(config, opts);
    ApplyHapticVariant(config, o.hapticVariant);
    ApplySpeakerOverrides(config, o);

    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (opts.audio < 0 || opts.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(opts.audio)].id, o.bufferMs)) {
        std::cout << "open failed: " << device.Error() << "\n";
        return 1;
    }
    ConfigureMixer(device, config, opts.speakerChannel);
    device.StartRenderThread();
    const auto rate = device.Format().sampleRate;

    std::string deflectSource, blockSource;
    auto deflectSpeaker = RenderSpeakerCue(config.deflect, rate, config.speakerVolume, deflectSource);
    auto blockSpeaker = RenderSpeakerCue(config.block, rate, config.speakerVolume, blockSource);
    auto deflectHaptic = SynthesizeGuardHaptic(config.deflect.haptic, rate, config.hapticStrength);
    auto blockHaptic = SynthesizeGuardHaptic(config.block.haptic, rate, config.hapticStrength);

    std::cout << "\nvariant=" << o.hapticVariant
              << "  speakerCh=" << opts.speakerChannel
              << " hapticL=" << opts.hapticLeft << " hapticR=" << opts.hapticRight << "\n"
              << "gain chain (so a doubled gain would show up here):\n"
              << "  haptic: normalizePeak=" << config.deflect.haptic.normalizePeak
              << " x profileGain=" << config.deflect.haptic.gain
              << " x hapticStrength=" << config.hapticStrength
              << " x voiceGain=1.0\n";
    ReportClip("deflect speaker", deflectSpeaker, rate, 1);
    ReportClip("block   speaker", blockSpeaker, rate, 1);
    ReportClip("deflect haptic ", deflectHaptic, rate, 1);
    ReportClip("block   haptic ", blockHaptic, rate, 1);

    HidApiDualSenseTransport transport;
    const bool routed = o.speakerRouting && EnableSpeakerRouting(transport, opts.speakerVolume, opts.audioPath);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent") << "\n";

    struct Phase { const char* name; bool speaker; bool haptic; int hits; int gapMs; };
    const Phase phases[] = {
        {"1 speaker only, single shots", true,  false, 3, 1500},
        {"2 haptic only, single shots",  false, true,  3, 1500},
        {"3 both together, single shots", true, true,  3, 1500},
        {"4 both, fast repeats",          true, true,  6,  180},
    };
    for (const auto& phase : phases) {
        device.ResetRenderStats();
        std::cout << "\n=== phase " << phase.name << " (" << phase.hits
                  << " hits, " << phase.gapMs << " ms apart) ===\n";
        for (int i = 0; i < phase.hits; ++i) {
            const bool deflect = (i % 2) == 0;
            if (phase.speaker && opts.speakerChannel >= 0)
                device.Queue(deflect ? deflectSpeaker : blockSpeaker, opts.speakerChannel, 1.0f);
            if (phase.haptic)
                device.QueuePair(deflect ? deflectHaptic : blockHaptic,
                                 opts.hapticLeft, opts.hapticRight, 1.0f, 0.0f);
            std::cout << "  hit " << i << " " << (deflect ? "DEFLECT" : "block") << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(phase.gapMs));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ReportRenderStats(device.RenderStats());
    }

    device.StopRenderThread();
    device.DropPending();
    device.Close();
    if (routed) ReleaseSpeakerRouting(transport); else transport.Close();
    std::cout << "\nThese are measurements of the output path only. Whether it still\n"
                 "sounds broken up is something only listening can answer.\n";
    return 0;
}

/// Interleaved multi-channel WAV, so the exact submitted mix can be opened in
/// an editor and looked at rather than argued about.
void WriteWavMulti(const std::string& path, const std::vector<float>& interleaved,
                   unsigned channels, std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cout << "  could not write " << path << "\n"; return; }
    const auto dataBytes = static_cast<std::uint32_t>(interleaved.size() * 2);
    auto u32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(static_cast<std::uint16_t>(channels));
    u32(rate); u32(rate * channels * 2); u16(static_cast<std::uint16_t>(channels * 2)); u16(16);
    out.write("data", 4); u32(dataBytes);
    for (float v : interleaved) {
        const auto sample = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        out.write(reinterpret_cast<const char*>(&sample), 2);
    }
    std::cout << "  wrote " << path << " (" << interleaved.size() / std::max(1u, channels)
              << " frames x " << channels << " ch)\n";
}

/// Where one channel of the captured mix actually starts and stops sounding.
/// This is what separates "the clip was cut short" from "the buffer ran dry":
/// a truncated clip ends early and cleanly, a starved buffer leaves a gap in
/// the middle.
struct MixSpan {
    double firstMs = -1.0, lastMs = -1.0, peak = 0.0;
    int gaps = 0;          // silent stretches longer than 5 ms between sound
    double longestGapMs = 0.0;
};
MixSpan MeasureChannel(const std::vector<float>& mix, unsigned channels, unsigned channel,
                       std::uint32_t rate) {
    MixSpan span;
    if (channels == 0 || channel >= channels) return span;
    const double msPerFrame = 1000.0 / static_cast<double>(rate);
    const auto quiet = static_cast<std::size_t>(5.0 / msPerFrame);
    const std::size_t frames = mix.size() / channels;

    // The threshold has to be RELATIVE to this channel's own peak. A fixed
    // 0.002 was fine while the cue was a 66 ms clang at -0.7 dBFS, but with an
    // -18 dB trim and a decaying tail the last tens of milliseconds fall under
    // it, and a perfectly complete cue got reported as "SHORT: a cue ended
    // before its clip did".
    for (std::size_t f = 0; f < frames; ++f)
        span.peak = std::max(span.peak, static_cast<double>(std::fabs(mix[f * channels + channel])));
    const double floorLevel = std::max(span.peak * 0.01, 1.0 / 32768.0);

    std::size_t run = 0;
    bool started = false;
    for (std::size_t f = 0; f < frames; ++f) {
        const double v = std::fabs(mix[f * channels + channel]);
        if (v > floorLevel) {
            if (!started) { span.firstMs = static_cast<double>(f) * msPerFrame; started = true; }
            else if (run > quiet) {
                ++span.gaps;
                span.longestGapMs = std::max(span.longestGapMs, static_cast<double>(run) * msPerFrame);
            }
            span.lastMs = static_cast<double>(f) * msPerFrame;
            run = 0;
        } else if (started) {
            ++run;
        }
    }
    return span;
}

/// Overlap policy check. Phase 1 answers "does one clip play all the way to
/// its own end"; the repeat phases answer "does a new event cut the previous
/// one short". The submitted mix is captured for every phase so the answer
/// comes from the samples, not from an impression.
int OverlapTest(const Options& o) {
    GuardCueConfig config = LoadConfig(o);
    Options opts = o;
    ApplyDeviceDefaults(config, opts);
    ApplyHapticVariant(config, o.hapticVariant);
    ApplySpeakerOverrides(config, o);

    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (opts.audio < 0 || opts.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(opts.audio)].id, o.bufferMs)) {
        std::cout << "open failed: " << device.Error() << "\n";
        return 1;
    }
    ConfigureMixer(device, config, opts.speakerChannel);
    device.StartRenderThread();
    const auto rate = device.Format().sampleRate;
    const auto channels = device.Format().channels;

    std::string deflectSource, blockSource;
    const auto deflectSpeaker = RenderSpeakerCue(config.deflect, rate, config.speakerVolume, deflectSource);
    const auto blockSpeaker = RenderSpeakerCue(config.block, rate, config.speakerVolume, blockSource);
    const auto deflectHaptic = SynthesizeGuardHaptic(config.deflect.haptic, rate, config.hapticStrength);
    const auto blockHaptic = SynthesizeGuardHaptic(config.block.haptic, rate, config.hapticStrength);
    const double deflectSpeakerMs = 1000.0 * static_cast<double>(deflectSpeaker.size()) / rate;
    const double blockSpeakerMs = 1000.0 * static_cast<double>(blockSpeaker.size()) / rate;
    const double deflectHapticMs = 1000.0 * static_cast<double>(deflectHaptic.size()) / rate;

    std::cout << "\nsource lengths -- speaker and haptic end at DIFFERENT times on purpose:\n"
              << "  deflect speaker " << deflectSpeakerMs << " ms   haptic " << deflectHapticMs << " ms\n"
              << "  block   speaker " << blockSpeakerMs << " ms   haptic "
              << 1000.0 * static_cast<double>(blockHaptic.size()) / rate << " ms\n"
              << "voice cap: soft " << device.VoiceLimit() << " / hard " << device.HardVoiceLimit()
              << "  (one event = 1 speaker + 2 haptic voices)\n";

    HidApiDualSenseTransport transport;
    const bool routed = o.speakerRouting && EnableSpeakerRouting(transport, opts.speakerVolume, opts.audioPath);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent") << "\n";

    struct Phase { const char* name; int hits; int gapMs; bool mixed; };
    const Phase phases[] = {
        {"1 single shot -- must play to its own end", 1, 0,   false},
        {"2 repeats 400 ms",                          4, 400, false},
        {"3 repeats 200 ms",                          4, 200, false},
        {"4 repeats 100 ms",                          6, 100, false},
        {"5 mixed deflect/block 150 ms",              6, 150, true},
        // The intervals above are all LONGER than the 71 ms speaker clip, so
        // nothing actually overlaps in them -- they only prove no cue is cut
        // short. These two are shorter than the clip, so voices genuinely sum
        // and the overlap policy is the thing under test.
        {"6 repeats 50 ms -- real overlap",           8,  50, false},
        {"7 mixed 30 ms -- heavy overlap",           10,  30, true},
    };
    for (const auto& phase : phases) {
        device.ResetRenderStats();
        // Enough room for every hit plus the longest tail.
        const auto captureFrames = static_cast<std::size_t>(
            rate * (static_cast<double>(phase.hits) * phase.gapMs + 600.0) / 1000.0);
        device.BeginSubmitCapture(captureFrames);
        std::cout << "\n=== phase " << phase.name << " ===\n";
        for (int i = 0; i < phase.hits; ++i) {
            const bool deflect = phase.mixed ? (i % 3) != 1 : true;
            if (opts.speakerChannel >= 0)
                device.Queue(deflect ? deflectSpeaker : blockSpeaker, opts.speakerChannel, 1.0f);
            device.QueuePair(deflect ? deflectHaptic : blockHaptic,
                             opts.hapticLeft, opts.hapticRight, 1.0f, 0.0f);
            if (i + 1 < phase.hits)
                std::this_thread::sleep_for(std::chrono::milliseconds(phase.gapMs));
        }
        // Let every tail finish on its own rather than tearing down under it.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        device.EndSubmitCapture();
        const auto stats = device.RenderStats();
        const auto& mix = device.SubmitCapture();
        const auto spk = MeasureChannel(mix, channels, static_cast<unsigned>(std::max(0, opts.speakerChannel)), rate);
        const double expectedMs = static_cast<double>(phase.hits - 1) * phase.gapMs + deflectSpeakerMs;
        std::cout << "  submitted mix, speaker channel: first " << spk.firstMs << " ms, last "
                  << spk.lastMs << " ms, peak " << spk.peak << "\n"
                  << "    sounding span " << (spk.lastMs - spk.firstMs) << " ms"
                  << "   expected >= " << expectedMs << " ms"
                  << ((spk.lastMs - spk.firstMs) + 12.0 >= expectedMs
                          ? "   OK: nothing was cut short"
                          : "   SHORT: a cue ended before its clip did")
                  << "\n"
                  << "    mid-sound silent gaps > 5 ms: " << spk.gaps
                  << " (longest " << spk.longestGapMs << " ms)"
                  << (spk.gaps == 0 ? "   -- no starvation in the mix itself" : "")
                  << "\n";
        ReportRenderStats(stats);
        if (!o.dumpMix.empty()) {
            const std::string name = o.dumpMix + "/overlap_phase" +
                                     std::to_string(&phase - phases + 1) + ".wav";
            WriteWavMulti(name, mix, channels, rate);
        }
    }

    device.StopRenderThread();
    device.DropPending();
    device.Close();
    if (routed) ReleaseSpeakerRouting(transport); else transport.Close();
    std::cout << "\nThe spans above are measured on the bytes handed to WASAPI. Whether the\n"
                 "overlap sounds natural is still a listening question.\n";
    return 0;
}

int Play(const Options& o) {
    GuardCueConfig config = LoadConfig(o);
    Options opts = o;
    ApplyDeviceDefaults(config, opts);
    ApplyHapticVariant(config, o.hapticVariant);
    ApplySpeakerOverrides(config, o);

    const auto hits = BuildPattern(o.pattern, std::max(1, o.count));
    if (hits.empty()) { std::cout << "unknown --play pattern\n"; return 2; }

    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (opts.audio < 0 || opts.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(opts.audio)].id, o.bufferMs)) {
        std::cout << "open failed: " << device.Error() << "\n";
        return 1;
    }
    ConfigureMixer(device, config, opts.speakerChannel);
    // The buffer is fed by its own thread from here on. Nothing in the loop
    // below -- console writes, timing arithmetic -- can starve it any more.
    device.StartRenderThread();
    const auto rate = device.Format().sampleRate;

    // Everything is rendered up front: the playback loop never synthesises,
    // opens a file, or touches a device.
    std::string deflectSource, blockSource;
    const auto deflectSpeaker = RenderSpeakerCue(config.deflect, rate, config.speakerVolume, deflectSource);
    const auto blockSpeaker = RenderSpeakerCue(config.block, rate, config.speakerVolume, blockSource);
    std::cout << "  deflect speaker cue: " << deflectSource << "\n"
              << "  block   speaker cue: " << blockSource << "\n"
              << "  haptics are synthesised for both (never copied from the recording)\n";
    const auto deflectHaptic = SynthesizeGuardHaptic(config.deflect.haptic, rate, config.hapticStrength);
    const auto blockHaptic = SynthesizeGuardHaptic(config.block.haptic, rate, config.hapticStrength);

    const bool wantSpeaker = o.mode == "speaker" || o.mode == "both";
    const bool wantHaptic = o.mode == "haptic" || o.mode == "both";
    if (wantSpeaker && opts.speakerChannel < 0) {
        std::cout << "--speaker-channel is required for speaker output (run --channel-test first;\n"
                     "this tool will not guess which channel is the speaker)\n";
        return 2;
    }
    if (wantHaptic && opts.hapticLeft < 0 && opts.hapticRight < 0) {
        std::cout << "--haptic-left / --haptic-right are required for haptic output (run\n"
                     "--channel-test first; this tool will not guess which channel vibrates)\n";
        return 2;
    }

    // Route controller audio to the built-in speaker over HID, not by changing
    // any Windows device setting.
    HidApiDualSenseTransport transport;
    // The output-path byte configures the WHOLE endpoint, not just the speaker.
    // Gating this on "the caller wants speaker output" left haptic-only playback
    // silent even though the same channels vibrated during --channel-test, which
    // did send it. So it is always sent.
    const bool routed = o.speakerRouting && EnableSpeakerRouting(transport, opts.speakerVolume, opts.audioPath);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent")
              << "  (path " << opts.audioPath << ", controller volume 0x" << std::hex
              << (opts.speakerVolume & 0xFF) << std::dec << ")\n";
    std::cout << "pattern=" << o.pattern << " hits=" << hits.size()
              << " interval=" << o.intervalMs << "ms mode=" << o.mode << "\n"
              << "speakerCh=" << opts.speakerChannel << " hapticL=" << opts.hapticLeft
              << " hapticR=" << opts.hapticRight << "\n\n";

    const auto intervalUs = static_cast<std::int64_t>(std::max(1, o.intervalMs)) * 1000;
    const auto start = NowUs();
    std::size_t next = 0;
    std::int64_t droppedLate = 0;
    while (true) {
        const auto now = NowUs();
        if (next < hits.size() && now >= start + static_cast<std::int64_t>(next) * intervalUs) {
            const auto due = start + static_cast<std::int64_t>(next) * intervalUs;
            const auto late = now - due;
            if (late > config.maxOutputLatencyUs) {
                ++droppedLate;   // never flush stale hits out in a burst
                std::cout << "  [" << (now - start) / 1000 << "ms] hit " << next
                          << " dropped: " << late / 1000 << "ms late\n";
            } else {
                const bool deflect = hits[next].deflect;
                // Nothing is ducked here. The previous clang keeps ringing and
                // this one sums on top of it; that overlap is the point.
                const auto& profile = deflect ? config.deflect : config.block;
                if (wantSpeaker)
                    device.Queue(deflect ? deflectSpeaker : blockSpeaker, opts.speakerChannel, 1.0f);
                if (wantHaptic)
                    device.QueuePair(deflect ? deflectHaptic : blockHaptic,
                                     opts.hapticLeft, opts.hapticRight, 1.0f, profile.balance);
                std::cout << "  [" << (now - start) / 1000 << "ms] "
                          << (deflect ? "DEFLECT" : "block  ")
                          << "  late=" << late / 1000 << "ms voices=" << device.ActiveVoices() << "\n";
            }
            ++next;
        }
        if (next >= hits.size() && device.ActiveVoices() == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Let the last tail actually reach the speaker before tearing down.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ReportRenderStats(device.RenderStats());
    // Leave nothing sounding and give the routing back.
    device.StopRenderThread();
    device.DropPending();
    device.Close();
    if (routed) ReleaseSpeakerRouting(transport); else transport.Close();
    if (droppedLate) std::cout << "\ndropped-late hits: " << droppedLate << "\n";
    std::cout << "\ndone. This was manual playback -- it measures nothing about in-game detection.\n";
    return 0;
}


/// Rough energy split so a supplied recording can be described without
/// claiming a real spectral analysis: successive-difference energy rises with
/// high-frequency content, absolute energy covers everything.
void DescribeClip(const std::string& label, const std::vector<float>& clip, std::uint32_t rate) {
    if (clip.empty()) { std::cout << "  " << label << ": empty\n"; return; }
    double energy = 0, diff = 0;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        energy += static_cast<double>(clip[i]) * clip[i];
        if (i) { const double d = clip[i] - clip[i - 1]; diff += d * d; }
    }
    const double rms = std::sqrt(energy / static_cast<double>(clip.size()));
    const double bright = std::sqrt(diff / static_cast<double>(clip.size() > 1 ? clip.size() - 1 : 1));
    // Where most of the energy has already arrived -- a hit sound front-loads.
    double running = 0;
    std::size_t p90 = clip.size();
    for (std::size_t i = 0; i < clip.size(); ++i) {
        running += static_cast<double>(clip[i]) * clip[i];
        if (running >= energy * 0.9) { p90 = i; break; }
    }
    std::cout << "  " << label << ": " << clip.size() << " samples ("
              << (clip.size() * 1000 / rate) << " ms), peak=" << PeakAmplitude(clip)
              << ", rms=" << rms << ", brightness=" << bright
              << ", 90% energy by " << (p90 * 1000 / rate) << " ms\n";
}


int ExtractHit(const Options& o) {
    const std::uint32_t rate = 48'000;
    AudioClipInfo info;
    auto clip = LoadAudioClipMono(o.extractFrom, rate, info);
    if (!info.ok) { std::cout << "decode failed: " << info.error << "\n"; return 1; }
    if (!o.alignOnly) TrimLeadingSilence(clip, 0.005f);

    if (o.alignOnly) {
        const float rawPeak = PeakAmplitude(clip);
        const float rawTrue = OversampledPeak(clip, 4);
        const auto onset = RmsOnset(clip, rate, 0.10f);
        std::cout << o.extractFrom << "\n"
                  << "  decoded      : " << clip.size() << " frames ("
                  << (clip.size() * 1000 / rate) << " ms)\n"
                  << "  sample peak  : " << rawPeak << " ("
                  << 20.0 * std::log10(std::max(rawPeak, 1e-9f)) << " dBFS)\n"
                  << "  4x true peak : " << rawTrue << " ("
                  << 20.0 * std::log10(std::max(rawTrue, 1e-9f)) << " dBFS)"
                  << (rawTrue > 1.0f ? "   <-- above 0 dBFS; blind amplification WOULD clip" : "")
                  << "\n  5ms-RMS onset: " << (onset * 1000 / rate) << " ms\n";
        clip.erase(clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(std::min(onset, clip.size())));
        // Scale so the OVERSAMPLED peak lands on the requested headroom.
        const float target = std::pow(10.0f, std::clamp(o.alignPeakDb, -30.0f, 0.0f) / 20.0f);
        const float truePeak = OversampledPeak(clip, 4);
        if (truePeak > 1e-6f) {
            const float scale = target / truePeak;
            for (float& v : clip) v = std::clamp(v * scale, -1.0f, 1.0f);
        }
        ApplyFadeOut(clip, rate, static_cast<float>(std::max(1, o.fadeMs)));
        std::cout << "  aligned      : " << clip.size() << " frames ("
                  << (clip.size() * 1000 / rate) << " ms), sample peak " << PeakAmplitude(clip)
                  << ", 4x true peak " << OversampledPeak(clip, 4) << "\n"
                  << "  tail preserved; no EQ or compression applied\n";
        if (o.extractOut.empty()) { std::cout << "  (no --out given; nothing written)\n"; return 0; }
        std::filesystem::path out(o.extractOut);
        std::error_code ec;
        if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
        WriteWav(out.string(), clip, rate);
        return 0;
    }

    const auto onsets = FindOnsets(clip, rate, 0.35f, 60.0f);
    std::cout << o.extractFrom << ": " << onsets.size() << " impact(s) at";
    for (auto s : onsets) std::cout << " " << (s * 1000 / rate) << "ms";
    std::cout << "\n";
    if (onsets.empty()) { std::cout << "  no impact found\n"; return 1; }
    if (o.onsetIndex < 0 || o.onsetIndex >= static_cast<int>(onsets.size())) {
        std::cout << "  --onset " << o.onsetIndex << " out of range\n";
        return 2;
    }

    // Start slightly before the detected peak so the very first transient --
    // the part that makes the hit read as sharp -- is not cut off.
    const auto preroll = static_cast<std::size_t>(0.004 * rate);
    const std::size_t peakAt = onsets[static_cast<std::size_t>(o.onsetIndex)];
    const std::size_t begin = peakAt > preroll ? peakAt - preroll : 0;
    std::size_t length = static_cast<std::size_t>(std::max(20, o.hitLengthMs) / 1000.0 * rate);
    // Never run into the next impact: that would put two hits in one cue.
    if (static_cast<std::size_t>(o.onsetIndex) + 1 < onsets.size()) {
        const auto nextAt = onsets[static_cast<std::size_t>(o.onsetIndex) + 1];
        const auto room = nextAt > begin ? nextAt - begin : 0;
        if (room > 0 && length > room) {
            std::cout << "  shortened to " << (room * 1000 / rate)
                      << " ms so the next impact is not included\n";
            length = room;
        }
    }
    length = std::min(length, clip.size() - begin);
    std::vector<float> hit(clip.begin() + static_cast<std::ptrdiff_t>(begin),
                           clip.begin() + static_cast<std::ptrdiff_t>(begin + length));
    ApplyFadeOut(hit, rate, static_cast<float>(std::max(1, o.fadeMs)));
    NormalizePeak(hit, o.extractPeak);
    DescribeClip("extracted", hit, rate);
    if (o.extractOut.empty()) { std::cout << "  (no --out given; nothing written)\n"; return 0; }
    std::filesystem::path out(o.extractOut);
    std::error_code ec;
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
    WriteWav(out.string(), hit, rate);
    return 0;
}

int InspectAudio(const Options& o) {
    const std::uint32_t rate = 48'000;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const std::filesystem::path root(o.inspectAudio);
    if (std::filesystem::is_directory(root, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
            if (entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(root);
    }
    if (files.empty()) { std::cout << "nothing to inspect at " << o.inspectAudio << "\n"; return 2; }
    std::cout << "decoding at " << rate << " Hz mono (Windows Media Foundation)\n\n";
    for (const auto& f : files) {
        AudioClipInfo info;
        auto clip = LoadAudioClipMono(f, rate, info);
        std::cout << f.filename().string() << "\n";
        if (!info.ok) { std::cout << "  FAILED: " << info.error << "\n\n"; continue; }
        std::cout << "  source: " << info.sourceSampleRate << " Hz, "
                  << info.sourceChannels << " ch\n";
        DescribeClip("decoded", clip, rate);
        const auto trimmed = TrimLeadingSilence(clip, 0.005f);
        if (trimmed) {
            std::cout << "  leading silence trimmed: " << (trimmed * 1000 / rate) << " ms\n";
            DescribeClip("trimmed ", clip, rate);
        }
        if (!o.decodeTo.empty()) {
            std::error_code wec;
            std::filesystem::create_directories(o.decodeTo, wec);
            WriteWav(o.decodeTo + "/" + f.stem().string() + "_decoded.wav", clip, rate);
        }
        std::cout << "\n";
    }
    std::cout << "A recording is used for the SPEAKER cue only. The haptic waveform stays\n"
                 "synthesised: a kHz metal clang is not something a voice coil reproduces as\n"
                 "an impact, so copying it to the actuators would give a thin buzz.\n";
    return 0;
}

int DumpCues(const Options& o) {
    GuardCueConfig config = DefaultGuardCueConfig();
    if (std::ifstream in(o.config); in) {
        std::stringstream b; b << in.rdbuf();
        std::string error;
        if (!ParseGuardCueConfig(b.str(), config, error)) { std::cout << "config error: " << error << "\n"; return 2; }
    }
    ApplyHapticVariant(config, o.hapticVariant);
    ApplySpeakerOverrides(config, o);
    const std::uint32_t rate = 48'000;
    struct Row { const char* name; std::vector<float> clip; };
    std::vector<Row> rows = {
        {"deflect.speaker", SynthesizeGuardSpeaker(config.deflect.speaker, rate, config.speakerVolume)},
        {"block.speaker", SynthesizeGuardSpeaker(config.block.speaker, rate, config.speakerVolume)},
        {"deflect.haptic", SynthesizeGuardHaptic(config.deflect.haptic, rate, config.hapticStrength)},
        {"block.haptic", SynthesizeGuardHaptic(config.block.haptic, rate, config.hapticStrength)},
    };
    std::cout << "rendered at " << rate << " Hz\n";
    for (const auto& r : rows) {
        std::cout << "  " << r.name << ": " << r.clip.size() << " samples ("
                  << (r.clip.size() * 1000 / rate) << " ms), peak=" << PeakAmplitude(r.clip)
                  << ", rms=" << Rms(r.clip) << "\n";
    }
    if (!o.writeWav.empty())
        for (const auto& r : rows) WriteWav(o.writeWav + "/" + r.name + ".wav", r.clip, rate);
    return 0;
}


// ---------------------------------------------------------------------------
// Live: read the game, turn pulses into events, play the matching cue.
// ---------------------------------------------------------------------------

namespace live_detail {

using namespace sekiro_haptics::process;

/// The WorldChrMan anchor, same AOB the probe already uses.
KnownRootSpec MakeWorldChrManSpec(const std::string& moduleName) {
    KnownRootSpec spec;
    spec.rootId = "WorldChrMan";
    spec.moduleName = moduleName;
    (void)ParseAobPattern("48 8B 35 ?? ?? ?? ?? 44 0F 28 18", spec.pattern);
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    return spec;
}

/// The one build these offsets were verified against.
ExecutableIdentity KnownGoodIdentity() {
    ExecutableIdentity id;
    id.fileSizeBytes = 68005144;
    const char* hex = "637aca527538c0ec6e1f136c8ed66046e95dfbdbb1f51926e134d9916398b856";
    auto nibble = [](char c) -> std::uint8_t {
        return static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (std::size_t i = 0; i < id.sha256.bytes.size(); ++i)
        id.sha256.bytes[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    return id;
}

} // namespace live_detail

int Live(const Options& o) {
    using namespace sekiro_haptics::process;
    if (o.pid <= 0) { std::cout << "--live requires --pid N\n"; return 2; }

    GuardCueConfig config = LoadConfig(o);
    Options opts = o;
    ApplyDeviceDefaults(config, opts);
    ApplyHapticVariant(config, o.hapticVariant);
    ApplySpeakerOverrides(config, o);

    // ---- attach read-only ------------------------------------------------
    Win32ProcessReader reader;
    if (reader.AttachByPid(static_cast<std::uint32_t>(o.pid)) != ProcessReaderResult::Success) {
        std::cout << "could not attach to pid " << o.pid << " (is sekiro.exe running?)\n";
        return 1;
    }
    ExecutableIdentity identity;
    if (BuildExecutableIdentity(reader, identity) != ProcessInspectionResult::Success) {
        std::cout << "could not build the executable identity\n";
        return 1;
    }
    ModuleInfo mainModule;
    if (reader.GetMainModule(mainModule) != ProcessInspectionResult::Success) {
        std::cout << "could not identify the main module\n";
        return 1;
    }
    std::cout << "attached to pid " << reader.Pid() << ", " << mainModule.name
              << " base=0x" << std::hex << mainModule.baseAddress << std::dec
              << ", sha256=" << ToHex(identity.sha256).substr(0, 16) << "...\n";

    SekiroPlayerGuardReader guardReader(reader, reader,
                                        live_detail::MakeWorldChrManSpec(mainModule.name),
                                        live_detail::KnownGoodIdentity(), identity,
                                        mainModule.baseAddress);
    const auto primed = guardReader.Prime();
    std::cout << "WorldChrMan resolve: " << ToString(primed) << "\n";
    if (primed == RootResolveResult::UnsupportedBuild) {
        std::cout << "  this build is not the one the offsets were verified against -- refusing\n";
        return 1;
    }

    // Enemy-side reader. Same two fields, same ownership test mirrored onto
    // SprjEnemyDamageModule. Built only when asked for -- discovery walks the
    // character graph and costs real time.
    std::optional<SekiroEnemyGuardReader> enemyReader;
    if (o.enemy) {
        enemyReader.emplace(reader, reader, live_detail::MakeWorldChrManSpec(mainModule.name),
                            live_detail::KnownGoodIdentity(), identity, mainModule.baseAddress);
        enemyReader->Prime();
        const auto tracked = enemyReader->Discover();
        std::cout << "enemy reader: tracking " << tracked << " character(s)"
                  << " (discovery took " << enemyReader->Stats().lastDiscoveryUs / 1000 << " ms)\n";
        // From here the walk repeats on its own thread; the detection loop
        // never waits for it, so enemies that load in later still get tracked.
        enemyReader->StartBackgroundDiscovery(o.enemyRediscoverSeconds);
        std::cout << "  rediscovering every " << o.enemyRediscoverSeconds
                  << " s on a background thread\n";
        if (tracked == 0)
            std::cout << "  none found -- stand near enemies and they will be picked up on the\n"
                         "  next rediscovery pass\n";
    }

    // ---- output ----------------------------------------------------------
    const auto endpoints = DualSenseAudioDevice::Enumerate();
    if (opts.audio < 0 || opts.audio >= static_cast<int>(endpoints.size())) {
        std::cout << "--audio index out of range; run --list-devices\n";
        return 2;
    }
    DualSenseAudioDevice device;
    if (!device.Open(endpoints[static_cast<std::size_t>(opts.audio)].id, o.bufferMs)) {
        std::cout << "audio open failed: " << device.Error() << "\n";
        return 1;
    }
    ConfigureMixer(device, config, opts.speakerChannel);
    // THE fix for the broken-up sound. The detection loop below calls
    // ReadProcessMemory in a graph walk that can run for tens of milliseconds
    // and writes to the console on every event; both used to sit between two
    // Pump() calls, so the feed gap regularly exceeded the whole buffer and
    // the endpoint played out whatever was left. Audio now has its own
    // time-critical thread and shares nothing with detection but a mutex.
    device.StartRenderThread();
    {
        const auto rs = device.RenderStats();
        std::cout << "  audio buffer: " << rs.bufferFrames << " frames ("
                  << rs.bufferMs << " ms) -- also the worst-case added latency\n";
    }
    const auto rate = device.Format().sampleRate;
    // Rendered once, before the loop. The polling loop never synthesises,
    // opens a file, or touches a device.
    std::string deflectSource, blockSource;
    const auto deflectSpeaker = RenderSpeakerCue(config.deflect, rate, config.speakerVolume, deflectSource);
    const auto blockSpeaker = RenderSpeakerCue(config.block, rate, config.speakerVolume, blockSource);
    std::cout << "  deflect speaker cue: " << deflectSource << "\n"
              << "  block   speaker cue: " << blockSource << "\n"
              << "  haptics are synthesised for both (never copied from the recording)\n";
    const auto deflectHaptic = SynthesizeGuardHaptic(config.deflect.haptic, rate, config.hapticStrength);
    const auto blockHaptic = SynthesizeGuardHaptic(config.block.haptic, rate, config.hapticStrength);

    const bool wantSpeaker = o.mode == "speaker" || o.mode == "both";
    const bool wantHaptic = o.mode == "haptic" || o.mode == "both";
    if (wantSpeaker && opts.speakerChannel < 0) { std::cout << "--speaker-channel required\n"; return 2; }
    if (wantHaptic && opts.hapticLeft < 0 && opts.hapticRight < 0) { std::cout << "--haptic-left/right required\n"; return 2; }

    HidApiDualSenseTransport transport;
    // The output-path byte configures the WHOLE endpoint, not just the speaker.
    // Gating this on "the caller wants speaker output" left haptic-only playback
    // silent even though the same channels vibrated during --channel-test, which
    // did send it. So it is always sent.
    const bool routed = o.speakerRouting && EnableSpeakerRouting(transport, opts.speakerVolume, opts.audioPath);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent")
              << "  (path " << opts.audioPath << ", controller volume 0x" << std::hex
              << (opts.speakerVolume & 0xFF) << std::dec << ")\n";
    // ---- detection -------------------------------------------------------
    GuardDetectorConfig detectorConfig;
    detectorConfig.mode = OccurrenceMode::Pulse;   // +0x3C is a one-frame pulse
    GuardOutcomeEventDetector detector(detectorConfig);
    // One detector per enemy: the pulse is per character, so a shared detector
    // would interleave two fights into one nonsense sequence.
    std::map<std::uintptr_t, GuardOutcomeEventDetector> enemyDetectors;
    std::uint64_t enemyDeflects = 0, enemyBlocks = 0;
    std::int64_t nextRediscoverUs = 0;

    // Windows' default timer granularity is ~15.6 ms, so a 5 ms sleep loop
    // actually runs at ~15 ms and would miss a one-frame pulse outright.
    // Ask for 1 ms for the duration of the session and hand it back after.
    timeBeginPeriod(1);
    std::cout << "\npolling at ~5 ms (timer resolution raised to 1 ms). Ctrl+C to stop.\n\n";
    const auto started = NowUs();
    const std::int64_t targetIntervalUs = 5'000;
    std::int64_t nextPollUs = started;
    std::int64_t worstIntervalUs = 0, lastPollUs = started, intervalSum = 0, intervalCount = 0;
    std::int64_t missedSlots = 0, lateDrops = 0;
    std::uint64_t deflects = 0, blocks = 0, unresolved = 0;
    GuardReadStatus lastStatus = GuardReadStatus::Ok;
    bool reportedStatus = false;

    while (o.liveSeconds <= 0 || NowUs() - started < static_cast<std::int64_t>(o.liveSeconds) * 1'000'000) {
        const auto now = NowUs();
        if (now < nextPollUs) {
            // No Pump() here. The render thread owns the buffer now; this call
            // was left over from when the detection loop fed it directly, and
            // between the two of them the buffer was being polled about six
            // million times a second.
            //
            // ::Sleep, not std::this_thread::sleep_for. Under MinGW's
            // winpthreads sleep_for does not honour timeBeginPeriod at all:
            // 500 us returned instantly (hence the spin) and 1 ms became a
            // full 15.6 ms tick, which pushed the 5 ms poll grid to a measured
            // mean of 15.6 ms. Win32 Sleep does honour it.
            ::Sleep(1);
            continue;
        }
        // Keep the 5 ms grid; count slots we could not service instead of
        // silently stretching the period.
        const auto behind = now - nextPollUs;
        if (behind > targetIntervalUs) {
            missedSlots += behind / targetIntervalUs;
            nextPollUs = now;
        }
        nextPollUs += targetIntervalUs;

        const auto interval = now - lastPollUs;
        lastPollUs = now;
        intervalSum += interval;
        ++intervalCount;
        worstIntervalUs = std::max(worstIntervalUs, interval);

        const auto sample = guardReader.Poll();
        if (sample.status != lastStatus || !reportedStatus) {
            if (sample.status != GuardReadStatus::Ok)
                std::cout << "  [" << (now - started) / 1000 << "ms] reader: " << ToString(sample.status) << "\n";
            else if (reportedStatus)
                std::cout << "  [" << (now - started) / 1000 << "ms] reader: Ok (module 0x"
                          << std::hex << sample.moduleAddress << std::dec << ")\n";
            lastStatus = sample.status;
            reportedStatus = true;
        }

        GuardObservation obs;
        obs.timestampUs = now;
        obs.readOk = sample.Ok();
        obs.generation = sample.generation;
        obs.continuityBreak = interval > targetIntervalUs * 4;   // we did not observe that window
        obs.occurrence = sample.pulse;
        obs.outcome = sample.outcome;

        for (const auto& event : detector.Update(obs)) {
            const auto age = NowUs() - event.timestampUs;
            if (age > config.maxOutputLatencyUs) {
                ++lateDrops;      // never flush a stale hit out later
                std::cout << "  [" << (now - started) / 1000 << "ms] dropped (" << age / 1000 << "ms late)\n";
                continue;
            }
            const bool deflect = event.kind == GuardEventKind::Deflect;
            if (event.kind == GuardEventKind::Unresolved) {
                ++unresolved;
                std::cout << "  [" << (now - started) / 1000 << "ms] unresolved (no cue played)\n";
                continue;
            }
            // The previous cue is left alone: a normal new event never stops,
            // ducks or fades what is already sounding. Only the voice cap can
            // shorten a tail, and it fades the quietest one instead of cutting.
            const auto& profile = deflect ? config.deflect : config.block;
            if (wantSpeaker) device.Queue(deflect ? deflectSpeaker : blockSpeaker, opts.speakerChannel, 1.0f);
            if (wantHaptic) device.QueuePair(deflect ? deflectHaptic : blockHaptic,
                                             opts.hapticLeft, opts.hapticRight, 1.0f, profile.balance);
            (deflect ? deflects : blocks)++;
            std::cout << "  [" << (now - started) / 1000 << "ms] " << (deflect ? "DEFLECT" : "block  ")
                      << "  latency=" << age / 1000 << "ms voices=" << device.ActiveVoices() << "\n";
        }

        if (enemyReader) {
            // Rediscovery does NOT happen here. Two reasons, both measured:
            // the walk takes ~5.7 s, and gating it on NeedsDiscovery() froze
            // the set found at startup for as long as you stayed in one area
            // (NeedsDiscovery() only reports an area change or a death), so
            // enemies that loaded in later were never tracked -- that is what
            // made detection work on some enemies and not others. Running it
            // here instead pushed the poll interval from 5 ms to 4.2 SECONDS.
            // It now runs on its own thread and hands over finished sets.
            for (const auto& es : enemyReader->Poll()) {
                GuardObservation eo;
                eo.timestampUs = now;
                eo.readOk = true;
                eo.generation = es.generation;
                eo.continuityBreak = interval > targetIntervalUs * 4;
                eo.occurrence = es.pulse;
                eo.outcome = es.outcome;
                auto& det = enemyDetectors.try_emplace(es.character, detectorConfig).first->second;
                for (const auto& event : det.Update(eo)) {
                    if (event.kind == GuardEventKind::Unresolved) continue;
                    if (NowUs() - event.timestampUs > config.maxOutputLatencyUs) { ++lateDrops; continue; }
                    const bool deflect = event.kind == GuardEventKind::Deflect;
                    const auto& profile = deflect ? config.deflect : config.block;
                    if (wantSpeaker)
                        device.Queue(deflect ? deflectSpeaker : blockSpeaker, opts.speakerChannel, 1.0f);
                    if (wantHaptic)
                        device.QueuePair(deflect ? deflectHaptic : blockHaptic,
                                         opts.hapticLeft, opts.hapticRight, 1.0f, profile.balance);
                    (deflect ? enemyDeflects : enemyBlocks)++;
                    std::cout << "  [" << (now - started) / 1000 << "ms] ENEMY "
                              << (deflect ? "DEFLECT" : "block  ") << "  0x" << std::hex
                              << es.character << std::dec << "\n";
                }
            }
        }
    }

    if (enemyReader) enemyReader->StopBackgroundDiscovery();
    for (const auto& event : detector.Finish()) { (void)event; ++unresolved; }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto renderStats = device.RenderStats();
    device.StopRenderThread();
    device.DropPending();
    device.Close();
    if (routed) ReleaseSpeakerRouting(transport); else transport.Close();

    timeEndPeriod(1);
    const auto& rs = guardReader.Stats();
    std::cout << "\n--- session ---\n"
              << "  polls=" << rs.polls << " ok=" << rs.ok << " failed=" << rs.failed << "\n"
              << "  moduleSearches=" << rs.moduleSearches
              << " playerInsChanges=" << rs.playerInsChanges
              << " moduleChanges=" << rs.moduleChanges
              << " moduleVote=" << rs.lastVoteCount << "/" << rs.lastVoteCandidates << "\n"
              << "  poll interval: mean="
              << (intervalCount ? intervalSum / intervalCount : 0) << "us worst=" << worstIntervalUs
              << "us missedSlots=" << missedSlots << "\n"
              << "  events: deflect=" << deflects << " block=" << blocks
              << " unresolved=" << unresolved << " droppedLate=" << lateDrops << "\n";
    if (enemyReader) {
        const auto& es = enemyReader->Stats();
        std::cout << "  enemy: deflect=" << enemyDeflects << " block=" << enemyBlocks
                  << "  tracked=" << es.charactersTracked
                  << " discoveries=" << es.discoveries
                  << " dropped=" << es.charactersDropped << "\n"
                  << "    path: ChrIns+0x" << std::hex << es.pathCharacterOffset
                  << " -> container+0x" << es.pathContainerOffset << std::dec
                  << " (support " << es.pathSupport << ")"
                  << "  rawPulseEdges=" << es.rawPulseEdges
                  << " nonZeroSamples=" << es.nonZeroPulseSamples << "\n"
                  << "    pathChanges=" << es.pathChanges
                  << "  ambiguousDropped=" << es.ambiguousModules
                  << "  lastDiscovery=" << es.lastDiscoveryUs / 1000 << " ms\n"
                  << "    An enemy event means THAT CHARACTER guarded. It does not prove\n"
                  << "    it guarded your attack.\n";
    }
    ReportRenderStats(renderStats);
    std::cout << "\n  These are detector outputs, not verified ground truth.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--help" || a == "-h") { Help(); return 0; }
        else if (a == "--list-devices") o.list = true;
        else if (a == "--probe-format") o.probe = true;
        else if (a == "--channel-test") o.channelTest = true;
        else if (a == "--dump-cues") o.dumpCues = true;
        else if (a == "--no-speaker-routing") o.speakerRouting = false;
        else if (a == "--audio") o.audio = std::atoi(next().c_str());
        else if (a == "--channel") o.channel = std::atoi(next().c_str());
        else if (a == "--speaker-channel") o.speakerChannel = std::atoi(next().c_str());
        else if (a == "--haptic-left") o.hapticLeft = std::atoi(next().c_str());
        else if (a == "--haptic-right") o.hapticRight = std::atoi(next().c_str());
        else if (a == "--count") o.count = std::atoi(next().c_str());
        else if (a == "--interval-ms") o.intervalMs = std::atoi(next().c_str());
        else if (a == "--play") o.pattern = next();
        else if (a == "--mode") o.mode = next();
        else if (a == "--config") o.config = next();
        else if (a == "--write-wav") { o.writeWav = next(); o.dumpCues = true; }
        else if (a == "--volume") o.volume = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--strength") o.strength = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--live") o.live = true;
        else if (a == "--pid") o.pid = std::atoi(next().c_str());
        else if (a == "--live-seconds") o.liveSeconds = std::atoi(next().c_str());
        else if (a == "--speaker-volume") { o.speakerVolume = std::atoi(next().c_str()); o.speakerVolumeGiven = true; }
        else if (a == "--probe-gain") o.probeGain = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--endpoint-volume") o.endpointVolume = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--inspect-audio") o.inspectAudio = next();
        else if (a == "--decode-to") o.decodeTo = next();
        else if (a == "--extract-hit") o.extractFrom = next();
        else if (a == "--out") o.extractOut = next();
        else if (a == "--onset") o.onsetIndex = std::atoi(next().c_str());
        else if (a == "--hit-length-ms") o.hitLengthMs = std::atoi(next().c_str());
        else if (a == "--fade-ms") o.fadeMs = std::atoi(next().c_str());
        else if (a == "--extract-peak") o.extractPeak = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--align-only") o.alignOnly = true;
        else if (a == "--align-peak-db") o.alignPeakDb = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--probe-hz") o.probeHz = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--probe-seconds") o.probeSeconds = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--audio-path") { o.audioPath = std::atoi(next().c_str()); o.audioPathGiven = true; }
        else if (a == "--path-sweep") o.pathSweep = true;
        else if (a == "--speaker-diag") o.speakerDiag = true;
        else if (a == "--pre-gain") { o.preGain = std::atoi(next().c_str()); o.preGainGiven = true; }
        else if (a == "--valid0") o.validFlag0 = std::atoi(next().c_str());
        else if (a == "--valid1") o.validFlag1 = std::atoi(next().c_str());
        else if (a == "--dump-submit") o.dumpSubmit = next();
        else if (a == "--write-config") o.writeConfig = next();
        else if (a == "--hid-info") o.hidInfo = true;
        else if (a == "--haptic-variant") { const auto v = next(); o.hapticVariant = v.empty() ? '-' : static_cast<char>(std::tolower(v[0])); }
        else if (a == "--speaker-trim-db") { o.speakerTrimDb = static_cast<float>(std::atof(next().c_str())); o.speakerTrimGiven = true; }
        else if (a == "--speaker-highpass") o.speakerHighpass = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--speaker-lowpass") o.speakerLowpass = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--audio-buffer-ms") o.bufferMs = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--duck-fade-ms") o.duckFadeMs = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--audio-diag") o.audioDiag = true;
        else if (a == "--overlap-test") o.overlapTest = true;
        else if (a == "--enemy") o.enemy = true;
        else if (a == "--enemy-rediscover") o.enemyRediscoverSeconds = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--dump-mix") o.dumpMix = next();
        else if (a == "--report-length") o.forceLength = std::atoi(next().c_str());
        else { std::cout << "unknown option: " << a << "\n"; Help(); return 2; }
    }
    if (o.list) return ListDevices();
    if (o.audioDiag) return AudioDiag(o);
    if (o.overlapTest) return OverlapTest(o);
    if (o.probe) return ProbeFormat(o);
    if (o.hidInfo) return HidInfo(o);
    if (o.speakerDiag) return SpeakerDiag(o);
    if (o.pathSweep) return PathSweep(o);
    if (o.channelTest) return ChannelTest(o);
    if (!o.writeConfig.empty()) {
        // Serialise from the live struct so the file can never drift from the
        // schema the code actually parses.
        const auto config = LoadConfig(o);
        std::ofstream out(o.writeConfig);
        if (!out) { std::cout << "could not write " << o.writeConfig << "\n"; return 1; }
        out << SerializeGuardCueConfig(config);
        std::cout << "wrote " << o.writeConfig << "\n";
        return 0;
    }
    if (!o.extractFrom.empty()) return ExtractHit(o);
    if (!o.inspectAudio.empty()) return InspectAudio(o);
    if (o.dumpCues) return DumpCues(o);
    if (o.live) return Live(o);
    if (!o.pattern.empty()) return Play(o);
    Help();
    return 0;
}
