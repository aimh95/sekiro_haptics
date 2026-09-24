// haptic_lab -- compare PCM haptic bursts by hand, with no game running.
//
// WHAT THIS PROCESS IS
// --------------------
// The output half of the tool. It opens the DualSense audio endpoint ONCE,
// starts the render thread ONCE, and then takes one command per line on
// stdin, answering with one JSON object per line on stdout. The GUI
// (tools/haptic_lab_gui.py) is the other half and owns no audio at all.
//
// It is split this way because the mixer, the endpoint selection and the
// verified haptic channels already exist here and are worth reusing exactly
// as the game path uses them. A tool that re-implemented any of that would be
// measuring itself rather than the thing the game will play.
//
// THE ONE RULE THIS FILE IS BUILT AROUND
// --------------------------------------
// A parameter change must never re-open the device or reset the stream.
// Re-opening is a discontinuity of tens of milliseconds and a fresh limiter
// state, which is precisely the kind of artefact somebody using this tool
// would otherwise attribute to the waveform they just changed. So the stream
// is opened at startup, held for the life of the process, and every command
// below only ever changes what gets QUEUED onto it.
//
// WHAT IT DOES NOT DO
// -------------------
// No game detection, no legacy rumble, no automatic normalisation, no hidden
// second gain. The numbers reported back are the ones that were played. The
// limiter stays on, because it is what the game path has and removing it
// would make the comparison a comparison with something else.

#include "sekiro_haptics/AudioClipLoader.hpp"
#include "sekiro_haptics/DualSenseAudioDevice.hpp"
#include "sekiro_haptics/GuardCue.hpp"
#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"
#include "sekiro_haptics/dualsense/DualSenseOutputState.hpp"
#include "sekiro_haptics/lab/LayeredPreset.hpp"
#include "sekiro_haptics/lab/ToneBurst.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sekiro_haptics;
using sekiro_haptics::lab::ToneBurstSpec;
using sekiro_haptics::lab::LayeredPresetSpec;
using sekiro_haptics::lab::WaveformLayer;

namespace {

constexpr std::uint32_t kExportRate = 48'000;

std::int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// --- the smallest JSON writer that answers this protocol -------------------
//
// Deliberately not a parser: everything coming IN is `key=value` tokens, so
// nothing here has to deal with arbitrary JSON. Output is JSON because the
// GUI side is Python and json.loads is one line there.
class Json {
public:
    Json& Str(const std::string& k, const std::string& v) {
        auto& out = Key(k);
        out << '"';
        for (char c : v) {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c == '\n') out << "\\n";
            else out << c;
        }
        out << '"';
        return *this;
    }
    Json& Num(const std::string& k, double v) {
        auto& out = Key(k);
        if (std::isfinite(v)) out << std::setprecision(6) << std::fixed << v;
        else out << "null";
        return *this;
    }
    Json& Int(const std::string& k, long long v) { Key(k) << v; return *this; }
    Json& Bool(const std::string& k, bool v) { Key(k) << (v ? "true" : "false"); return *this; }
    /// Raw, for a nested object or array that is already rendered.
    Json& Raw(const std::string& k, const std::string& v) { Key(k) << v; return *this; }

    std::string Done() { return "{" + out_.str() + "}"; }

private:
    std::ostringstream& Key(const std::string& k) {
        if (!first_) out_ << ',';
        first_ = false;
        out_ << '"' << k << "\":";
        return out_;
    }
    std::ostringstream out_;
    bool first_ = true;
};

void Reply(const std::string& json) {
    std::cout << json << std::endl;   // flushed: the GUI reads line by line
}

// --- command parsing -------------------------------------------------------

struct Command {
    std::string verb;
    std::vector<std::pair<std::string, std::string>> args;

    const std::string* Find(const std::string& key) const {
        for (const auto& [k, v] : args)
            if (k == key) return &v;
        return nullptr;
    }
    bool Number(const std::string& key, float& out) const {
        const auto* raw = Find(key);
        if (!raw) return false;
        try {
            out = std::stof(*raw);
        } catch (...) {
            return false;
        }
        return std::isfinite(out);
    }
};

Command ParseCommand(const std::string& line) {
    Command command;
    std::istringstream in(line);
    in >> command.verb;
    std::string token;
    while (in >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) continue;
        command.args.emplace_back(token.substr(0, eq), token.substr(eq + 1));
    }
    return command;
}

// --- who else is driving this controller -----------------------------------
//
// The live app owns the same HID device and the same audio endpoint. Two
// processes writing output reports to one controller means each one's idea of
// the device state is wrong half the time -- the trigger effect one of them
// applied disappears when the other submits, and neither is misbehaving.
//
// So this looks for the live app and says so, rather than producing a session
// where nothing quite works for a reason that is invisible from inside it.
// It only READS the process list; the live app is not touched in any way.
std::string FindConflictingProcess() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::string found;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring name = entry.szExeFile;
            if (name == L"sekiro_guard_feedback.exe" || name == L"sekiro_trigger_lab.exe") {
                found = Narrow(name);
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

/// One lab at a time, for the same reason.
class SingleInstance {
public:
    SingleInstance() {
        handle_ = CreateMutexW(nullptr, TRUE, L"Local\\SekiroHapticLabSingleInstance");
        held_ = handle_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    ~SingleInstance() {
        if (handle_) {
            if (held_) ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }
    bool Held() const { return held_; }

private:
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

// --- clip lifetime ---------------------------------------------------------
//
// DualSenseAudioDevice::Queue REFERENCES the clip rather than copying it, so
// a buffer must stay alive until its voice has finished. Freeing it on the
// next burst would be a use-after-free the render thread reads through, which
// is not the kind of bug a hand-comparison tool should be able to have.
//
// So every generated burst is kept until well past its own length. The bound
// is time, not a count: a 1000 ms burst repeating every 60 ms needs many more
// live buffers than a 20 ms one, and a fixed count would be wrong for one of
// the two.
class ClipKeeper {
public:
    std::shared_ptr<std::vector<float>> Keep(std::vector<float> clip, float lifetimeMs) {
        auto held = std::make_shared<std::vector<float>>(std::move(clip));
        std::lock_guard<std::mutex> lock(mutex_);
        held_.push_back({held, NowMs() + static_cast<std::int64_t>(lifetimeMs) + 1000});
        while (!held_.empty() && held_.front().expiresAtMs < NowMs()) held_.pop_front();
        return held;
    }
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        held_.clear();
    }

private:
    struct Held {
        std::shared_ptr<std::vector<float>> clip;
        std::int64_t expiresAtMs = 0;
    };
    std::mutex mutex_;
    std::deque<Held> held_;
};

// --- the output side -------------------------------------------------------

class HapticLab {
public:
    bool Start(int endpointIndex, int hapticLeft, int hapticRight, int speakerChannel,
               const GuardCueConfig& config, std::string& error) {
        hapticLeft_ = hapticLeft;
        hapticRight_ = hapticRight;
        speakerChannel_ = speakerChannel;

        const auto endpoints = DualSenseAudioDevice::Enumerate();
        if (endpointIndex < 0 || endpointIndex >= static_cast<int>(endpoints.size())) {
            error = "audio endpoint index " + std::to_string(endpointIndex) + " is out of range";
            return false;
        }
        endpointName_ = Narrow(endpoints[static_cast<std::size_t>(endpointIndex)].name);
        endpointId_ = Narrow(endpoints[static_cast<std::size_t>(endpointIndex)].id);
        if (!device_.Open(endpoints[static_cast<std::size_t>(endpointIndex)].id)) {
            error = device_.Error();
            return false;
        }
        // The same mixer settings the game path uses. The limiter stays ON:
        // the question this tool answers is what the game will actually put
        // out, and that includes the limiter.
        device_.SetVoiceLimit(config.voiceLimit, config.retireFadeMs);
        device_.SetSpeakerChannel(speakerChannel);
        LimiterSettings limiter;
        limiter.threshold = config.limiterThreshold;
        limiter.speakerHeadroom = config.speakerHeadroom;
        limiter.hapticHeadroom = config.hapticHeadroom;
        device_.SetLimiter(limiter);

        if (!device_.StartRenderThread()) {
            error = "could not start the render thread: " + device_.Error();
            return false;
        }

        // The controller's own audio routing, claimed once. MarkPcmHapticsActive
        // is bookkeeping: it does NOT set HAPTICS_SELECT, which would switch
        // the actuators to classic rumble and take them off the PCM path this
        // whole tool is about.
        hidState_.MarkPcmHapticsActive(true);
        const auto candidates = transport_.EnumerateCandidates();
        if (!candidates.empty() &&
            transport_.Open(candidates.front().path) == TransportResult::Success) {
            dualsense::AudioOutputSettings audio;
            audio.speakerVolume = static_cast<std::uint8_t>(
                std::clamp(config.device.controllerSpeakerVolume, 0, 255));
            audio.headphoneVolume = audio.speakerVolume;
            audio.outputPath = static_cast<std::uint8_t>(std::clamp(config.device.audioOutputPath, 0, 3));
            audio.speakerPreGain = static_cast<std::uint8_t>(std::clamp(config.device.speakerPreGain, 0, 7));
            hidState_.SetAudio(audio);
            routed_ = hidState_.Submit(transport_) == TransportResult::Success;
        }

        triggers_ = std::make_unique<dualsense::AdaptiveTriggerRuntime>(transport_, hidState_);

        repeatThread_ = std::thread([this] { RepeatLoop(); });
        return true;
    }

    /// Everything stops and nothing is left driving the actuators. Called on
    /// `stop`, on `quit`, and from the destructor, so closing the window or
    /// killing the GUI cannot leave the controller buzzing.
    void Silence() {
        repeating_ = false;
        // Fade rather than cut: dropping a live voice mid-cycle is a step to
        // zero, which the actuator renders as a tick.
        device_.DuckActive(0.0f, 8.0f);
        device_.DropPending();
    }

    void Shutdown() {
        Silence();
        // Triggers are released before the transport closes, so a held effect
        // cannot outlive the process that applied it.
        if (triggers_) {
            triggers_->ResetToNeutral(NowMs() * 1000);
            triggers_.reset();
        }
        running_ = false;
        wake_.notify_all();
        if (repeatThread_.joinable()) repeatThread_.join();
        device_.StopRenderThread();
        device_.Close();
        clips_.Clear();
        if (transport_.IsOpen()) {
            hidState_.ResetToNeutral();
            hidState_.Submit(transport_, /*force=*/true);
            transport_.Close();
        }
    }

    ~HapticLab() { Shutdown(); }

    // ---- layered preset --------------------------------------------------
    //
    // Held here and rendered on demand. A render is offline arithmetic over a
    // few hundred thousand samples -- microseconds to a couple of
    // milliseconds -- so it happens on the command thread and never on the
    // render thread, which is the rule the whole output path is built on.
    LayeredPresetSpec& Preset() { return preset_; }
    const LayeredPresetSpec& Preset() const { return preset_; }

    std::shared_ptr<lab::RenderedPreset> RenderPreset() {
        auto rendered = std::make_shared<lab::RenderedPreset>(
            lab::RenderLayeredPreset(preset_, device_.Format().sampleRate));
        if (rendered->ok) lastRender_ = rendered;
        return rendered;
    }

    /// Queue the rendered preset. Both sides go out as ONE pair of voices, so
    /// every layer's start time is already baked in at sample resolution --
    /// nothing here schedules anything.
    bool PlayPreset(std::string& error) {
        auto rendered = RenderPreset();
        if (!rendered->ok) { error = rendered->error; return false; }
        const float lifetime = preset_.LengthMs();
        const auto heldLeft = clips_.Keep(rendered->left, lifetime);
        const auto heldRight = clips_.Keep(rendered->right, lifetime);
        bool queued = false;
        // Voice gain 1.0: masterGain and the layer gains are already in the
        // samples, so a second multiplier here would be a hidden one.
        if (hapticLeft_ >= 0) queued = device_.Queue(*heldLeft, hapticLeft_, 1.0f) || queued;
        if (hapticRight_ >= 0) queued = device_.Queue(*heldRight, hapticRight_, 1.0f) || queued;

        // The speaker cue goes out in the SAME queueing, between the same two
        // pumps, so both start on the same frame. Its delay is leading silence
        // in the buffer rather than a sleep or a timer -- the same sample-clock
        // rule the layers follow, and the only way "20 ms after the hit" means
        // the same thing twice.
        if (speakerEnabled_ && !speakerClip_.empty() && speakerChannel_ >= 0) {
            const auto lead = static_cast<std::size_t>(
                static_cast<double>(speakerOffsetMs_) / 1000.0 * device_.Format().sampleRate);
            std::vector<float> cue(lead + speakerClip_.size(), 0.0f);
            std::copy(speakerClip_.begin(), speakerClip_.end(), cue.begin() + lead);
            const auto heldCue = clips_.Keep(std::move(cue),
                                             lifetime + speakerOffsetMs_ + 2000.0f);
            queued = device_.Queue(*heldCue, speakerChannel_, speakerGain_) || queued;
        }
        if (!queued) error = "햅틱 채널이 버스트를 받지 못했습니다";
        return queued;
    }

    std::shared_ptr<lab::RenderedPreset> LastRender() const { return lastRender_; }

    const LimiterSettings& Limiter() const { return device_.Limiter(); }

    // ---- speaker cue -----------------------------------------------------
    //
    // The controller's built-in speaker, on its own channel, playing an
    // ordinary WAV alongside the haptic preset. This is how the parry/block
    // sounds the game path already uses can be heard while a candidate
    // vibration is felt -- judging a waveform against silence is judging a
    // different thing from what the game will do.
    //
    // It is a SEPARATE voice on a SEPARATE channel, not mixed into the
    // haptic buffer: the speaker and the actuators are different transducers
    // with their own limiter, and summing them would make each one's
    // measurements meaningless.
    std::string LoadSpeakerClip(const std::string& path, std::string& error) {
        AudioClipInfo info;
        auto clip = LoadAudioClipMono(path, device_.Format().sampleRate, info);
        if (!info.ok) { error = info.error; return {}; }
        speakerClip_ = std::move(clip);
        speakerPath_ = path;
        std::ostringstream report;
        report << info.frames * 1000 / (std::max)(1u, device_.Format().sampleRate) << " ms, "
               << info.sourceSampleRate << " Hz/" << info.sourceChannels << "ch, peak "
               << info.peak;
        return report.str();
    }

    void ClearSpeakerClip() {
        speakerClip_.clear();
        speakerPath_.clear();
    }

    void SetSpeaker(float gain, float offsetMs, bool enabled) {
        speakerGain_ = std::clamp(gain, 0.0f, 4.0f);
        speakerOffsetMs_ = std::clamp(offsetMs, 0.0f, 2000.0f);
        speakerEnabled_ = enabled;
    }

    std::string SpeakerJson() const {
        const auto rate = (std::max)(1u, device_.Format().sampleRate);
        Json j;
        j.Str("path", speakerPath_)
            .Bool("loaded", !speakerClip_.empty())
            .Bool("enabled", speakerEnabled_)
            .Num("gain", speakerGain_)
            .Num("offsetMs", speakerOffsetMs_)
            .Int("channel", speakerChannel_)
            .Int("frames", static_cast<long long>(speakerClip_.size()))
            .Num("durationMs", static_cast<double>(speakerClip_.size()) * 1000.0 / rate);
        return j.Done();
    }

    /// The clip's shape for drawing, as min/max per bucket -- the same form
    /// the layer traces use, and for the same reason (every Nth sample draws
    /// a moire of a different amplitude at audio rates).
    ///
    /// Scaled by the speaker gain, so what is drawn is what will be played.
    /// It is NOT put in SpeakerJson: that one is saved into every preset
    /// file, and a few hundred numbers per file would be noise on disk.
    std::string SpeakerEnvelopeJson(std::size_t buckets) const {
        if (speakerClip_.empty()) return "[]";
        buckets = std::clamp<std::size_t>(buckets, 32, 4000);
        const auto bucket = (std::max)(std::size_t{1},
                                       (speakerClip_.size() + buckets - 1) / buckets);
        std::ostringstream out;
        out << '[';
        bool first = true;
        for (std::size_t start = 0; start < speakerClip_.size(); start += bucket) {
            const auto stop = (std::min)(speakerClip_.size(), start + bucket);
            float lo = speakerClip_[start], hi = speakerClip_[start];
            for (std::size_t i = start; i < stop; ++i) {
                lo = (std::min)(lo, speakerClip_[i]);
                hi = (std::max)(hi, speakerClip_[i]);
            }
            if (!first) out << ',';
            first = false;
            out << '[' << std::setprecision(4) << std::fixed << lo * speakerGain_
                << ',' << hi * speakerGain_ << ']';
        }
        out << ']';
        return out.str();
    }

    // ---- adaptive triggers ----------------------------------------------
    //
    // Through the SAME DualSenseOutputState that holds the audio routing. That
    // is the whole point of a single owner: applying a trigger effect must not
    // rewrite the speaker/output-path bytes, and releasing one must not take
    // the actuators off PCM.
    dualsense::AdaptiveTriggerRuntime& Triggers() { return *triggers_; }
    bool TriggersReady() const { return triggers_ != nullptr; }

    void SetSpec(const ToneBurstSpec& spec) {
        std::lock_guard<std::mutex> lock(mutex_);
        target_ = spec;
        // Frequency, length and the fades define the SHAPE of a burst, so
        // they take effect on the next one -- there is nothing to interpolate
        // part way through a cycle that has already been generated. Amplitude
        // and the two channel gains are interpolated instead (see Blend).
        applied_.frequencyHz = spec.frequencyHz;
        applied_.lengthMs = spec.lengthMs;
        applied_.fadeInMs = spec.fadeInMs;
        applied_.fadeOutMs = spec.fadeOutMs;
    }

    ToneBurstSpec Target() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_;
    }

    /// One burst, now.
    bool PlayOnce(std::string& error) {
        ToneBurstSpec spec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A single shot is not a slider drag, so it plays exactly what is
            // on screen rather than an interpolated value on the way there.
            applied_ = target_;
            spec = target_;
        }
        return Emit(spec, error);
    }

    void StartRepeat(float intervalMs) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            intervalMs_ = std::clamp(intervalMs, 20.0f, 5000.0f);
        }
        repeating_ = true;
        wake_.notify_all();
    }

    bool Repeating() const { return repeating_; }
    float IntervalMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return intervalMs_;
    }

    /// What the mixer did with it. Reset by `resetstats` so a measurement can
    /// be pinned to one burst rather than to everything since startup.
    AudioRenderStats Stats() const { return device_.RenderStats(); }
    void ResetStats() { device_.ResetRenderStats(); }

    std::uint32_t SampleRate() const { return device_.Format().sampleRate; }
    unsigned Channels() const { return device_.Format().channels; }
    const std::string& EndpointName() const { return endpointName_; }
    const std::string& EndpointId() const { return endpointId_; }
    bool Routed() const { return routed_; }
    int HapticLeft() const { return hapticLeft_; }
    int HapticRight() const { return hapticRight_; }

    /// Capture what is actually submitted to WASAPI, so the "did it get
    /// weaker on the way out" question has a measured answer instead of an
    /// inferred one.
    void BeginCapture(std::size_t frames) { device_.BeginSubmitCapture(frames); }
    void EndCapture() { device_.EndSubmitCapture(); }
    std::vector<float> CaptureChannel(int channel) const {
        const auto& interleaved = device_.SubmitCapture();
        const unsigned channels = device_.SubmitCaptureChannels();
        std::vector<float> out;
        if (channels == 0 || channel < 0 || channel >= static_cast<int>(channels)) return out;
        out.reserve(interleaved.size() / channels);
        for (std::size_t f = 0; f * channels + static_cast<unsigned>(channel) < interleaved.size(); ++f)
            out.push_back(interleaved[f * channels + static_cast<unsigned>(channel)]);
        return out;
    }

private:
    /// Move the click-prone parameters toward the target instead of jumping.
    ///
    /// Amplitude and the channel gains are level. Stepping level between two
    /// bursts during a drag is heard and felt as a series of jumps, and the
    /// person dragging reads that as a property of the waveform. A short
    /// exponential approach over ~120 ms makes a drag feel like one change.
    /// Everything else is already applied whole in SetSpec.
    void Blend(std::int64_t elapsedMs) {
        const float alpha = 1.0f - std::exp(-static_cast<float>(elapsedMs) / 120.0f);
        applied_.amplitude += (target_.amplitude - applied_.amplitude) * alpha;
        applied_.leftGain += (target_.leftGain - applied_.leftGain) * alpha;
        applied_.rightGain += (target_.rightGain - applied_.rightGain) * alpha;
    }

    bool Emit(const ToneBurstSpec& spec, std::string& error) {
        error = lab::ValidateToneBurst(spec);
        if (!error.empty()) return false;
        std::vector<float> left, right;
        lab::RenderToneBurst(spec, device_.Format().sampleRate, left, right);
        if (left.empty()) { error = "rendered to nothing"; return false; }

        const auto heldLeft = clips_.Keep(std::move(left), spec.lengthMs);
        const auto heldRight = clips_.Keep(std::move(right), spec.lengthMs);
        // Two separate voices, one per actuator, at voice gain 1.0. The gain
        // is 1.0 on purpose: the per-side scale is already in the waveform, so
        // a second multiplier here would be exactly the hidden double gain
        // this tool must not have.
        bool queued = false;
        if (hapticLeft_ >= 0) queued = device_.Queue(*heldLeft, hapticLeft_, 1.0f) || queued;
        if (hapticRight_ >= 0) queued = device_.Queue(*heldRight, hapticRight_, 1.0f) || queued;
        if (!queued) error = "no haptic channel accepted the burst";
        return queued;
    }

    void RepeatLoop() {
        std::int64_t lastMs = NowMs();
        while (running_) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!repeating_) {
                wake_.wait_for(lock, std::chrono::milliseconds(100));
                lastMs = NowMs();
                continue;
            }
            const auto interval = intervalMs_;
            const auto now = NowMs();
            Blend(std::max<std::int64_t>(1, now - lastMs));
            lastMs = now;
            const auto spec = applied_;
            lock.unlock();

            std::string error;
            Emit(spec, error);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(std::max(20.0f, interval))));
        }
    }

    DualSenseAudioDevice device_;
    /// The transport logs every enumerate/open/write. stdout is the command
    /// channel and must carry nothing but JSON, so its log goes to stderr --
    /// where it is still readable when something is wrong, without the GUI
    /// having to sift protocol out of diagnostics.
    HidApiDualSenseTransport transport_{std::cerr};
    dualsense::DualSenseOutputState hidState_;
    ClipKeeper clips_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    ToneBurstSpec target_;
    ToneBurstSpec applied_;
    float intervalMs_ = 400.0f;

    std::atomic<bool> running_{true};
    std::atomic<bool> repeating_{false};
    std::thread repeatThread_;

    int hapticLeft_ = -1, hapticRight_ = -1;
    std::string endpointName_;
    std::string endpointId_;
    bool routed_ = false;

    LayeredPresetSpec preset_;
    std::vector<float> speakerClip_;
    std::string speakerPath_;
    float speakerGain_ = 1.0f;
    float speakerOffsetMs_ = 0.0f;
    bool speakerEnabled_ = true;
    int speakerChannel_ = -1;
    std::shared_ptr<lab::RenderedPreset> lastRender_;
    std::unique_ptr<dualsense::AdaptiveTriggerRuntime> triggers_;
};

// --- export ----------------------------------------------------------------

bool WriteStereoWavFloat32(const std::string& path, const std::vector<float>& interleaved,
                           std::uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto dataBytes = static_cast<std::uint32_t>(interleaved.size() * 4);
    auto u32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    // Format 3 = IEEE float. The samples are written EXACTLY as generated --
    // no conversion to 16-bit, because a rounded export is not the waveform
    // that was compared.
    out.write("fmt ", 4); u32(16); u16(3); u16(2);
    u32(rate); u32(rate * 2 * 4); u16(2 * 4); u16(32);
    out.write("data", 4); u32(dataBytes);
    out.write(reinterpret_cast<const char*>(interleaved.data()),
              static_cast<std::streamsize>(dataBytes));
    return out.good();
}

std::string SpecJson(const ToneBurstSpec& spec) {
    Json j;
    j.Num("frequencyHz", spec.frequencyHz)
        .Num("amplitude", spec.amplitude)
        .Num("lengthMs", spec.lengthMs)
        .Num("fadeInMs", spec.fadeInMs)
        .Num("fadeOutMs", spec.fadeOutMs)
        .Num("leftGain", spec.leftGain)
        .Num("rightGain", spec.rightGain);
    return j.Done();
}

std::string StatsJson(const lab::ToneBurstStats& stats) {
    Json j;
    j.Int("frames", static_cast<long long>(stats.frames))
        .Num("peakLeft", stats.peakLeft)
        .Num("peakRight", stats.peakRight)
        .Num("rmsLeft", stats.rmsLeft)
        .Num("rmsRight", stats.rmsRight);
    return j.Done();
}

std::string StatsJson(const lab::SignalStats& stats) {
    Json j;
    j.Int("frames", static_cast<long long>(stats.frames))
        .Num("peak", stats.peak)
        .Num("rms", stats.rms)
        .Num("mean", stats.mean)
        .Int("overRange", static_cast<long long>(stats.overRange));
    return j.Done();
}

std::string LayerJson(const WaveformLayer& layer) {
    Json j;
    j.Str("name", layer.name)
        .Bool("muted", layer.muted)
        .Bool("solo", layer.solo)
        .Num("startMs", layer.startMs)
        .Num("durationMs", layer.durationMs)
        .Str("waveform", lab::ToString(layer.waveform))
        .Num("frequencyHz", layer.frequencyHz)
        .Num("frequencyEndHz", layer.frequencyEndHz)
        .Num("modulationHz", layer.modulationHz)
        .Num("modulationDepth", layer.modulationDepth)
        .Num("decayPerSecond", layer.decayPerSecond)
        .Num("amplitude", layer.amplitude)
        .Num("gainDb", layer.gainDb)
        .Num("startPhaseDeg", layer.startPhaseDeg)
        .Num("leftGain", layer.leftGain)
        .Num("rightGain", layer.rightGain)
        .Bool("envelopeEnabled", layer.envelope.enabled)
        .Num("attackMs", layer.envelope.attackMs)
        .Num("holdMs", layer.envelope.holdMs)
        .Num("decayMs", layer.envelope.decayMs)
        .Num("sustain", layer.envelope.sustain)
        .Num("releaseMs", layer.envelope.releaseMs)
        .Str("formula", layer.formula)
        .Num("t0Seconds", layer.t0Seconds)
        .Bool("formulaTimeFromLayerStart", layer.formulaTimeFromLayerStart);
    return j.Done();
}

std::string PresetJson(const LayeredPresetSpec& preset, const std::string& speaker = "null") {
    std::ostringstream layers;
    layers << '[';
    for (std::size_t i = 0; i < preset.layers.size(); ++i) {
        if (i) layers << ',';
        layers << LayerJson(preset.layers[i]);
    }
    layers << ']';
    Json j;
    // schemaVersion first, and bumped whenever a field's MEANING changes --
    // a file that loads but means something else is worse than one that is
    // refused.
    j.Int("schemaVersion", 1)
        .Str("name", preset.name)
        .Num("masterGain", preset.masterGain)
        .Num("saturationCeiling", preset.saturationCeiling)
        .Num("lengthMs", preset.LengthMs())
        .Raw("speaker", speaker)
        .Raw("layers", layers.str());
    return j.Done();
}

/// Min/max per bucket -- see the `wave` command for why picking every Nth
/// sample is wrong here.
std::string EnvelopeJson(const std::vector<float>& samples, std::size_t bucket) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (std::size_t start = 0; start < samples.size(); start += bucket) {
        const auto stop = (std::min)(samples.size(), start + bucket);
        float lo = samples[start], hi = samples[start];
        for (std::size_t i = start; i < stop; ++i) {
            lo = (std::min)(lo, samples[i]);
            hi = (std::max)(hi, samples[i]);
        }
        if (!first) out << ',';
        first = false;
        out << '[' << std::setprecision(4) << std::fixed << lo << ',' << hi << ']';
    }
    out << ']';
    return out.str();
}

bool LayerFromCommand(const Command& command, WaveformLayer& layer, std::string& error) {
    float v = 0.0f;
    if (const auto* name = command.Find("name")) layer.name = *name;
    if (const auto* waveform = command.Find("type")) {
        if (!lab::ParseWaveform(*waveform, layer.waveform)) {
            error = "알 수 없는 파형 \"" + *waveform + "\"";
            return false;
        }
    }
    if (command.Number("start", v)) layer.startMs = v;
    if (command.Number("dur", v)) layer.durationMs = v;
    if (command.Number("freq", v)) layer.frequencyHz = v;
    if (command.Number("freq2", v)) layer.frequencyEndHz = v;
    if (command.Number("modhz", v)) layer.modulationHz = v;
    if (command.Number("moddepth", v)) layer.modulationDepth = v;
    if (command.Number("decayrate", v)) layer.decayPerSecond = v;
    if (command.Number("amp", v)) layer.amplitude = v;
    if (command.Number("gaindb", v)) layer.gainDb = v;
    if (command.Number("phase", v)) layer.startPhaseDeg = v;
    if (command.Number("left", v)) layer.leftGain = v;
    if (command.Number("right", v)) layer.rightGain = v;
    if (command.Number("mute", v)) layer.muted = v != 0.0f;
    if (command.Number("solo", v)) layer.solo = v != 0.0f;
    if (command.Number("env", v)) layer.envelope.enabled = v != 0.0f;
    if (command.Number("atk", v)) layer.envelope.attackMs = v;
    if (command.Number("hold", v)) layer.envelope.holdMs = v;
    if (command.Number("dec", v)) layer.envelope.decayMs = v;
    if (command.Number("sus", v)) layer.envelope.sustain = v;
    if (command.Number("rel", v)) layer.envelope.releaseMs = v;
    if (command.Number("t0", v)) layer.t0Seconds = v;
    if (command.Number("localt", v)) layer.formulaTimeFromLayerStart = v != 0.0f;
    return true;
}

ToneBurstSpec SpecFromCommand(const Command& command, ToneBurstSpec spec) {
    float v = 0.0f;
    if (command.Number("freq", v)) spec.frequencyHz = v;
    if (command.Number("amp", v)) spec.amplitude = v;
    if (command.Number("len", v)) spec.lengthMs = v;
    if (command.Number("fadein", v)) spec.fadeInMs = v;
    if (command.Number("fadeout", v)) spec.fadeOutMs = v;
    if (command.Number("left", v)) spec.leftGain = v;
    if (command.Number("right", v)) spec.rightGain = v;
    return spec;
}

/// Drop the silence a capture has around the burst, so both sides of the
/// comparison cover the same span.
///
/// The onset is found on whichever side is louder -- a burst sent to one
/// actuator only is a normal thing to try here, and keying on a channel that
/// was meant to be silent would find nothing and leave the window unmoved.
/// Both sides are then cut at the SAME point, because the comparison between
/// them is part of what is being measured.
void AlignToBurst(std::vector<float>& left, std::vector<float>& right, std::size_t frames) {
    if (frames == 0) return;
    const auto& loudest = [&]() -> const std::vector<float>& {
        float peakLeft = 0.0f, peakRight = 0.0f;
        for (float v : left) peakLeft = std::max(peakLeft, std::fabs(v));
        for (float v : right) peakRight = std::max(peakRight, std::fabs(v));
        return peakLeft >= peakRight ? left : right;
    }();
    // Relative to that peak, not an absolute number: a burst at amplitude
    // 0.02 is a legitimate thing to compare, and a fixed threshold would
    // decide it never started.
    float peak = 0.0f;
    for (float v : loudest) peak = std::max(peak, std::fabs(v));
    if (peak <= 0.0f) return;                   // nothing was captured; say so by not lying
    const float onset = peak * 0.01f;

    std::size_t first = 0;
    while (first < loudest.size() && std::fabs(loudest[first]) <= onset) ++first;
    if (first >= loudest.size()) return;

    auto cut = [&](std::vector<float>& v) {
        if (first >= v.size()) { v.clear(); return; }
        v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(first));
        if (v.size() > frames) v.resize(frames);
    };
    cut(left);
    cut(right);
}

} // namespace

int main(int argc, char** argv) {
    int endpointIndex = -1, hapticLeft = -1, hapticRight = -1, speakerChannel = -1;
    bool force = false;
    std::string endpointId;
    std::string configPath = "config/guard_cues.json";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--audio") endpointIndex = std::atoi(next().c_str());
        else if (a == "--haptic-left") hapticLeft = std::atoi(next().c_str());
        else if (a == "--haptic-right") hapticRight = std::atoi(next().c_str());
        else if (a == "--config") configPath = next();
        else if (a == "--force") force = true;
        // The STABLE id, preferred over --audio: a list index moves whenever
        // anything else is plugged in or removed, and a saved setting that
        // names one would silently point at a different device.
        else if (a == "--audio-id") endpointId = next();
        else if (a == "--list") {
            const auto endpoints = DualSenseAudioDevice::Enumerate();
            for (std::size_t e = 0; e < endpoints.size(); ++e)
                std::cout << "[" << e << "] " << Narrow(endpoints[e].name) << "\n";
            return 0;
        } else {
            std::cout << "haptic_lab -- the output half of the PCM haptic bench.\n"
                         "  --audio N --haptic-left K --haptic-right K --config PATH\n"
                         "  --list    audio render endpoints\n"
                         "Reads one command per line on stdin; answers one JSON object per\n"
                         "line on stdout. Intended to be driven by tools/haptic_lab_gui.py.\n";
            return a == "--help" ? 0 : 2;
        }
    }

    // The verified endpoint and channels come from the same config the game
    // path reads. They were established by feeling which channel buzzed in
    // which palm, so guessing them here would throw that away.
    GuardCueConfig config = DefaultGuardCueConfig();
    std::string configSource = "built-in defaults";
    if (std::ifstream in(configPath); in) {
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string error;
        if (ParseGuardCueConfig(buffer.str(), config, error)) configSource = configPath;
        else configSource = configPath + " (rejected: " + error + ")";
    }
    if (!endpointId.empty()) {
        const auto endpoints = DualSenseAudioDevice::Enumerate();
        for (std::size_t e = 0; e < endpoints.size(); ++e)
            if (Narrow(endpoints[e].id) == endpointId) { endpointIndex = static_cast<int>(e); break; }
        if (endpointIndex < 0) {
            Reply(Json().Str("event", "error")
                      .Str("message", "저장된 장치를 찾지 못했습니다: " + endpointId).Done());
            return 1;
        }
    }
    if (endpointIndex < 0) endpointIndex = config.device.audioEndpointIndex;
    if (hapticLeft < 0) hapticLeft = config.device.hapticLeftChannel;
    if (hapticRight < 0) hapticRight = config.device.hapticRightChannel;
    if (speakerChannel < 0) speakerChannel = config.device.speakerChannel;

    // Refuse to fight the live app over the same controller, unless told to.
    // Two owners of one HID device is a session where effects vanish for no
    // visible reason, so it is reported instead of discovered.
    SingleInstance instance;
    if (!instance.Held() && !force) {
        Reply(Json().Str("event", "error")
                  .Str("message", "haptic_lab 이 이미 실행 중입니다 (--force 로 무시)").Done());
        return 1;
    }
    if (const auto conflict = FindConflictingProcess(); !conflict.empty() && !force) {
        Reply(Json().Str("event", "error")
                  .Str("message", conflict + " 이(가) 같은 컨트롤러를 사용 중입니다. "
                                            "종료한 뒤 다시 실행하세요 (--force 로 무시)")
                  .Done());
        return 1;
    }

    HapticLab lab;
    std::string error;
    if (!lab.Start(endpointIndex, hapticLeft, hapticRight, speakerChannel, config, error)) {
        Reply(Json().Str("event", "error").Str("message", error).Done());
        return 1;
    }

    Reply(Json()
              .Str("event", "ready")
              .Str("endpoint", lab.EndpointName())
              .Int("endpointIndex", endpointIndex)
              .Str("endpointId", lab.EndpointId())
              .Int("sampleRate", lab.SampleRate())
              .Int("channels", lab.Channels())
              .Int("hapticLeft", lab.HapticLeft())
              .Int("hapticRight", lab.HapticRight())
              .Bool("routingClaimed", lab.Routed())
              .Str("config", configSource)
              .Done());

    std::string line;
    while (std::getline(std::cin, line)) {
        const auto command = ParseCommand(line);
        if (command.verb.empty()) continue;

        if (command.verb == "quit") break;

        if (command.verb == "set") {
            const auto spec = SpecFromCommand(command, lab.Target());
            const auto why = lab::ValidateToneBurst(spec);
            if (!why.empty()) {
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            lab.SetSpec(spec);
            Reply(Json().Str("event", "set").Raw("spec", SpecJson(spec)).Done());
            continue;
        }

        if (command.verb == "play") {
            std::string why;
            if (!lab.PlayOnce(why)) {
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            Reply(Json().Str("event", "played").Done());
            continue;
        }

        if (command.verb == "repeat") {
            float interval = lab.IntervalMs();
            command.Number("interval", interval);
            lab.StartRepeat(interval);
            Reply(Json().Str("event", "repeating").Num("intervalMs", interval).Done());
            continue;
        }

        if (command.verb == "stop") {
            lab.Silence();
            Reply(Json().Str("event", "stopped").Done());
            continue;
        }

        if (command.verb == "resetstats") {
            lab.ResetStats();
            Reply(Json().Str("event", "statsreset").Done());
            continue;
        }

        // The generated waveform for the GUI to draw, as MIN/MAX PER BUCKET
        // rather than every Nth sample.
        //
        // Taking every Nth sample is what a plot like this usually does, and
        // it is wrong here: a 500 Hz burst over 1000 ms is 500 cycles, a
        // canvas is a few hundred pixels wide, and picking one sample per
        // bucket draws a slow moire pattern that looks like a completely
        // different waveform at a completely different amplitude. A min/max
        // pair per bucket cannot do that -- the drawn extent is the real
        // extent. When a bucket is one sample the two are equal and this
        // degenerates to the exact waveform, which is what happens at the low
        // frequencies and short lengths where the shape is worth seeing.
        if (command.verb == "wave") {
            float points = 560.0f;
            command.Number("points", points);
            const auto spec = lab.Target();
            std::vector<float> left, right;
            lab::RenderToneBurst(spec, kExportRate, left, right);
            const auto stats = lab::MeasureToneBurst(left, right);

            const auto buckets = static_cast<std::size_t>(std::clamp(points, 16.0f, 4000.0f));
            const auto bucket = std::max<std::size_t>(1, (left.size() + buckets - 1) / buckets);
            std::ostringstream body;
            auto emit = [&](const std::vector<float>& v) {
                std::ostringstream out;
                out << '[';
                bool firstBucket = true;
                for (std::size_t start = 0; start < v.size(); start += bucket) {
                    const auto stop = std::min(v.size(), start + bucket);
                    float lo = v[start], hi = v[start];
                    for (std::size_t i = start; i < stop; ++i) {
                        lo = std::min(lo, v[i]);
                        hi = std::max(hi, v[i]);
                    }
                    if (!firstBucket) out << ',';
                    firstBucket = false;
                    out << '[' << std::setprecision(4) << std::fixed << lo << ',' << hi << ']';
                }
                out << ']';
                return out.str();
            };
            Reply(Json()
                      .Str("event", "wave")
                      .Int("sampleRate", kExportRate)
                      .Int("samplesPerBucket", static_cast<long long>(bucket))
                      .Raw("generated", StatsJson(stats))
                      .Raw("left", emit(left))
                      .Raw("right", emit(right))
                      .Done());
            continue;
        }

        // What the mixer actually did. `peakHapticBeforeLimit` is the SUM of
        // whatever was sounding before the limiter; the reduction and clip
        // counts say whether anything was taken off it afterwards.
        if (command.verb == "stats") {
            const auto s = lab.Stats();
            Reply(Json()
                      .Str("event", "stats")
                      .Num("hapticPeakBeforeLimit", s.peakHapticBeforeLimit)
                      .Int("limitedHapticFrames", static_cast<long long>(s.limitedHapticFrames))
                      .Num("worstHapticReductionDb", s.worstHapticReductionDb)
                      .Int("clippedHapticSamples", static_cast<long long>(s.clippedHapticSamples))
                      .Int("voicesStarted", static_cast<long long>(s.voicesStarted))
                      .Int("voicesRetired", static_cast<long long>(s.voicesRetired))
                      .Int("voicesEvicted", static_cast<long long>(s.voicesEvicted))
                      .Int("activeVoices", static_cast<long long>(s.maxConcurrentVoices))
                      .Num("bufferMs", s.bufferMs)
                      .Done());
            continue;
        }

        // One burst, captured on the way to the device, measured with the
        // same function as the generated waveform. This is the answer to "is
        // it weaker than what I made" -- and it is still a measurement of
        // SAMPLES, not of how far the actuator moved.
        if (command.verb == "measure") {
            const auto spec = lab.Target();
            const auto frames = static_cast<std::size_t>(
                static_cast<double>(spec.lengthMs + 200.0f) / 1000.0 * lab.SampleRate());
            lab.ResetStats();
            lab.BeginCapture(frames);
            std::string why;
            if (!lab.PlayOnce(why)) {
                lab.EndCapture();
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(spec.lengthMs) + 250));
            auto submittedLeft = lab.CaptureChannel(lab.HapticLeft());
            auto submittedRight = lab.CaptureChannel(lab.HapticRight());
            lab.EndCapture();

            std::vector<float> left, right;
            lab::RenderToneBurst(spec, lab.SampleRate(), left, right);

            // Line the capture up with the burst before measuring it.
            //
            // The capture deliberately runs longer than the burst, so it
            // always contains silence before and after. Measuring RMS across
            // that window divides by the padding as well and reports a level
            // well under the generated one -- which reads as "the device made
            // it quieter", the exact wrong conclusion, from a tool whose only
            // job is to answer that question honestly.
            AlignToBurst(submittedLeft, submittedRight, left.size());
            const auto s = lab.Stats();
            Reply(Json()
                      .Str("event", "measure")
                      .Raw("generated", StatsJson(lab::MeasureToneBurst(left, right)))
                      .Raw("submitted",
                           StatsJson(lab::MeasureToneBurst(submittedLeft, submittedRight)))
                      .Num("hapticPeakBeforeLimit", s.peakHapticBeforeLimit)
                      .Int("limitedHapticFrames", static_cast<long long>(s.limitedHapticFrames))
                      .Num("worstHapticReductionDb", s.worstHapticReductionDb)
                      .Int("clippedHapticSamples", static_cast<long long>(s.clippedHapticSamples))
                      .Done());
            continue;
        }

        // The WAV is written from the SAME renderer that feeds the device, at
        // 48 kHz stereo float, so the exported file is the waveform that was
        // compared rather than a re-derivation of it.
        if (command.verb == "export") {
            const auto* path = command.Find("path");
            if (!path || path->empty()) {
                Reply(Json().Str("event", "rejected").Str("message", "export needs path=").Done());
                continue;
            }
            const auto spec = lab.Target();
            std::vector<float> left, right;
            lab::RenderToneBurst(spec, kExportRate, left, right);
            const auto interleaved = lab::InterleaveStereo(left, right);
            const bool wroteWav = WriteStereoWavFloat32(*path + ".wav", interleaved, kExportRate);
            bool wroteJson = false;
            if (std::ofstream json(*path + ".json"); json) {
                json << "{\"tool\":\"haptic_lab\",\"sampleRateHz\":" << kExportRate
                     << ",\"channels\":[\"haptic_left\",\"haptic_right\"],\"spec\":"
                     << SpecJson(spec) << ",\"generated\":"
                     << StatsJson(lab::MeasureToneBurst(left, right))
                     << ",\"note\":\"sample-value measurements, not vibration\"}\n";
                wroteJson = json.good();
            }
            Reply(Json()
                      .Str("event", "exported")
                      .Str("wav", wroteWav ? *path + ".wav" : std::string())
                      .Str("json", wroteJson ? *path + ".json" : std::string())
                      .Bool("ok", wroteWav && wroteJson)
                      .Done());
            continue;
        }

        // ---- layered preset ----------------------------------------------

        if (command.verb == "preset.clear") {
            lab.Preset() = LayeredPresetSpec{};
            lab.Preset().layers.clear();
            Reply(Json().Str("event", "preset").Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson())).Done());
            continue;
        }

        if (command.verb == "preset.name") {
            if (const auto* name = command.Find("name")) lab.Preset().name = *name;
            float gain = lab.Preset().masterGain;
            if (command.Number("master", gain)) lab.Preset().masterGain = gain;
            float ceiling = lab.Preset().saturationCeiling;
            if (command.Number("ceiling", ceiling)) lab.Preset().saturationCeiling = ceiling;
            // Only the two fields this command sets are checked. Validating the
            // WHOLE preset here was wrong: this is normally sent right after
            // preset.clear, when there are no layers yet, so every call was
            // refused as "no layers" and quietly reset the values it had just
            // been given. The layer commands validate the rest.
            if (!std::isfinite(lab.Preset().masterGain) || lab.Preset().masterGain < 0.0f ||
                lab.Preset().masterGain > 4.0f) {
                lab.Preset().masterGain = 1.0f;
                Reply(Json().Str("event", "rejected")
                          .Str("message", "masterGain 은 0~4 사이여야 합니다").Done());
                continue;
            }
            if (!std::isfinite(lab.Preset().saturationCeiling) ||
                lab.Preset().saturationCeiling < 0.0f || lab.Preset().saturationCeiling > 1.0f) {
                lab.Preset().saturationCeiling = 0.0f;
                Reply(Json().Str("event", "rejected")
                          .Str("message", "출력 포화 한계는 0(끔)~1 사이여야 합니다").Done());
                continue;
            }
            Reply(Json().Str("event", "preset").Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson())).Done());
            continue;
        }

        if (command.verb == "layer.add" || command.verb == "layer.set" ||
            command.verb == "layer.dup" || command.verb == "layer.remove") {
            auto& layers = lab.Preset().layers;
            // Kept so an edit that does not validate can be undone. The
            // alternative is storing it and refusing at render time, which
            // puts the complaint a long way from the field that caused it --
            // and leaves the preset in a state that cannot be played.
            const auto previous = layers;
            float indexValue = -1.0f;
            command.Number("index", indexValue);
            const auto index = static_cast<int>(indexValue);

            if (command.verb == "layer.add") {
                if (layers.size() >= 32) {
                    Reply(Json().Str("event", "rejected").Str("message", "레이어는 최대 32개입니다").Done());
                    continue;
                }
                WaveformLayer layer;
                layer.name = "레이어 " + std::to_string(layers.size() + 1);
                std::string why;
                if (!LayerFromCommand(command, layer, why)) {
                    Reply(Json().Str("event", "rejected").Str("message", why).Done());
                    continue;
                }
                layers.push_back(layer);
            } else {
                if (index < 0 || index >= static_cast<int>(layers.size())) {
                    Reply(Json().Str("event", "rejected").Str("message", "레이어 번호가 범위를 벗어났습니다").Done());
                    continue;
                }
                if (command.verb == "layer.remove") {
                    layers.erase(layers.begin() + index);
                } else if (command.verb == "layer.dup") {
                    auto copy = layers[static_cast<std::size_t>(index)];
                    copy.name += " 복사본";
                    copy.solo = false;   // a duplicate must not silence its original
                    layers.insert(layers.begin() + index + 1, copy);
                } else {
                    std::string why;
                    if (!LayerFromCommand(command, layers[static_cast<std::size_t>(index)], why)) {
                        Reply(Json().Str("event", "rejected").Str("message", why).Done());
                        continue;
                    }
                }
            }
            if (const auto why = lab::ValidateLayeredPreset(lab.Preset()); !why.empty()) {
                layers = previous;
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            Reply(Json().Str("event", "preset").Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson())).Done());
            continue;
        }

        // The formula is the rest of the line: it contains spaces and commas,
        // so it cannot be one of the key=value tokens.
        if (command.verb == "layer.formula") {
            auto& layers = lab.Preset().layers;
            const auto space = line.find(' ');
            const auto text = space == std::string::npos ? std::string() : line.substr(space + 1);
            const auto indexEnd = text.find(' ');
            int index = -1;
            std::string formula;
            if (indexEnd != std::string::npos) {
                try { index = std::stoi(text.substr(0, indexEnd)); } catch (...) { index = -1; }
                formula = text.substr(indexEnd + 1);
            }
            if (index < 0 || index >= static_cast<int>(layers.size())) {
                Reply(Json().Str("event", "rejected").Str("message", "레이어 번호가 범위를 벗어났습니다").Done());
                continue;
            }
            // Parsed here so a bad formula is refused where it was typed,
            // rather than turning into silence at render time.
            lab::Expression probe;
            const auto why = probe.Parse(formula, {"t", "t0", "f"});
            if (!why.empty()) {
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            const auto previous = layers[static_cast<std::size_t>(index)];
            layers[static_cast<std::size_t>(index)].formula = formula;
            layers[static_cast<std::size_t>(index)].waveform = lab::LayerWaveform::Formula;
            if (const auto bad = lab::ValidateLayeredPreset(lab.Preset()); !bad.empty()) {
                layers[static_cast<std::size_t>(index)] = previous;
                Reply(Json().Str("event", "rejected").Str("message", bad).Done());
                continue;
            }
            Reply(Json().Str("event", "preset").Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson())).Done());
            continue;
        }

        if (command.verb == "preset.dump") {
            Reply(Json().Str("event", "preset").Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson())).Done());
            continue;
        }

        // Everything the analysis panels need, from ONE render: the summed
        // waveform, each layer in place, the limiter prediction, and the
        // measurements of each. One render means the numbers on screen all
        // describe the same signal.
        if (command.verb == "preset.analyze") {
            float points = 700.0f;
            command.Number("points", points);
            const auto rendered = lab.RenderPreset();
            if (!rendered->ok) {
                Reply(Json().Str("event", "rejected").Str("message", rendered->error).Done());
                continue;
            }
            const auto& limiter = lab.Limiter();
            const auto previewLeft = lab::PreviewLimiter(rendered->left, rendered->sampleRate,
                                                         limiter.threshold, limiter.attackMs,
                                                         limiter.releaseMs);
            const auto previewRight = lab::PreviewLimiter(rendered->right, rendered->sampleRate,
                                                          limiter.threshold, limiter.attackMs,
                                                          limiter.releaseMs);
            const auto buckets = static_cast<std::size_t>(std::clamp(points, 32.0f, 4000.0f));
            const auto bucket = (std::max)(std::size_t{1},
                                           (rendered->left.size() + buckets - 1) / buckets);

            std::ostringstream layerTraces;
            layerTraces << '[';
            for (std::size_t i = 0; i < rendered->layerLeft.size(); ++i) {
                if (i) layerTraces << ',';
                layerTraces << "{\"name\":\"" << lab.Preset().layers[i].name << "\",\"left\":"
                            << EnvelopeJson(rendered->layerLeft[i], bucket) << ",\"right\":"
                            << EnvelopeJson(rendered->layerRight[i], bucket) << '}';
            }
            layerTraces << ']';

            std::ostringstream scaled;
            scaled << '[';
            for (std::size_t i = 0; i < rendered->diagnostics.envelopeScaled.size(); ++i) {
                if (i) scaled << ',';
                scaled << '"' << rendered->diagnostics.envelopeScaled[i] << '"';
            }
            scaled << ']';

            Reply(Json()
                      .Str("event", "analysis")
                      .Int("sampleRate", rendered->sampleRate)
                      .Int("frames", static_cast<long long>(rendered->left.size()))
                      .Int("samplesPerBucket", static_cast<long long>(bucket))
                      .Num("lengthMs", lab.Preset().LengthMs())
                      .Raw("sumLeft", EnvelopeJson(rendered->left, bucket))
                      .Raw("sumRight", EnvelopeJson(rendered->right, bucket))
                      .Raw("limitedLeft", EnvelopeJson(previewLeft.samples, bucket))
                      .Raw("limitedRight", EnvelopeJson(previewRight.samples, bucket))
                      .Raw("layers", layerTraces.str())
                      .Raw("statsLeft", StatsJson(rendered->statsLeft))
                      .Raw("statsRight", StatsJson(rendered->statsRight))
                      .Raw("statsLimitedLeft", StatsJson(lab::MeasureSignal(previewLeft.samples)))
                      .Raw("statsLimitedRight", StatsJson(lab::MeasureSignal(previewRight.samples)))
                      .Num("limiterThreshold", limiter.threshold)
                      .Num("saturationCeiling", lab.Preset().saturationCeiling)
                      .Num("worstReductionDbLeft", previewLeft.worstReductionDb)
                      .Num("worstReductionDbRight", previewRight.worstReductionDb)
                      .Int("limitedFramesLeft", static_cast<long long>(previewLeft.limitedFrames))
                      .Int("limitedFramesRight", static_cast<long long>(previewRight.limitedFrames))
                      .Int("clampedLeft", static_cast<long long>(previewLeft.clampedSamples))
                      .Int("clampedRight", static_cast<long long>(previewRight.clampedSamples))
                      .Int("guardedDivisions",
                           static_cast<long long>(rendered->diagnostics.expression.guardedDivisions))
                      .Int("nonFiniteExpression",
                           static_cast<long long>(rendered->diagnostics.expression.nonFiniteResults))
                      .Int("nonFiniteSamples",
                           static_cast<long long>(rendered->diagnostics.nonFiniteSamples))
                      .Raw("envelopeScaled", scaled.str())
                      .Done());
            continue;
        }

        if (command.verb == "preset.fft") {
            float size = 2048.0f, start = 0.0f, channel = 0.0f;
            command.Number("size", size);
            command.Number("start", start);
            command.Number("channel", channel);
            std::string windowName = "hann";
            if (const auto* w = command.Find("window")) windowName = *w;
            lab::FftWindow window = lab::FftWindow::Hann;
            if (!lab::ParseFftWindow(windowName, window)) {
                Reply(Json().Str("event", "rejected").Str("message", "창 함수는 rect/hann/hamming").Done());
                continue;
            }
            auto rendered = lab.LastRender();
            if (!rendered || !rendered->ok) rendered = lab.RenderPreset();
            if (!rendered->ok) {
                Reply(Json().Str("event", "rejected").Str("message", rendered->error).Done());
                continue;
            }
            const auto& samples = channel >= 1.0f ? rendered->right : rendered->left;
            const auto result = lab::AnalyzeSpectrum(samples, rendered->sampleRate,
                                                     static_cast<std::size_t>((std::max)(0.0f, start)),
                                                     static_cast<std::size_t>(size), window);
            if (!result.ok) {
                Reply(Json().Str("event", "rejected").Str("message", result.error).Done());
                continue;
            }
            std::ostringstream freq, mag;
            freq << '[';
            mag << '[';
            for (std::size_t i = 0; i < result.magnitude.size(); ++i) {
                if (i) { freq << ','; mag << ','; }
                freq << std::setprecision(2) << std::fixed << result.frequencyHz[i];
                mag << std::setprecision(6) << std::fixed << result.magnitude[i];
            }
            freq << ']';
            mag << ']';
            Reply(Json()
                      .Str("event", "fft")
                      .Int("sampleRate", result.sampleRate)
                      .Int("size", static_cast<long long>(result.size))
                      .Int("startFrame", static_cast<long long>(result.startFrame))
                      .Num("binHz", static_cast<double>(result.sampleRate) /
                                        static_cast<double>(result.size))
                      .Str("window", lab::ToString(result.window))
                      .Str("channel", channel >= 1.0f ? "right" : "left")
                      .Str("unit", "PCM 진폭 (물리 단위 아님)")
                      .Raw("frequencyHz", freq.str())
                      .Raw("magnitude", mag.str())
                      .Done());
            continue;
        }

        if (command.verb == "preset.play") {
            std::string why;
            if (!lab.PlayPreset(why)) {
                Reply(Json().Str("event", "rejected").Str("message", why).Done());
                continue;
            }
            Reply(Json().Str("event", "played").Num("lengthMs", lab.Preset().LengthMs()).Done());
            continue;
        }

        // The WAV comes from the SAME render that was played. Not a second
        // pass with the same settings -- the same buffer.
        if (command.verb == "preset.export") {
            const auto* path = command.Find("path");
            if (!path || path->empty()) {
                Reply(Json().Str("event", "rejected").Str("message", "export needs path=").Done());
                continue;
            }
            const auto rendered = lab.RenderPreset();
            if (!rendered->ok) {
                Reply(Json().Str("event", "rejected").Str("message", rendered->error).Done());
                continue;
            }
            const auto interleaved = lab::InterleaveStereo(rendered->left, rendered->right);
            const bool wroteWav =
                WriteStereoWavFloat32(*path + ".wav", interleaved, rendered->sampleRate);
            Reply(Json()
                      .Str("event", "exported")
                      .Str("wav", wroteWav ? *path + ".wav" : std::string())
                      .Int("sampleRate", rendered->sampleRate)
                      .Int("frames", static_cast<long long>(rendered->left.size()))
                      .Raw("preset", PresetJson(lab.Preset(), lab.SpeakerJson()))
                      .Bool("ok", wroteWav)
                      .Done());
            continue;
        }

        // ---- speaker cue -------------------------------------------------

        if (command.verb == "speaker.load") {
            // The whole rest of the line: a path has spaces in it far more
            // often than not.
            const auto space = line.find(' ');
            const auto path = space == std::string::npos ? std::string() : line.substr(space + 1);
            if (path.empty()) {
                Reply(Json().Str("event", "rejected").Str("message", "speaker.load 에 경로가 없습니다").Done());
                continue;
            }
            std::string why;
            const auto report = lab.LoadSpeakerClip(path, why);
            if (report.empty()) {
                Reply(Json().Str("event", "rejected")
                          .Str("message", "소리 파일을 열 수 없습니다: " + why).Done());
                continue;
            }
            Reply(Json().Str("event", "speaker").Str("report", report)
                      .Raw("speaker", lab.SpeakerJson())
                      .Raw("waveform", lab.SpeakerEnvelopeJson(600)).Done());
            continue;
        }

        if (command.verb == "speaker.clear") {
            lab.ClearSpeakerClip();
            Reply(Json().Str("event", "speaker").Str("report", "없음")
                      .Raw("speaker", lab.SpeakerJson())
                      .Raw("waveform", lab.SpeakerEnvelopeJson(600)).Done());
            continue;
        }

        if (command.verb == "speaker.set") {
            float gain = 1.0f, offset = 0.0f, on = 1.0f;
            command.Number("gain", gain);
            command.Number("offset", offset);
            command.Number("on", on);
            lab.SetSpeaker(gain, offset, on != 0.0f);
            Reply(Json().Str("event", "speaker").Str("report", "설정됨")
                      .Raw("speaker", lab.SpeakerJson())
                      .Raw("waveform", lab.SpeakerEnvelopeJson(600)).Done());
            continue;
        }

        // ---- device ------------------------------------------------------
        //
        // Listed with the STABLE Windows endpoint id. A saved setting that
        // said "index 6" would point at a different device as soon as
        // anything else is plugged in or removed.
        if (command.verb == "device.list") {
            const auto endpoints = DualSenseAudioDevice::Enumerate();
            std::ostringstream list;
            list << '[';
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                if (i) list << ',';
                list << "{\"index\":" << i << ",\"id\":\"" << Narrow(endpoints[i].id)
                     << "\",\"name\":\"" << Narrow(endpoints[i].name)
                     << "\",\"looksLikeDualSense\":"
                     << (endpoints[i].looksLikeDualSense ? "true" : "false") << '}';
            }
            list << ']';
            Reply(Json().Str("event", "devices").Raw("endpoints", list.str()).Done());
            continue;
        }

        // ---- adaptive triggers -------------------------------------------
        //
        // Only the modes that are actually implemented, with only the
        // parameters that mode uses. The runtime validates and REFUSES rather
        // than clamping, and the reply carries the effect id so a release can
        // name exactly the effect it meant.
        if (command.verb == "trigger.apply" || command.verb == "trigger.release" ||
            command.verb == "trigger.reset") {
            if (!lab.TriggersReady()) {
                Reply(Json().Str("event", "rejected").Str("message", "트리거 장치가 열려 있지 않습니다").Done());
                continue;
            }
            const auto nowUs = NowMs() * 1000;
            if (command.verb == "trigger.reset") {
                lab.Triggers().ResetToNeutral(nowUs);
                Reply(Json().Str("event", "trigger").Str("action", "reset").Done());
                continue;
            }
            const auto* sideText = command.Find("side");
            const bool left = sideText && (*sideText == "l" || *sideText == "left");
            const auto side = left ? dualsense::TriggerSide::Left : dualsense::TriggerSide::Right;
            if (command.verb == "trigger.release") {
                lab.Triggers().CancelSide(side, nowUs);
                Reply(Json().Str("event", "trigger").Str("action", "release")
                          .Str("side", left ? "left" : "right").Done());
                continue;
            }
            float start = 2.0f, end = 5.0f, strength = 4.0f, frequency = 40.0f, durationMs = 0.0f;
            command.Number("start", start);
            command.Number("end", end);
            command.Number("strength", strength);
            command.Number("freq", frequency);
            command.Number("ms", durationMs);
            const auto* modeText = command.Find("mode");
            const std::string mode = modeText ? *modeText : "off";
            dualsense::TriggerEffectSpec spec;
            if (mode == "off") spec = dualsense::TriggerEffectSpec::MakeOff();
            else if (mode == "feedback")
                spec = dualsense::TriggerEffectSpec::MakeFeedback(
                    static_cast<std::uint8_t>(start), static_cast<std::uint8_t>(strength));
            else if (mode == "weapon")
                spec = dualsense::TriggerEffectSpec::MakeWeapon(
                    static_cast<std::uint8_t>(start), static_cast<std::uint8_t>(end),
                    static_cast<std::uint8_t>(strength));
            else if (mode == "vibration")
                spec = dualsense::TriggerEffectSpec::MakeVibration(
                    static_cast<std::uint8_t>(start), static_cast<std::uint8_t>(strength),
                    static_cast<std::uint8_t>(frequency));
            else {
                Reply(Json().Str("event", "rejected")
                          .Str("message", "모드는 off/feedback/weapon/vibration").Done());
                continue;
            }
            const auto durationUs = durationMs > 0.0f
                                        ? static_cast<std::int64_t>(durationMs) * 1000
                                        : dualsense::kHoldUntilReplaced;
            const auto applied = lab.Triggers().Apply(side, spec, durationUs, nowUs);
            Reply(Json()
                      .Str("event", "trigger")
                      .Str("action", "apply")
                      .Str("side", left ? "left" : "right")
                      .Str("mode", mode)
                      .Bool("accepted", applied.accepted)
                      .Bool("reducedToOff", applied.reducedToOff)
                      .Int("effectId", static_cast<long long>(applied.effectId))
                      .Str("error", applied.error)
                      .Done());
            continue;
        }

        if (command.verb == "trigger.tick") {
            if (lab.TriggersReady()) lab.Triggers().Tick(NowMs() * 1000);
            Reply(Json().Str("event", "trigger").Str("action", "tick").Done());
            continue;
        }

        Reply(Json().Str("event", "rejected").Str("message", "unknown command \"" + command.verb + "\"").Done());
    }

    lab.Shutdown();
    Reply(Json().Str("event", "closed").Done());
    return 0;
}
