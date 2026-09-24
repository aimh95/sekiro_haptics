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
#include "sekiro_haptics/process/SekiroProstheticReader.hpp"
#include "sekiro_haptics/process/SekiroWireActionReader.hpp"
#include "sekiro_haptics/process/SekiroProstheticActionReader.hpp"
#include "sekiro_haptics/ProstheticTriggerPolicy.hpp"
#include "sekiro_haptics/WirePrepPolicy.hpp"
#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"
#include "sekiro_haptics/presets/MappingRepository.hpp"
#include "sekiro_haptics/presets/OutputPresetRepository.hpp"
#include "sekiro_haptics/runtime/DualSenseAudioPcmSink.hpp"
#include "sekiro_haptics/runtime/OutputRuntime.hpp"

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
  --enemy              also cue when an ENEMY blocks/deflects (ON BY DEFAULT).
                       The signal says "this character guarded", NOT "it
                       guarded my attack"
  --no-enemy           turn enemy reactions off
  --prosthetic-cue-dir DIR
                       tool_switch.wav plays when you CHANGE prosthetic.
                       shuriken.wav / spear.wav / axe.wav play on the ATTACK
                       code of that tool (not its ready code, not on R2).
  --no-blade-pcm       use the SYNTHESISED deflect/block haptics instead of
                       the authored blade waveforms (the A/B; the authored
                       ones are on by default)
  --blade-pcm-dir DIR  where blade_deflect.wav / blade_block.wav live
  --blade-gain F       overdrive into the ceiling below (default 80; the
                       curve flattens there -- doubling again buys ~10%)
  --blade-ceiling F    where the overdrive folds down, under the mixer's 0.95
                       limiter threshold so a single cue never trips it (0.94)
  --blade-tail-ms F    ring added from the clip's own late material (320)
  --blade-tail-decay F how fast that ring dies; lower is longer (5)
  --no-prosthetic-trigger
                       turn off the R2 preparation resistance
  --no-wire            turn off wire-action detection entirely
  --no-wire-haptic     no PCM thump at the start of a wire action
  --no-wire-trigger    no L2 resistance during a wire action
  --wire-start N --wire-end N --wire-strength N
                       L2 resistance while a wire target is AVAILABLE, as a
                       weapon-style catch: start zone 2..7, end zone
                       start+1..8, strength 0..8 (default 2 / 4 / 4). It is
                       already on when you can grapple, and pushing past the
                       end zone releases it completely.
                       Zones are positions along trigger travel and strength is
                       the firmware's 1..8 scale -- neither is a percentage.
  --wire-ms N          older timings, for comparison: N>0 applies the
                       resistance AT the launch for N ms; 0 holds it for the
                       whole flight (resistance arrives mid-flight).
  --shuriken-start N --shuriken-end N --shuriken-strength N
                       Loaded Shuriken (70000) catch: zones 2..7 / start+1..8,
                       strength 0..8  (default 3 / 4 / 4)
  --spear-start N --spear-strength N
                       Loaded Spear (78000) constant resistance: zone 0..9,
                       strength 0..8  (default 2 / 3)
                       Zones are positions along trigger travel, strength is
                       the firmware's 1..8 scale -- neither is a percentage.
  --enemy-rediscover N seconds between rebuilds of the tracked enemy set (6)
  --trigger-demo       ALSO drive the adaptive triggers from the same events:
                       deflect -> R2 weapon (zones 3..7, strength 8) for 120 ms,
                       block -> L2 feedback (zone 2, strength 5) for 180 ms.
                       DEMO VALUES. Nothing measured says these are right, no
                       game button is remapped, and it is off unless asked for.
                       Works with --play as well as --live. See docs/12.
  --overlap-test       single shot, then 400/200/100 ms repeats and a mixed run;
                       measures the submitted mix for cut-short cues and gaps
  --write-cue DIR      write the FINISHED speaker cues (what --live actually plays,
                       recording + rebuilt ring + shaping) as 48k mono WAV
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
    /// Where to write the finished speaker cues (see WriteRenderedCues).
    std::string writeCue;
    /// Also fire a cue when an ENEMY blocks or deflects.
    ///
    /// ON by default since 2026-09-20, at the user's request: the canonical
    /// run is `--live --pid <pid>` and enemy reactions are wanted there.
    /// `--no-enemy` turns it off.
    ///
    /// The signal itself still only says "this character guarded" -- NOT "it
    /// guarded MY attack" (docs/astra/results/DEFLECT_STATUS.md 8.3). Which
    /// reactions are adopted as player-melee reactions is decided separately;
    /// see EnemyCausePolicy below.
    bool enemy = true;
    /// Attach a DEMO adaptive-trigger layer to the deflect/block presets.
    ///
    /// Off by default and deliberately so: nothing about Sekiro's L1 guard
    /// says a trigger resistance belongs on L2/R2, no game button is
    /// remapped, and no measurement says these parameters are right. This
    /// exists to let trigger output be felt alongside the real PCM cues.
    bool triggerDemo = false;
    /// Where per-prosthetic selection cues live: shuriken.wav / spear.wav,
    /// 48 kHz stereo float32, as the haptic workbench exports them.
    ///
    /// Empty means the feature is off, which is the default -- this fires on a
    /// SELECTION CHANGE, and somebody who has not asked for that should not
    /// suddenly feel their controller when they cycle tools.
    std::string prostheticCueDir;
    /// Use the AUTHORED blade waveforms for deflect/block instead of the
    /// synthesised ones.
    ///
    /// On by default so the ordinary run command exercises them without
    /// growing an option; `--no-blade-pcm` goes back to synthesis, which is
    /// the A/B. The pack marks itself `autoLiveBinding: false` and
    /// `design_candidate_not_hardware_validated`, so this default is a
    /// decision to make the comparison reachable, NOT a claim that the
    /// waveforms have been felt and judged.
    ///
    /// This swaps a WAVEFORM. It adds no event, relaxes no filter and
    /// changes nothing about which reactions reach the output.
    bool bladePcm = true;
    /// Where the pack's wav/ directory is, relative to the working directory
    /// like every other clip path here.
    std::string bladePcmDir = "docs/astra/assets/sekiro_pcm_v1/wav";
    /// One multiplier on the sample values, applied ONCE -- the preset layer
    /// stays at 1.0 and the clips are not normalised, so the whole gain chain
    /// is this number and `bladeCeiling`.
    ///
    /// The pack proposed 0.6, which measured at RMS 0.060 against the
    /// synthesised haptic's 0.191 -- under a third of the energy, because
    /// these clips are very peaky (crest factor ~6.8) and matching their PEAK
    /// still leaves the body of the waveform far below.
    ///
    /// 80 is deliberate, heavy overdrive. Everything above the ceiling is
    /// folded down by `bladeCeiling`, so this number does not set the level
    /// -- it sets how MUCH of the waveform gets pushed up against that
    /// ceiling, which is what a hand feels on a peaky waveform like this one.
    ///
    /// Measured RMS for blade_deflect at ceiling 0.94, after the 320 ms ring:
    ///
    ///     gain     20     40     80    160    400
    ///     rms   0.275  0.335  0.387  0.427  0.459
    ///
    /// 80 is where that curve flattens -- doubling again buys 10%. The clip
    /// is close to a square wave by then, so it is harsh by construction;
    /// that is the trade this default is making on purpose.
    float bladeGain = 80.0f;
    /// Where the overdrive is folded down, applied to the CLIP at load.
    ///
    /// This is the whole reason the gain above does anything. The mixer's
    /// limiter holds a channel at 0.95 with a 120 ms release, so simply
    /// raising the gain made the limiter hand the level straight back: RMS
    /// went 0.223 at gain 4 to only 0.286 at gain 12, for three times the
    /// number. Saturating here instead, just BELOW that threshold, means a
    /// single cue never trips the limiter at all, and the limiter goes back
    /// to doing its actual job -- catching sums during overlap.
    ///
    /// The fold is a tanh knee, not a hard cut: a square corner on a voice
    /// coil is a click, and these waveforms are driven far enough that a
    /// large part of them sits on the knee.
    float bladeCeiling = 0.94f;
    /// Milliseconds of ring added to each blade clip, built from that clip's
    /// OWN late material by ExtendDecayTail -- the same granular tail the
    /// speaker cues use, not a loop and not silence.
    ///
    /// The authored clips are 120 ms and 165 ms against the synthesised
    /// haptics' 378 ms and 500 ms. Gain cannot close that: at equal RMS a
    /// clip a third as long delivers a third of the energy. So the length is
    /// closed here instead, and the tail is added BEFORE the gain so it is
    /// driven into the ceiling like everything else.
    float bladeTailMs = 320.0f;
    /// How fast that ring dies away. Lower is longer; the speaker cues use 11.
    float bladeTailDecayPerSecond = 5.0f;
    /// Seconds between rebuilds of the tracked enemy set.
    ///
    /// Was 6 s, which made detection "not work at first and then suddenly
    /// start": the set and the offset path are derived at startup, when the
    /// right characters may not be loaded yet, and only a later pass fixes
    /// them. The walk now costs ~134 ms, so waiting 6 s bought nothing.
    float enemyRediscoverSeconds = 2.0f;
    /// R2 PREPARATION resistance chosen by the currently selected prosthetic.
    /// On by default; it only ever acts on two exactly-matched equip ids.
    bool prosthetic = true;
    /// Log wire-action start/end. Detection only -- no output is attached to
    /// it yet, on purpose: the button mapping and what a wire should feel like
    /// have not been decided.
    bool wire = true;
    /// A short PCM thump on the grip the moment a wire action starts. This is
    /// the part that gets felt regardless of where the fingers are.
    bool wireHaptic = true;
    /// Constant L2 resistance held for the duration of the wire action.
    ///
    /// L2 and not R2 on purpose: R2 already carries the prosthetic preparation
    /// resistance, and two owners fighting over one trigger would need a
    /// hand-back rule for something nobody asked for. This also means a wire
    /// action never disturbs the prosthetic resistance.
    bool wireTrigger = true;
    /// Zone the L2 resistance starts at (0..9) and its 1..8 strength. First
    /// prototype values, adjustable -- not measured optima.
    /// Weapon mode, like the shuriken preparation: the trigger resists
    /// between these two zones and then LETS GO completely once the finger
    /// pushes past the end. That release is the "fired" sensation.
    ///
    /// Feedback mode was tried first and is wrong for this: it resists all the
    /// way to the bottom of travel, so there is no moment where it gives way.
    std::uint8_t wireTriggerZone = 2;      // start zone (2..7)
    /// End zone (start+1..8). ONE zone wide by default: the span is where the
    /// resistance lives, so a wide span keeps pushing back while the finger is
    /// still travelling through it. A single zone gives a bump that is over
    /// the instant it is crossed, which is the "click then nothing" the
    /// sensation calls for. 2->4 was tried first and left residual resistance.
    std::uint8_t wireTriggerEndZone = 3;
    std::uint8_t wireTriggerStrength = 4;
    /// How the L2 resistance is timed.
    ///
    /// The wanted sensation is the one the shuriken preparation already has:
    /// the resistance is ALREADY THERE while a wire target is available, and
    /// it LETS GO when the hook fires. So it follows the target-available
    /// state, exactly like the prosthetic preparation follows the selected
    /// tool -- it is not started by the launch.
    ///
    /// Two earlier versions got this wrong and are kept reachable for
    /// comparison, because the difference is only decidable by hand:
    ///   --wire-ms N (>0)  apply at launch for N ms  (catch on launch)
    ///   --wire-ms 0       apply at launch, hold for the whole flight
    ///                     (the inverted feel: resistance ARRIVES mid-flight)
    /// Without --wire-ms, the resistance tracks target availability.
    int wireTriggerMs = -1;
    ProstheticTriggerSettings prostheticTrigger;
    /// How often the selection is read. Selection is a slow, human-scale
    /// state -- reading it on the 5 ms guard grid would cost pointer walks for
    /// no benefit, and a resistance applied 100 ms after a menu closes is not
    /// something a hand can notice.
    int prostheticPollMs = 100;
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

/// Claims the audio section on the SHARED output state and transmits it.
///
/// Byte for byte this is the report EnableSpeakerRouting already sent -- the
/// full audio section (headphone + speaker volume, output path, pre-gain,
/// with valid_flag0 0xF0 and valid_flag1 0x80), which is the report this
/// project has actually verified on the hardware. What changes is OWNERSHIP:
/// the claim now lives in the same state the triggers are written from, so
/// every later trigger report re-asserts it instead of depending on the
/// firmware remembering what a separate earlier report said.
bool ClaimSpeakerRouting(dualsense::DualSenseOutputState& state,
                         HidApiDualSenseTransport& transport, const Options& o) {
    const auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) return false;
    if (!transport.IsOpen() && transport.Open(candidates.front().path) != TransportResult::Success)
        return false;
    dualsense::AudioOutputSettings audio;
    audio.speakerVolume = static_cast<std::uint8_t>(std::clamp(o.speakerVolume, 0, 255));
    audio.headphoneVolume = audio.speakerVolume;   // as EnableSpeakerRouting did
    audio.outputPath = static_cast<std::uint8_t>(std::clamp(o.audioPath, 0, 3));
    audio.speakerPreGain = static_cast<std::uint8_t>(std::clamp(o.preGain, 0, 7));
    state.SetAudio(audio);
    return state.Submit(transport) == TransportResult::Success;
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

// --- the event -> mapping -> preset -> output path -------------------------
//
// The live loop used to call device.Queue() directly from inside the
// detection loop. The cues and their timing are unchanged -- the same two
// pre-rendered clips, queued the same way, with the same late-event budget --
// but they now travel the documented route, so a preset can add a trigger or
// a start offset without the detector learning anything about output.

constexpr const char* kGameId = "sekiro";
constexpr const char* kDeflectEventId = "combat.perfect_deflect";
constexpr const char* kBlockEventId = "combat.block";
constexpr const char* kDeflectPresetId = "guard.deflect";
constexpr const char* kBlockPresetId = "guard.block";

constexpr const char* kWireStartedEventId = "wire.started";
constexpr const char* kWireStartPresetId = "wire.start";
constexpr const char* kWireHapticCueId = "wire.start.haptic";

// 의수는 서로 다른 두 가지 일이 일어난다. 섞으면 안 된다.
//
//  1. 바꿨다  -- 어느 의수로 넘어갔는지 알려 주는 짧은 알림. 무기의 성격을
//                흉내낼 이유가 없다. UI 를 보지 않고 무엇을 골랐는지 아는 것이
//                전부이므로, 한 가지 파형(tool_switch.wav)을 쓴다.
//  2. 쐈다    -- 그 무기의 프리셋이 울린다. 수리검은 수리검 파형, 창은 창 파형.
//
// (1)은 선택 신호로 동작한다.
// (2)는 SprjChrActionRequestModule +0xC8 의 동작 코드로 동작한다
// (SekiroProstheticActionReader.hpp). 코드의 앞 두 자리가 무기를 말하고,
// 끝 다섯 자리가 준비(000xx)와 공격(001xx)을 가른다. 진동은 **공격**에
// 붙는다 -- 준비에 붙이면 도끼처럼 준비가 긴 무기는 휘두르기 전에 울린다.
// R2 만 누른 것(…00900)은 공격이 아니라서 울리지 않는다.
constexpr const char* kToolSwitchedEventId = "prosthetic.switched";
constexpr const char* kToolSwitchPresetId = "prosthetic.switch";
constexpr const char* kToolSwitchCueId = "prosthetic.switch.haptic";
constexpr const char* kToolFiredShurikenEventId = "prosthetic.fired.shuriken";
constexpr const char* kToolFiredSpearEventId = "prosthetic.fired.spear";
constexpr const char* kToolFiredAxeEventId = "prosthetic.fired.axe";
constexpr const char* kToolAxePresetId = "prosthetic.axe";
constexpr const char* kToolAxeCueId = "prosthetic.axe.haptic";
constexpr const char* kToolShurikenPresetId = "prosthetic.shuriken";
constexpr const char* kToolSpearPresetId = "prosthetic.spear";
constexpr const char* kToolShurikenCueId = "prosthetic.shuriken.haptic";
constexpr const char* kToolSpearCueId = "prosthetic.spear.haptic";

/// The "thump" felt as the hook fires.
///
/// Built with the SAME synthesiser the guard cues use -- this is different
/// parameters, not a new engine. Short and low: a wire launch is a shove, not
/// a metallic clang, so the bright inharmonic content the guard cues carry
/// would be wrong here. Every number is a first prototype.
HapticCueProfile WireStartHapticProfile() {
    HapticCueProfile profile;
    profile.totalMs = 70.0f;
    // The same digital amplitude the guard haptics settled on, so the two do
    // not need separate level matching by hand. 0.85 is an amplitude, not a
    // percentage of force.
    profile.normalizePeak = 0.85f;
    //                   start    end      f0      f1    rise   hold  weight  decay
    profile.contact = {  0.0f,   9.0f, 150.0f, 115.0f,  0.8f,  0.0f,  1.00f, 150.0f};
    profile.body    = {  2.0f,  48.0f,  90.0f,  90.0f,  2.0f,  9.0f,  0.90f,  38.0f};
    profile.ring    = { 22.0f,  70.0f, 110.0f, 110.0f,  4.0f,  0.0f,  0.15f,  30.0f};
    profile.noiseAmplitude = 0.05f;
    profile.gain = 1.0f;
    return profile;
}

GameEvent MakeToolEvent(const std::string& eventId, std::int64_t timestampUs) {
    GameEvent event;
    event.gameId = kGameId;
    event.eventId = eventId;
    event.timestamp = std::chrono::microseconds(timestampUs);
    return event;
}

GameEvent MakeWireEvent(std::int64_t timestampUs) {
    GameEvent event;
    event.gameId = kGameId;
    event.eventId = kWireStartedEventId;
    event.timestamp = std::chrono::microseconds(timestampUs);
    return event;
}


struct GuardOutputProfile {
    MappingRepository mappings;
    OutputPresetRepository presets;
};

/// Builds the in-memory profile that reproduces the existing behaviour.
///
/// `wantSpeaker`/`wantHaptic` come from --mode and decide which LAYERS exist,
/// rather than being checked at queue time. That keeps "how many outputs did
/// this event produce" answerable from the preset instead of from a flag read
/// somewhere else.
/// The authored deflect/block actuator waveforms, held for as long as the
/// sink refers to them.
///
/// Two channels, kept apart. These were written as a left/right PAIR, so
/// downmixing them would average out the only thing that distinguishes the
/// two hands, and folding them into the existing mono cue would do the same.
struct BladeHapticPack {
    std::vector<float> deflectLeft, deflectRight;
    std::vector<float> blockLeft, blockRight;
    bool loaded = false;
    /// Why it did or did not load, printed at startup -- a silent fallback to
    /// the synthesised waveform would look exactly like the pack working.
    std::string report;
};

/// Fold everything above `ceiling` down onto it with a tanh knee.
///
/// Driving a clip and then letting the mixer's limiter pull it back is how
/// the level was being given away: the limiter's release is 120 ms, so once
/// it engaged it stayed engaged and held the whole cue down. Saturating the
/// CLIP below that threshold puts the energy in permanently and leaves the
/// limiter idle for a single cue.
///
/// A hard cut would do the same arithmetic and click, because a corner is a
/// step in velocity. The knee only bends what is actually over.
void SaturateToCeiling(std::vector<float>& clip, float ceiling) {
    const float limit = (std::max)(0.05f, (std::min)(ceiling, 0.99f));
    // y = c * tanh(x / c). Below the ceiling tanh(u) ~= u, so quiet material
    // passes through essentially untouched; far above it the curve asymptotes
    // to the ceiling and never reaches a corner.
    for (float& v : clip) v = limit * std::tanh(v / limit);
}

BladeHapticPack LoadBladeHapticPack(const std::string& dir, std::uint32_t rate, float gain,
                                    float ceiling, float tailMs, float tailDecayPerSecond) {
    BladeHapticPack pack;
    if (!std::isfinite(gain) || gain <= 0.0f) {
        pack.report = "refused: --blade-gain must be a positive number";
        return pack;
    }
    struct Entry {
        const char* file;
        std::vector<float>* left;
        std::vector<float>* right;
    };
    const Entry entries[] = {
        {"blade_deflect.wav", &pack.deflectLeft, &pack.deflectRight},
        {"blade_block.wav", &pack.blockLeft, &pack.blockRight},
    };
    std::ostringstream report;
    for (const auto& entry : entries) {
        const auto path = std::filesystem::path(dir) / entry.file;
        AudioClipInfo info;
        LoadAudioClipStereo(path, rate, *entry.left, *entry.right, info);
        if (!info.ok) {
            pack.report = std::string("could not load ") + path.string() + " (" + info.error +
                          ") -- falling back to the SYNTHESISED haptics";
            pack.deflectLeft.clear();
            pack.deflectRight.clear();
            pack.blockLeft.clear();
            pack.blockRight.clear();
            return pack;
        }
        const auto authoredMs = entry.left->size() * 1000 / (std::max)(1u, rate);

        // Order matters, and it is: tail, then gain, then ceiling.
        //
        // The tail is built from the ORIGINAL material, before the gain, so
        // its grains come from a ring that has not been squared off yet --
        // and then it is driven and folded along with everything else, so the
        // added length is as strong as the hit rather than a quiet fade.
        // Both sides get the same deterministic grain offsets, so the pair
        // stays correlated instead of drifting apart.
        ExtendDecayTail(*entry.left, rate, tailMs, tailDecayPerSecond, 18.0f);
        ExtendDecayTail(*entry.right, rate, tailMs, tailDecayPerSecond, 18.0f);

        // No per-clip normalisation: the pack set the relative levels of
        // these two cues on purpose, and normalising each would flatten
        // exactly that difference. The gain is the same number for both.
        for (float& v : *entry.left) v *= gain;
        for (float& v : *entry.right) v *= gain;
        SaturateToCeiling(*entry.left, ceiling);
        SaturateToCeiling(*entry.right, ceiling);

        float peak = 0.0f, sumSquares = 0.0f;
        for (float v : *entry.left) {
            peak = (std::max)(peak, std::fabs(v));
            sumSquares += v * v;
        }
        const float rms = entry.left->empty()
                              ? 0.0f
                              : std::sqrt(sumSquares / static_cast<float>(entry.left->size()));
        report << "    " << entry.file << ": " << authoredMs << " ms + " << tailMs << " ms ring = "
               << entry.left->size() * 1000 / (std::max)(1u, rate) << " ms, x" << gain
               << " into ceiling " << ceiling << " -> peak " << peak << " rms " << rms << "\n";
    }
    pack.loaded = true;
    pack.report = std::string("authored blade waveforms (deflect/block actuator pair)\n") +
                  report.str() + "    preset layer gain stays 1.0, clips are not normalised";
    return pack;
}

/// One prosthetic's selection cue, as the workbench exports it.
struct ToolCuePack {
    std::vector<float> left, right;
    bool loaded = false;
    std::string report;
};

ToolCuePack LoadToolCue(const std::string& dir, const char* file, std::uint32_t rate) {
    ToolCuePack pack;
    const auto path = std::filesystem::path(dir) / file;
    AudioClipInfo info;
    LoadAudioClipStereo(path, rate, pack.left, pack.right, info);
    if (!info.ok) {
        pack.report = std::string(file) + ": " + info.error;
        return pack;
    }
    pack.loaded = true;
    std::ostringstream report;
    report << file << ": " << pack.left.size() * 1000 / (std::max)(1u, rate) << " ms";
    pack.report = report.str();
    return pack;
}

/// Put whichever pair of waveforms is in force onto the two guard cues. The
/// cue IDS are unchanged, so no preset, mapping or event path knows which of
/// the two it got.
void RegisterGuardHapticCues(DualSenseAudioPcmSink& sink, const BladeHapticPack& blade,
                             const std::vector<float>& deflectHaptic,
                             const std::vector<float>& blockHaptic) {
    if (blade.loaded) {
        sink.RegisterStereoCue("deflect.haptic", blade.deflectLeft, blade.deflectRight);
        sink.RegisterStereoCue("block.haptic", blade.blockLeft, blade.blockRight);
        return;
    }
    sink.RegisterCue("deflect.haptic", deflectHaptic, PcmCueTarget::Haptic);
    sink.RegisterCue("block.haptic", blockHaptic, PcmCueTarget::Haptic);
}

GuardOutputProfile BuildGuardProfile(const GuardCueConfig& config, bool wantSpeaker,
                                     bool wantHaptic, bool triggerDemo, bool wireHaptic) {
    GuardOutputProfile profile;
    if (wireHaptic) {
        // Haptic only. No speaker layer: the recordings on hand are sword
        // impacts, and playing one for a grappling hook would be using an
        // asset for something it is not.
        profile.mappings.AddMapping({kGameId, kWireStartedEventId, kWireStartPresetId});
        OutputPreset wire;
        wire.presetId = kWireStartPresetId;
        wire.displayName = "Wire launch";
        PcmHapticOutputLayer layer;
        layer.cueId = kWireHapticCueId;
        layer.gain = 1.0f;
        wire.pcmHaptic.push_back(layer);
        profile.presets.AddPreset(wire);
    }
    profile.mappings.AddMapping({kGameId, kDeflectEventId, kDeflectPresetId});
    profile.mappings.AddMapping({kGameId, kBlockEventId, kBlockPresetId});

    auto build = [&](const char* presetId, const char* displayName, const char* cuePrefix,
                     const GuardCueProfile& cue) {
        OutputPreset preset;
        preset.presetId = presetId;
        preset.displayName = displayName;
        if (wantSpeaker) {
            SpeakerOutputLayer layer;
            layer.cueId = std::string(cuePrefix) + ".speaker";
            layer.gain = 1.0f;          // the clip is already at its rendered level
            preset.speaker.push_back(layer);
        }
        if (wantHaptic) {
            PcmHapticOutputLayer layer;
            layer.cueId = std::string(cuePrefix) + ".haptic";
            layer.gain = 1.0f;
            layer.balance = cue.balance;
            preset.pcmHaptic.push_back(layer);
        }
        if (triggerDemo) {
            // DEMO VALUES. Nothing measured says a deflect should feel like a
            // weapon trigger at zones 3..7, or that a block should be a flat
            // resistance from zone 2. They are here to make trigger output
            // audible/feelable next to the real cues, and they are off unless
            // --trigger-demo is passed.
            TriggerOutputLayer layer;
            if (std::string(presetId) == kDeflectPresetId) {
                layer.side = dualsense::TriggerSide::Right;
                layer.spec = dualsense::TriggerEffectSpec::MakeWeapon(3, 7, 8);
                layer.durationMs = 120.0f;
            } else {
                layer.side = dualsense::TriggerSide::Left;
                layer.spec = dualsense::TriggerEffectSpec::MakeFeedback(2, 5);
                layer.durationMs = 180.0f;
            }
            preset.trigger.push_back(layer);
        }
        profile.presets.AddPreset(preset);
    };

    build(kDeflectPresetId, "Deflect", "deflect", config.deflect);
    build(kBlockPresetId, "Block", "block", config.block);
    return profile;
}

GameEvent MakeGuardEvent(bool deflect, std::int64_t timestampUs) {
    GameEvent event;
    event.gameId = kGameId;
    event.eventId = deflect ? kDeflectEventId : kBlockEventId;
    event.timestamp = std::chrono::microseconds(timestampUs);
    return event;
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

    // Loaded here, before the loop, like every other clip: nothing on the
    // event path opens a file. Only the WAVEFORM changes -- the cue ids, the
    // presets, the mappings and which events reach them are untouched.
    BladeHapticPack blade;
    if (o.bladePcm) {
        blade = LoadBladeHapticPack(o.bladePcmDir, rate, o.bladeGain, o.bladeCeiling,
                                    o.bladeTailMs, o.bladeTailDecayPerSecond);
        std::cout << "  deflect/block haptics: " << blade.report << "\n";
    } else {
        std::cout << "  deflect/block haptics: SYNTHESISED (--no-blade-pcm)\n";
    }

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
    // The HID side, owned in one place. MarkPcmHapticsActive is bookkeeping
    // only: it writes no byte, and crucially it does NOT set HAPTICS_SELECT,
    // which would take the actuators off the PCM path the cues use.
    dualsense::DualSenseOutputState hidState;
    hidState.MarkPcmHapticsActive(wantHaptic);
    // The output-path byte configures the WHOLE endpoint, not just the speaker.
    // Gating this on "the caller wants speaker output" left haptic-only playback
    // silent even though the same channels vibrated during --channel-test, which
    // did send it. So it is always sent.
    const bool routed =
        o.speakerRouting && ClaimSpeakerRouting(hidState, transport, opts);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent")
              << "  (path " << opts.audioPath << ", controller volume 0x" << std::hex
              << (opts.speakerVolume & 0xFF) << std::dec << ")\n"
              << "  haptic path: " << dualsense::ToString(hidState.HapticPathInUse())
              << "   (legacy rumble is NOT claimed, so the actuators stay on PCM)\n";
    std::cout << "pattern=" << o.pattern << " hits=" << hits.size()
              << " interval=" << o.intervalMs << "ms mode=" << o.mode << "\n"
              << "speakerCh=" << opts.speakerChannel << " hapticL=" << opts.hapticLeft
              << " hapticR=" << opts.hapticRight << "\n\n";

    // Manual playback goes through the SAME event -> mapping -> preset ->
    // output path as --live, so this is a real rehearsal of that path with a
    // scripted event source instead of the game. Only the source differs.
    DualSenseAudioPcmSink pcmSink(device);
    if (wantSpeaker) pcmSink.SetSpeakerChannel(opts.speakerChannel);
    if (wantHaptic) pcmSink.SetHapticChannels(opts.hapticLeft, opts.hapticRight);
    pcmSink.RegisterCue("deflect.speaker", deflectSpeaker, PcmCueTarget::Speaker);
    pcmSink.RegisterCue("block.speaker", blockSpeaker, PcmCueTarget::Speaker);
    RegisterGuardHapticCues(pcmSink, blade, deflectHaptic, blockHaptic);

    dualsense::AdaptiveTriggerRuntime triggerRuntime(transport, hidState);
    if (o.triggerDemo) {
        triggerRuntime.OnReconnect(NowUs());
        std::cout << "adaptive-trigger DEMO layers are ON: deflect -> R2 weapon 120 ms,\n"
                     "  block -> L2 feedback 180 ms. Demonstration values, not a tuned\n"
                     "  mapping, and no game button is remapped. Hold L2/R2 to feel them.\n\n";
    }

    const auto profile = BuildGuardProfile(config, wantSpeaker, wantHaptic, o.triggerDemo, false);
    OutputRuntimeConfig runtimeConfig;
    runtimeConfig.maxOutputLatencyUs = config.maxOutputLatencyUs;
    OutputRuntime outputRuntime(profile.mappings, profile.presets, &pcmSink,
                                o.triggerDemo ? &triggerRuntime : nullptr,
                                o.triggerDemo ? &hidState : nullptr, runtimeConfig);
    for (const auto& problem : outputRuntime.Bind())
        std::cout << "  preset \"" << problem.presetId << "\": " << problem.message << "\n";

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
                const auto dispatched = outputRuntime.Handle(MakeGuardEvent(deflect, due), now);
                std::cout << "  [" << (now - start) / 1000 << "ms] "
                          << (deflect ? "DEFLECT" : "block  ")
                          << "  late=" << late / 1000 << "ms voices=" << device.ActiveVoices()
                          << " id=" << dispatched.correlationId
                          << " layers=" << dispatched.layersDispatched << "/"
                          << dispatched.layersTotal;
                if (dispatched.layersFailed > 0) std::cout << " FAILED=" << dispatched.layersFailed;
                std::cout << "\n";
            }
            ++next;
        }
        outputRuntime.Tick(now);
        // Wait for the PCM tails AND for any trigger effect still holding its
        // lifetime: a trigger that outlives the last clip must still be
        // released by this process rather than left on the controller.
        const bool triggersBusy = o.triggerDemo &&
                                  (triggerRuntime.Active(dualsense::TriggerSide::Left).active ||
                                   triggerRuntime.Active(dualsense::TriggerSide::Right).active);
        if (next >= hits.size() && device.ActiveVoices() == 0 && !triggersBusy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Let the last tail actually reach the speaker before tearing down.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    outputRuntime.DropPending();
    // Both owners of a trigger effect hand it back. Leaving a preparation
    // resistance on the controller after the session would be exactly the
    // "stale effect outlives its state" failure this is built to avoid.
    if (o.triggerDemo) triggerRuntime.ResetToNeutral(NowUs());
    {
        const auto rt = outputRuntime.Stats();
        std::cout << "\n--- output runtime ---\n"
                  << "  events dispatched : " << rt.eventsDispatched
                  << "   droppedLate=" << rt.eventsDroppedLate << "\n"
                  << "  layers queued     : speaker=" << rt.speakerLayersQueued
                  << " haptic=" << rt.hapticLayersQueued
                  << " trigger=" << rt.triggerLayersApplied
                  << "   failures=" << rt.layerFailures << "\n";
        if (o.triggerDemo) {
            const auto ts = triggerRuntime.Stats();
            std::cout << "  triggers          : applied=" << ts.applied
                      << " replaced=" << ts.replaced << " expired=" << ts.expired
                      << " writeFailures=" << ts.writeFailures << "\n"
                      << "                      (submissions, NOT a claim about how they felt)\n";
        }
        const auto voiceHistory = device.RecentVoiceHistory();
        std::size_t completed = 0, retired = 0, discarded = 0;
        for (const auto& record : voiceHistory) {
            if (record.reason == VoiceEndReason::Completed) ++completed;
            else if (record.reason == VoiceEndReason::RetiredAtCap) ++retired;
            else ++discarded;
        }
        std::cout << "  voice end reasons : playedToTheEnd=" << completed
                  << " retiredAtCap=" << retired << " droppedOrEvicted=" << discarded
                  << "   (of " << voiceHistory.size() << " finished voices)\n";
    }
    ReportRenderStats(device.RenderStats());
    // Leave nothing sounding and give the routing back.
    device.StopRenderThread();
    device.DropPending();
    device.Close();
    // One report hands back everything this process claimed: both triggers
    // to Off and the audio section released. Doing it through the shared
    // state is what keeps the trigger release from dropping the routing
    // release, or the reverse.
    if (routed) { hidState.ResetToNeutral(); hidState.Submit(transport, true); }
    transport.Close();
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

/// Write the FINISHED speaker cues -- the exact buffers `--live` plays.
///
/// `--write-wav` writes the SYNTHESISED cues, which is a different thing: the
/// live path loads a recording, rebuilds its ring with ExtendDecayTail and
/// shapes it, and none of that is in a synthesised dump. Anything that wants
/// to play what the game plays (the haptic workbench's speaker cue, for one)
/// needs this buffer, not the raw slice and not the synthesised stand-in.
int WriteRenderedCues(const Options& o) {
    GuardCueConfig config = DefaultGuardCueConfig();
    if (std::ifstream in(o.config); in) {
        std::stringstream b;
        b << in.rdbuf();
        std::string error;
        if (!ParseGuardCueConfig(b.str(), config, error)) {
            std::cout << "config error: " << error << "\n";
            return 2;
        }
    }
    ApplySpeakerOverrides(config, o);
    const std::uint32_t rate = 48'000;
    std::error_code ec;
    std::filesystem::create_directories(o.writeCue, ec);
    for (const auto& [name, profile] : {std::pair<const char*, const GuardCueProfile&>{"deflect", config.deflect},
                                        std::pair<const char*, const GuardCueProfile&>{"block", config.block}}) {
        std::string source;
        const auto cue = RenderSpeakerCue(profile, rate, config.speakerVolume, source);
        const auto path = o.writeCue + "/" + name + "_cue.wav";
        WriteWav(path, cue, rate);
        std::cout << "  " << name << ": " << source << "\n";
    }
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

    // Loaded here, before the loop, like every other clip: nothing on the
    // event path opens a file. Only the WAVEFORM changes -- the cue ids, the
    // presets, the mappings and which events reach them are untouched.
    BladeHapticPack blade;
    if (o.bladePcm) {
        blade = LoadBladeHapticPack(o.bladePcmDir, rate, o.bladeGain, o.bladeCeiling,
                                    o.bladeTailMs, o.bladeTailDecayPerSecond);
        std::cout << "  deflect/block haptics: " << blade.report << "\n";
    } else {
        std::cout << "  deflect/block haptics: SYNTHESISED (--no-blade-pcm)\n";
    }

    const bool wantSpeaker = o.mode == "speaker" || o.mode == "both";
    const bool wantHaptic = o.mode == "haptic" || o.mode == "both";
    if (wantSpeaker && opts.speakerChannel < 0) { std::cout << "--speaker-channel required\n"; return 2; }
    if (wantHaptic && opts.hapticLeft < 0 && opts.hapticRight < 0) { std::cout << "--haptic-left/right required\n"; return 2; }

    HidApiDualSenseTransport transport;
    // The HID side, owned in one place. MarkPcmHapticsActive is bookkeeping
    // only: it writes no byte, and crucially it does NOT set HAPTICS_SELECT,
    // which would take the actuators off the PCM path the cues use.
    dualsense::DualSenseOutputState hidState;
    hidState.MarkPcmHapticsActive(wantHaptic);
    // The output-path byte configures the WHOLE endpoint, not just the speaker.
    // Gating this on "the caller wants speaker output" left haptic-only playback
    // silent even though the same channels vibrated during --channel-test, which
    // did send it. So it is always sent.
    const bool routed =
        o.speakerRouting && ClaimSpeakerRouting(hidState, transport, opts);
    std::cout << "audio routing report: " << (routed ? "sent" : "not sent")
              << "  (path " << opts.audioPath << ", controller volume 0x" << std::hex
              << (opts.speakerVolume & 0xFF) << std::dec << ")\n"
              << "  haptic path: " << dualsense::ToString(hidState.HapticPathInUse())
              << "   (legacy rumble is NOT claimed, so the actuators stay on PCM)\n";
    // ---- output runtime ---------------------------------------------------
    // The clips were rendered above, before the loop. The sink only ever hands
    // the mixer a reference to them; nothing on the event path synthesises,
    // decodes, opens a file or allocates a clip.
    DualSenseAudioPcmSink pcmSink(device);
    if (wantSpeaker) pcmSink.SetSpeakerChannel(opts.speakerChannel);
    if (wantHaptic) pcmSink.SetHapticChannels(opts.hapticLeft, opts.hapticRight);
    pcmSink.RegisterCue("deflect.speaker", deflectSpeaker, PcmCueTarget::Speaker);
    pcmSink.RegisterCue("block.speaker", blockSpeaker, PcmCueTarget::Speaker);
    RegisterGuardHapticCues(pcmSink, blade, deflectHaptic, blockHaptic);
    // Rendered here, before the loop, like every other cue -- the polling loop
    // never synthesises.
    const auto wireHapticCue = SynthesizeGuardHaptic(WireStartHapticProfile(), rate,
                                                     config.hapticStrength);
    pcmSink.RegisterCue(kWireHapticCueId, wireHapticCue, PcmCueTarget::Haptic);

    dualsense::AdaptiveTriggerRuntime triggerRuntime(transport, hidState);
    if (o.triggerDemo) {
        // Start from neutral, so a previous run's held effect cannot be
        // mistaken for this run's output. The audio claim above survives it:
        // OnReconnect touches the two trigger sections only.
        triggerRuntime.OnReconnect(NowUs());
        std::cout << "adaptive-trigger DEMO layers are ON. Those parameters are a\n"
                     "  demonstration, not a tuned mapping; no game button is remapped.\n";
    }

    // R2 preparation resistance from the SELECTED prosthetic. This is
    // selection state, not use: nothing below records a shot, a swing or a
    // hit, and pressing R2 is never treated as one.
    std::optional<SekiroProstheticReader> prostheticReader;
    if (o.prosthetic) {
        prostheticReader.emplace(reader, reader, live_detail::MakeWorldChrManSpec(mainModule.name),
                                 live_detail::KnownGoodIdentity(), identity, mainModule.baseAddress);
        const auto primedTool = prostheticReader->Prime();
        std::cout << "prosthetic reader: " << ToString(primedTool) << "\n"
                  << "  R2 prep: Loaded Shuriken(70000) weapon catch zones "
                  << int(o.prostheticTrigger.shurikenStartZone) << ".."
                  << int(o.prostheticTrigger.shurikenEndZone) << " strength "
                  << int(o.prostheticTrigger.shurikenStrength)
                  << " | Loaded Spear(78000) feedback from zone "
                  << int(o.prostheticTrigger.spearStartZone) << " strength "
                  << int(o.prostheticTrigger.spearStrength) << "\n"
                  << "  every other prosthetic, and any unknown selection, releases it.\n";
        if (primedTool != RootResolveResult::Resolved) {
            std::cout << "  not primed -- no preparation resistance will be applied\n";
            prostheticReader.reset();
        }
    }

    // Wire actions. Detection and logging ONLY: the field below distinguishes
    // a real wire action from a button press with no target, from a usable
    // target nobody grappled to, and from ordinary jumps and falls -- but what
    // it should FEEL like has not been decided, so nothing is driven from it.
    // 의수 사용(공격) 리더. 발사 큐를 싣는 경우에만 켠다 -- 쓸 곳 없는 읽기를
    // 매 5 ms 돌릴 이유가 없다.
    std::optional<SekiroProstheticActionReader> toolActionReader;
    ProstheticActionDetector toolActionDetector;
    std::uint64_t toolAttacks = 0, toolReadies = 0;
    if (!o.prostheticCueDir.empty()) {
        toolActionReader.emplace(reader, reader, live_detail::MakeWorldChrManSpec(mainModule.name),
                                 live_detail::KnownGoodIdentity(), identity,
                                 mainModule.baseAddress);
        const auto primedAction = toolActionReader->Prime();
        std::cout << "prosthetic action reader: " << ToString(primedAction)
                  << "  (SprjChrActionRequestModule +0xC8)\n";
        if (primedAction != RootResolveResult::Resolved) {
            std::cout << "  not primed -- attack cues will not fire\n";
            toolActionReader.reset();
        }
    }

    std::optional<SekiroWireActionReader> wireReader;
    WireActionEventDetector wireDetector;
    if (o.wire) {
        wireReader.emplace(reader, reader, live_detail::MakeWorldChrManSpec(mainModule.name),
                           live_detail::KnownGoodIdentity(), identity, mainModule.baseAddress);
        const auto primedWire = wireReader->Prime();
        std::cout << "wire reader: " << ToString(primedWire)
                  << "  (CSWireActionModule +0x1c0; start/end only -- shoot, attach,\n"
                     "  pull and arrive are NOT separable from this field)\n";
        if (primedWire != RootResolveResult::Resolved) {
            std::cout << "  not primed -- wire actions will not be logged\n";
            wireReader.reset();
        }
    }

    // 의수 선택 큐. 확인된 두 개(수리검 70000, 창 78000)만 싣는다 -- 나머지는
    // 무엇인지 모르고, 모르는 것에 파형을 붙이는 것은 추측이다.
    ToolCuePack switchCue, shurikenCue, spearCue, axeCue;
    if (!o.prostheticCueDir.empty()) {
        switchCue = LoadToolCue(o.prostheticCueDir, "tool_switch.wav", rate);
        shurikenCue = LoadToolCue(o.prostheticCueDir, "shuriken.wav", rate);
        spearCue = LoadToolCue(o.prostheticCueDir, "spear.wav", rate);
        axeCue = LoadToolCue(o.prostheticCueDir, "axe.wav", rate);
        std::cout << "prosthetic cues:\n"
                  << "    switch  " << switchCue.report << "   <- when you change tool\n"
                  << "    attack  " << shurikenCue.report << "\n"
                  << "    attack  " << spearCue.report << "\n"
                  << "    attack  " << axeCue.report << "\n"
                  << "  attack cues fire on the ATTACK code, not the ready code and\n"
                  << "  not on R2 -- see SekiroProstheticActionReader.hpp.\n";
        if (switchCue.loaded)
            pcmSink.RegisterStereoCue(kToolSwitchCueId, switchCue.left, switchCue.right);
        if (shurikenCue.loaded)
            pcmSink.RegisterStereoCue(kToolShurikenCueId, shurikenCue.left, shurikenCue.right);
        if (spearCue.loaded)
            pcmSink.RegisterStereoCue(kToolSpearCueId, spearCue.left, spearCue.right);
        if (axeCue.loaded)
            pcmSink.RegisterStereoCue(kToolAxeCueId, axeCue.left, axeCue.right);
    }

    auto profile = BuildGuardProfile(config, wantSpeaker, wantHaptic, o.triggerDemo,
                                          o.wire && o.wireHaptic && wantHaptic);
    // 바꿈 큐 하나 + 무기별 발사 큐. 매핑은 이벤트 하나에 프리셋 하나다.
    for (const auto& [cueId, presetId, eventId, loaded] :
         {std::tuple<const char*, const char*, const char*, bool>{
              kToolSwitchCueId, kToolSwitchPresetId, kToolSwitchedEventId, switchCue.loaded},
          std::tuple<const char*, const char*, const char*, bool>{
              kToolShurikenCueId, kToolShurikenPresetId, kToolFiredShurikenEventId,
              shurikenCue.loaded},
          std::tuple<const char*, const char*, const char*, bool>{
              kToolSpearCueId, kToolSpearPresetId, kToolFiredSpearEventId, spearCue.loaded},
          std::tuple<const char*, const char*, const char*, bool>{
              kToolAxeCueId, kToolAxePresetId, kToolFiredAxeEventId, axeCue.loaded}}) {
        if (!loaded || !wantHaptic) continue;
        profile.mappings.AddMapping({kGameId, eventId, presetId});
        OutputPreset preset;
        preset.presetId = presetId;
        preset.displayName = "Prosthetic selection";
        PcmHapticOutputLayer layer;
        layer.cueId = cueId;
        layer.gain = 1.0f;      // 파형에 이미 들어 있다; 두 번 곱하지 않는다
        preset.pcmHaptic.push_back(layer);
        profile.presets.AddPreset(preset);
    }
    OutputRuntimeConfig runtimeConfig;
    runtimeConfig.maxOutputLatencyUs = config.maxOutputLatencyUs;
    OutputRuntime outputRuntime(profile.mappings, profile.presets, &pcmSink,
                                o.triggerDemo ? &triggerRuntime : nullptr,
                                o.triggerDemo ? &hidState : nullptr, runtimeConfig);
    for (const auto& problem : outputRuntime.Bind())
        std::cout << "  preset \"" << problem.presetId << "\": " << problem.message << "\n";

    // ---- detection -------------------------------------------------------
    GuardDetectorConfig detectorConfig;
    detectorConfig.mode = OccurrenceMode::Pulse;   // +0x3C is a one-frame pulse
    GuardOutcomeEventDetector detector(detectorConfig);
    // One detector per enemy: the pulse is per character, so a shared detector
    // would interleave two fights into one nonsense sequence.
    std::map<std::uintptr_t, GuardOutcomeEventDetector> enemyDetectors;
    std::uint64_t enemyDeflects = 0, enemyBlocks = 0;
    // Where enemy reactions actually go. The reader counts RAW pulse edges;
    // these count what happened to them afterwards, so "the reader saw 51
    // edges and 2 events came out" can be attributed instead of guessed at.
    std::uint64_t enemyUnresolved = 0;     // detector could not classify it
    std::uint64_t enemyLateDropped = 0;    // older than the output budget
    std::uint64_t enemyObservations = 0;   // samples handed to a detector
    std::uint64_t enemyPulseObs = 0;       // ... of which had a non-zero pulse
    std::uint64_t enemyContinuityBreaks = 0;
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
    std::int64_t nextProstheticPollUs = started;
    bool prostheticPrepActive = false;
    std::uint32_t prostheticPrepId = kNoEquipId;
    // 선택 큐는 준비 저항과 따로 센다. 준비 저항은 "무엇이 골라져 있나" 의
    // 상태이고, 큐는 "방금 바뀌었다" 의 순간이라 다른 것이다.
    std::uint32_t prostheticCueEquipId = kNoEquipId;
    std::uint64_t prostheticCueGeneration = 0;
    bool prostheticCueBaselined = false;
    std::uint64_t prostheticCues = 0;
    std::uint64_t prostheticGeneration = 0;
    std::uint64_t prostheticEffectId = 0;
    std::uint64_t prostheticApplies = 0, prostheticReleases = 0;
    std::uint64_t wireStarts = 0, wireEnds = 0, wireUnobserved = 0;
    std::uint64_t wireEffectId = 0;
    WirePrepPolicy wirePrepPolicy;
    bool wireLaunchedThisTick = false;
    std::uint64_t wirePrepApplies = 0, wirePrepReleases = 0, wirePrepRearms = 0;
    std::int64_t wirePrepWriteWorstUs = 0, wirePrepPollWorstUs = 0;

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

        // Layer start offsets and trigger expiries. Both write only when
        // something is actually due, so the 5 ms grid is not disturbed.
        //
        // The trigger runtime is ticked explicitly: OutputRuntime only owns it
        // in --trigger-demo, and without this a wire or prosthetic effect with
        // a lifetime would never release itself.
        outputRuntime.Tick(now);
        triggerRuntime.Tick(now);

        if (prostheticReader && now >= nextProstheticPollUs) {
            nextProstheticPollUs = now + static_cast<std::int64_t>(o.prostheticPollMs) * 1000;
            const auto tool = prostheticReader->Poll();
            const auto prep = PrepForSelection(tool.selectedEquipId, tool.Ok(),
                                               o.prostheticTrigger);
            // Key on what was actually APPLIED, so an unchanged selection does
            // not re-apply the same effect every poll -- that would replace the
            // live effect a few times a second and churn HID writes.
            // 선택이 바뀌었을 때만 큐를 낸다.
            //
            // 첫 관측은 기준선일 뿐이다 -- 시작할 때 이미 수리검이 골라져
            // 있었던 것은 바꾼 것이 아니다. 읽기 실패와 객체 교체(로딩/사망)도
            // 마찬가지로 기준선을 다시 잡는다. 그렇게 하지 않으면 불러오기가
            // 끝날 때마다 "바꿨다" 가 한 번씩 나간다.
            if (!tool.Ok() || tool.generation != prostheticCueGeneration) {
                prostheticCueGeneration = tool.generation;
                prostheticCueBaselined = tool.Ok();
                prostheticCueEquipId = tool.selectedEquipId;
            } else if (!prostheticCueBaselined) {
                prostheticCueBaselined = true;
                prostheticCueEquipId = tool.selectedEquipId;
            } else if (tool.selectedEquipId != prostheticCueEquipId) {
                prostheticCueEquipId = tool.selectedEquipId;
                // 어느 의수로 갔든 같은 알림이다. 무엇으로 갔는지는 로그의
                // 장비 id 가 말하고, 손에는 "바뀌었다" 만 오면 된다 -- 무기의
                // 성격은 쐈을 때 오는 것이지 고를 때 오는 것이 아니다.
                if (!o.prostheticCueDir.empty()) {
                    const auto dispatched =
                        outputRuntime.Handle(MakeToolEvent(kToolSwitchedEventId, now), NowUs());
                    ++prostheticCues;
                    std::cout << "  [" << (now - started) / 1000 << "ms] tool SWITCHED to "
                              << tool.selectedEquipId << "  layers="
                              << dispatched.layersDispatched << "/" << dispatched.layersTotal
                              << "\n";
                }
            }

            const bool same = prep.active == prostheticPrepActive &&
                              prep.equipId == prostheticPrepId &&
                              tool.generation == prostheticGeneration;
            if (!same) {
                prostheticPrepActive = prep.active;
                prostheticPrepId = prep.equipId;
                prostheticGeneration = tool.generation;
                if (prep.active) {
                    const auto applied = triggerRuntime.Apply(
                        dualsense::TriggerSide::Right, prep.spec,
                        dualsense::kHoldUntilReplaced, now);
                    prostheticEffectId = applied.accepted ? applied.effectId : 0;
                    std::cout << "  [" << (now - started) / 1000 << "ms] R2 prep: "
                              << prep.reason << "  ("
                              << (applied.accepted ? "applied" : "FAILED")
                              << " id=" << applied.effectId << ")\n";
                    ++prostheticApplies;
                } else {
                    if (prostheticEffectId != 0)
                        triggerRuntime.Cancel(dualsense::TriggerSide::Right,
                                              prostheticEffectId, now);
                    else
                        triggerRuntime.CancelSide(dualsense::TriggerSide::Right, now);
                    prostheticEffectId = 0;
                    std::cout << "  [" << (now - started) / 1000 << "ms] R2 prep released: "
                              << prep.reason;
                    if (prep.equipId != kNoEquipId) std::cout << " (id " << prep.equipId << ")";
                    std::cout << "\n";
                    ++prostheticReleases;
                }
            }
        }

        // 공격 코드는 10 ms 남짓만 머문다. 5 ms 루프마다 읽어야 두 번은 본다.
        if (toolActionReader) {
            const auto act = toolActionReader->Poll();
            for (const auto& e : toolActionDetector.Update(
                     {now, act.Ok(), act.generation, act.actionCode})) {
                if (e.phase == ProstheticPhase::Ready) {
                    ++toolReadies;
                    continue;          // 준비에는 울리지 않는다
                }
                const char* eventId =
                    e.tool == ProstheticTool::Shuriken ? kToolFiredShurikenEventId
                    : e.tool == ProstheticTool::Spear ? kToolFiredSpearEventId
                                                      : kToolFiredAxeEventId;
                const auto dispatched =
                    outputRuntime.Handle(MakeToolEvent(eventId, e.timestampUs), NowUs());
                ++toolAttacks;
                std::cout << "  [" << (now - started) / 1000 << "ms] " << ToString(e.tool)
                          << " ATTACK  code=" << e.code << "  layers="
                          << dispatched.layersDispatched << "/" << dispatched.layersTotal
                          << "\n";
            }
        }

        if (wireReader) {
            const auto wireSample = wireReader->Poll();

            // PREPARATION resistance: L2 holds a catch so the NEXT press has
            // something to give way against. The rule itself lives in
            // WirePrepPolicy, where each clause has a test naming the hardware
            // failure it prevents.
            if (o.wireTrigger && o.wireTriggerMs < 0) {
                WirePrepInput prepIn;
                prepIn.readOk = wireSample.Ok();
                prepIn.groundTarget = wireSample.groundTarget;
                prepIn.airTarget = wireSample.airTarget;
                // 공중 바이트 하나로는 점프와 연속 그래플을 구분하지 못한다.
                // 실제로 와이어 액션 중인지가 그 구분이다.
                prepIn.inWireAction = wireSample.inWireAction;
                // A launch is an EDGE. It is taken from the detector below via
                // the flag set on the previous tick, so the policy sees it
                // exactly once.
                prepIn.launched = wireLaunchedThisTick;
                wireLaunchedThisTick = false;

                const auto prep = wirePrepPolicy.Update(prepIn);
                if (prep.reArm) {
                    // Clear the side first. An identical report is skipped by
                    // the output state, so re-sending the same effect would
                    // not reach the device -- and the device is exactly what
                    // needs telling, because its Weapon effect has latched
                    // released since the trigger was pulled through.
                    triggerRuntime.CancelSide(dualsense::TriggerSide::Left, now);
                    const auto again = triggerRuntime.Apply(
                        dualsense::TriggerSide::Left,
                        dualsense::TriggerEffectSpec::MakeWeapon(o.wireTriggerZone,
                                                                 o.wireTriggerEndZone,
                                                                 o.wireTriggerStrength),
                        dualsense::kHoldUntilReplaced, now);
                    wireEffectId = again.accepted ? again.effectId : 0;
                    ++wirePrepRearms;
                    if (!again.accepted) wirePrepPolicy.Reset();
                    std::cout << "  [" << (now - started) / 1000 << "ms] L2 prep re-arm ("
                              << prep.reason << ")  id=" << again.effectId << "\n";
                } else if (prep.changed) {
                    if (prep.armed) {
                        const auto beforeWrite = NowUs();
                        const auto applied = triggerRuntime.Apply(
                            dualsense::TriggerSide::Left,
                            dualsense::TriggerEffectSpec::MakeWeapon(o.wireTriggerZone,
                                                                     o.wireTriggerEndZone,
                                                                     o.wireTriggerStrength),
                            dualsense::kHoldUntilReplaced, now);
                        const auto writeUs = NowUs() - beforeWrite;
                        wirePrepWriteWorstUs = std::max(wirePrepWriteWorstUs, writeUs);
                        wirePrepPollWorstUs = std::max(wirePrepPollWorstUs, interval);
                        wireEffectId = applied.accepted ? applied.effectId : 0;
                        ++wirePrepApplies;
                        if (!applied.accepted) {
                            // The app must not remember an effect the device
                            // never took.
                            wirePrepPolicy.Reset();
                            std::cout << "  [" << (now - started) / 1000
                                      << "ms] L2 wire prep FAILED: " << applied.error << "\n";
                        } else {
                            std::cout << "  [" << (now - started) / 1000 << "ms] L2 prep ON ("
                                      << prep.reason << ")  id=" << applied.effectId
                                      << " hidWrite=" << writeUs / 1000
                                      << "ms pollGap=" << interval / 1000 << "ms\n";
                        }
                    } else {
                        // Release only what this feature owns. A stale id means
                        // something else took the side over, and forcing it off
                        // would cut that other effect short -- but leaving the
                        // resistance on is worse, so the side is cleared when
                        // the id is not ours any more.
                        bool released = false;
                        if (wireEffectId != 0)
                            released = triggerRuntime.Cancel(dualsense::TriggerSide::Left,
                                                             wireEffectId, now);
                        if (!released) triggerRuntime.CancelSide(dualsense::TriggerSide::Left, now);
                        wireEffectId = 0;
                        ++wirePrepReleases;
                        std::cout << "  [" << (now - started) / 1000 << "ms] L2 prep off ("
                                  << prep.reason << ")  ground=" << (prepIn.groundTarget ? 1 : 0)
                                  << " air=" << (prepIn.airTarget ? 1 : 0)
                                  << " inAction=" << (prepIn.inWireAction ? 1 : 0)
                                  << " read=" << ToString(wireSample.status) << "\n";
                    }
                }
            }

            WireObservation wo;
            wo.timestampUs = now;
            wo.readOk = wireSample.Ok();
            wo.generation = wireSample.generation;
            wo.inWireAction = wireSample.inWireAction;
            wo.handle = wireSample.handle;
            wo.continuityBreak = interval > targetIntervalUs * 4;
            for (const auto& event : wireDetector.Update(wo)) {
                std::cout << "  [" << (now - started) / 1000 << "ms] WIRE "
                          << ToString(event.kind);
                if (event.kind == WireEventKind::Ended)
                    std::cout << "  " << event.durationUs / 1000 << "ms";
                std::cout << "  anchor=0x" << std::hex << event.handle << std::dec;

                if (event.kind == WireEventKind::Started) {
                    ++wireStarts;
                    // Hand the launch to the preparation rule as an edge. It
                    // releases on the next evaluation, which is the same 5 ms
                    // tick -- the flags themselves stay set for about half the
                    // flight and are far too late to release on.
                    wireLaunchedThisTick = true;
                    // The thump goes through the same event -> mapping ->
                    // preset -> sink path as everything else; nothing here
                    // touches the mixer directly.
                    if (o.wireHaptic && wantHaptic) {
                        const auto dispatched =
                            outputRuntime.Handle(MakeWireEvent(event.timestampUs), NowUs());
                        std::cout << " layers=" << dispatched.layersDispatched << "/"
                                  << dispatched.layersTotal;
                    }
                    // The resistance is a SUSTAINED state for as long as the
                    // action runs, so it is held rather than given a duration:
                    // a timer would have to guess how long the flight lasts.
                    if (o.wireTrigger && o.wireTriggerMs >= 0) {
                        // The two older timings, kept only for comparison.
                        const auto lifetime =
                            o.wireTriggerMs > 0
                                ? static_cast<std::int64_t>(o.wireTriggerMs) * 1000
                                : dualsense::kHoldUntilReplaced;
                        const auto applied = triggerRuntime.Apply(
                            dualsense::TriggerSide::Left,
                            dualsense::TriggerEffectSpec::MakeWeapon(o.wireTriggerZone,
                                                                     o.wireTriggerEndZone,
                                                                     o.wireTriggerStrength),
                            lifetime, now);
                        wireEffectId = (applied.accepted && o.wireTriggerMs == 0)
                                           ? applied.effectId : 0;
                        if (!applied.accepted) std::cout << "  L2 FAILED: " << applied.error;
                    }
                } else {
                    if (event.kind == WireEventKind::Ended) ++wireEnds;
                    else ++wireUnobserved;
                    // Ended OR ended-unobserved: either way this process stops
                    // asserting a resistance it can no longer justify.
                    if (wireEffectId != 0) {
                        triggerRuntime.Cancel(dualsense::TriggerSide::Left, wireEffectId, now);
                        wireEffectId = 0;
                    }
                }
                std::cout << "\n";
            }
        }

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
            if (event.kind == GuardEventKind::Unresolved) {
                ++unresolved;
                std::cout << "  [" << (now - started) / 1000 << "ms] unresolved (no cue played)\n";
                continue;
            }
            const bool deflect = event.kind == GuardEventKind::Deflect;
            const auto age = NowUs() - event.timestampUs;
            // The previous cue is left alone: a normal new event never stops,
            // ducks or fades what is already sounding. Only the voice cap can
            // shorten a tail, and it fades the quietest one instead of cutting.
            // The late-event budget moved into the runtime, with the same
            // value read from the same config field.
            const auto dispatched =
                outputRuntime.Handle(MakeGuardEvent(deflect, event.timestampUs), NowUs());
            if (dispatched.outcome == DispatchOutcome::DroppedLate) {
                ++lateDrops;      // never flush a stale hit out later
                std::cout << "  [" << (now - started) / 1000 << "ms] dropped (" << age / 1000 << "ms late)\n";
                continue;
            }
            (deflect ? deflects : blocks)++;
            std::cout << "  [" << (now - started) / 1000 << "ms] " << (deflect ? "DEFLECT" : "block  ")
                      << "  latency=" << age / 1000 << "ms voices=" << device.ActiveVoices()
                      << " id=" << dispatched.correlationId
                      << " layers=" << dispatched.layersDispatched << "/" << dispatched.layersTotal;
            if (dispatched.layersFailed > 0) std::cout << " FAILED=" << dispatched.layersFailed;
            std::cout << "\n";
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
                ++enemyObservations;
                if (es.pulse != 0) ++enemyPulseObs;
                if (eo.continuityBreak) ++enemyContinuityBreaks;
                auto& det = enemyDetectors.try_emplace(es.character, detectorConfig).first->second;
                for (const auto& event : det.Update(eo)) {
                    if (event.kind == GuardEventKind::Unresolved) {
                        // Counted, not swallowed: an unresolved reaction is a
                        // reaction the detector saw and could not classify,
                        // which is a different fact from never seeing one.
                        ++enemyUnresolved;
                        continue;
                    }
                    const bool deflect = event.kind == GuardEventKind::Deflect;
                    const auto dispatched =
                        outputRuntime.Handle(MakeGuardEvent(deflect, event.timestampUs), NowUs());
                    if (dispatched.outcome == DispatchOutcome::DroppedLate) {
                        ++lateDrops;
                        ++enemyLateDropped;
                        continue;
                    }
                    (deflect ? enemyDeflects : enemyBlocks)++;
                    std::cout << "  [" << (now - started) / 1000 << "ms] ENEMY "
                              << (deflect ? "DEFLECT" : "block  ") << "  0x" << std::hex
                              << es.character << std::dec << "\n";
                }
            }
        }
    }

    if (enemyReader) enemyReader->StopBackgroundDiscovery();
    if (toolActionReader) {
        const auto& ts = toolActionReader->Stats();
        std::cout << "prosthetic action: attacks=" << toolAttacks << " readies=" << toolReadies
                  << "  (reads ok=" << ts.ok << " failed=" << ts.failed
                  << " moduleSearches=" << ts.moduleSearches << ")\n";
    }
    // A layer scheduled but not yet started must not escape after the session.
    for (const auto& event : wireDetector.Finish(NowUs())) { (void)event; ++wireUnobserved; }
    if (wireEffectId != 0) triggerRuntime.Cancel(dualsense::TriggerSide::Left, wireEffectId, NowUs());
    wirePrepPolicy.Reset();
    outputRuntime.DropPending();
    // Both owners of a trigger effect hand it back. Leaving a preparation
    // resistance on the controller after the session would be exactly the
    // "stale effect outlives its state" failure this is built to avoid.
    if (o.triggerDemo || prostheticReader || wireReader) triggerRuntime.ResetToNeutral(NowUs());
    for (const auto& event : detector.Finish()) { (void)event; ++unresolved; }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto renderStats = device.RenderStats();
    device.StopRenderThread();
    device.DropPending();
    device.Close();
    if (routed) { hidState.ResetToNeutral(); hidState.Submit(transport, true); }
    transport.Close();

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
                  << "    observations=" << enemyObservations
                  << " withPulse=" << enemyPulseObs
                  << " unresolved=" << enemyUnresolved
                  << " lateDropped=" << enemyLateDropped
                  << " vptrRejected=" << es.pulseVptrRejected
                  << " outcomeUnreadable=" << es.pulseOutcomeUnreadable
                  << " continuityBreaks=" << enemyContinuityBreaks << "\n"
                  << "    detectors=" << enemyDetectors.size() << "\n"
                  << "    pathChanges=" << es.pathChanges
                  << "  ambiguousDropped=" << es.ambiguousModules
                  << "  lastDiscovery=" << es.lastDiscoveryUs / 1000 << " ms\n"
                  << "    An enemy event means THAT CHARACTER guarded. It does not prove\n"
                  << "    it guarded your attack.\n";
    }
    {
        const auto rt = outputRuntime.Stats();
        std::cout << "  output: dispatched=" << rt.eventsDispatched
                  << " speakerLayers=" << rt.speakerLayersQueued
                  << " hapticLayers=" << rt.hapticLayersQueued
                  << " triggerLayers=" << rt.triggerLayersApplied
                  << " layerFailures=" << rt.layerFailures << "\n";
        if (o.triggerDemo) {
            const auto ts = triggerRuntime.Stats();
            std::cout << "  triggers: applied=" << ts.applied << " replaced=" << ts.replaced
                      << " expired=" << ts.expired << " writeFailures=" << ts.writeFailures
                      << "   (submissions, NOT a claim about how they felt)\n";
        }
        if (wireReader) {
            const auto ws = wireReader->Stats();
            std::cout << "  wire: started=" << wireStarts << " ended=" << wireEnds
                      << " endedUnobserved=" << wireUnobserved
                      << "  polls=" << ws.polls << " ok=" << ws.ok << " failed=" << ws.failed
                      << " moduleSearches=" << ws.moduleSearches << "\n"
                      << "    L2 prep worst hidWrite=" << wirePrepWriteWorstUs / 1000
                      << "ms worst pollGap=" << wirePrepPollWorstUs / 1000 << "ms\n"
                  << "    L2 prep re-arms=" << wirePrepRearms << "\n"
                  << "    L2 prep applies=" << wirePrepApplies
                      << " releases=" << wirePrepReleases
                      << "  (resistance follows target availability; "
                      << (o.wireTriggerMs < 0 ? "default" : "legacy --wire-ms timing")
                      << ")\n"
                  << "    start thump " << (o.wireHaptic ? "on" : "off") << ".\n"
                  << "    Start and end only. No shoot/attach/pull/arrive phase is claimed,\n"
                      << "    and endedUnobserved is NOT a completed action.\n";
        }
        if (prostheticReader) {
            std::cout << "  prosthetic switch cues: " << prostheticCues << "\n";
            const auto ps = prostheticReader->Stats();
            std::cout << "  prosthetic: applies=" << prostheticApplies
                      << " releases=" << prostheticReleases
                      << "  polls=" << ps.polls << " ok=" << ps.ok << " failed=" << ps.failed
                      << " gameDataSearches=" << ps.playerGameDataSearches
                      << " gameDataChanges=" << ps.playerGameDataChanges << "\n"
                      << "    This is SELECTION state only. No use, shot or hit is claimed.\n";
        }
        // Per-voice end reasons. "Every cue played to its end" is checked
        // here rather than inferred from the absence of a complaint.
        const auto voiceHistory = device.RecentVoiceHistory();
        std::size_t completed = 0, retired = 0, discarded = 0;
        for (const auto& record : voiceHistory) {
            if (record.reason == VoiceEndReason::Completed) ++completed;
            else if (record.reason == VoiceEndReason::RetiredAtCap) ++retired;
            else ++discarded;
        }
        std::cout << "  voices(last " << voiceHistory.size() << "): playedToTheEnd=" << completed
                  << " retiredAtCap=" << retired << " droppedOrEvicted=" << discarded << "\n";
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
        else if (a == "--no-enemy") o.enemy = false;
        else if (a == "--prosthetic-cue-dir") o.prostheticCueDir = next();
        else if (a == "--no-blade-pcm") o.bladePcm = false;
        else if (a == "--blade-pcm-dir") o.bladePcmDir = next();
        else if (a == "--blade-gain") o.bladeGain = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--blade-ceiling") o.bladeCeiling = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--blade-tail-ms") o.bladeTailMs = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--blade-tail-decay")
            o.bladeTailDecayPerSecond = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--no-prosthetic-trigger") o.prosthetic = false;
        else if (a == "--no-wire") o.wire = false;
        else if (a == "--no-wire-haptic") o.wireHaptic = false;
        else if (a == "--no-wire-trigger") o.wireTrigger = false;
        else if (a == "--wire-zone" || a == "--wire-start") o.wireTriggerZone =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--wire-end") o.wireTriggerEndZone =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--wire-strength") o.wireTriggerStrength =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--wire-ms") o.wireTriggerMs = std::atoi(next().c_str());
        else if (a == "--shuriken-start") o.prostheticTrigger.shurikenStartZone =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--shuriken-end") o.prostheticTrigger.shurikenEndZone =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--shuriken-strength") o.prostheticTrigger.shurikenStrength =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--spear-start") o.prostheticTrigger.spearStartZone =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--spear-strength") o.prostheticTrigger.spearStrength =
                     static_cast<std::uint8_t>(std::atoi(next().c_str()));
        else if (a == "--trigger-demo") o.triggerDemo = true;
        else if (a == "--enemy-rediscover") o.enemyRediscoverSeconds = static_cast<float>(std::atof(next().c_str()));
        else if (a == "--write-cue") o.writeCue = next();
        else if (a == "--dump-mix") o.dumpMix = next();
        else if (a == "--report-length") o.forceLength = std::atoi(next().c_str());
        else { std::cout << "unknown option: " << a << "\n"; Help(); return 2; }
    }
    if (o.list) return ListDevices();
    if (!o.writeCue.empty()) return WriteRenderedCues(o);
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
