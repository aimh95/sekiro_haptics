#define NOMINMAX
#include "sekiro_haptics/DualSenseAudioDevice.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

namespace sekiro_haptics {
using Microsoft::WRL::ComPtr;

namespace {

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool Ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

std::string Hr(HRESULT hr) {
    if (hr == S_OK) return "S_OK";
    if (hr == S_FALSE) return "S_FALSE (a closest match exists)";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "AUDCLNT_E_UNSUPPORTED_FORMAT";
    std::ostringstream out;
    out << "0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

std::string TagName(WORD tag) {
    switch (tag) {
        case WAVE_FORMAT_PCM: return "PCM";
        case WAVE_FORMAT_IEEE_FLOAT: return "IEEE_FLOAT";
        case WAVE_FORMAT_EXTENSIBLE: return "EXTENSIBLE";
        default: return "other(" + std::to_string(tag) + ")";
    }
}

void FillReport(const WAVEFORMATEX& f, AudioFormatReport& report) {
    report.sampleRate = f.nSamplesPerSec;
    report.channels = f.nChannels;
    report.bitsPerSample = f.wBitsPerSample;
    report.validBitsPerSample = f.wBitsPerSample;
    report.formatTag = TagName(f.wFormatTag);
    report.subFormat = TagName(f.wFormatTag);
    if (f.wFormatTag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&f);
        report.channelMask = ext->dwChannelMask;
        report.validBitsPerSample = ext->Samples.wValidBitsPerSample;
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) report.subFormat = "PCM";
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) report.subFormat = "IEEE_FLOAT";
        else report.subFormat = "unrecognised GUID";
    }
}

std::int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool LooksLikeDualSense(const std::wstring& name) {
    std::wstring lower;
    lower.reserve(name.size());
    for (wchar_t c : name) lower.push_back(static_cast<wchar_t>(::towlower(c)));
    return lower.find(L"dualsense") != std::wstring::npos ||
           lower.find(L"wireless controller") != std::wstring::npos;
}

} // namespace

struct DualSenseAudioDevice::Impl {
    ComScope com;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* format = nullptr;
    UINT32 bufferFrames = 0;
    bool floatSamples = false;
    bool started = false;
    AudioFormatReport report;
    std::string error;
    /// Guards `voices` and `stats` only. Held for the duration of a mix, which
    /// is a few hundred microseconds; Queue() from the detection thread waits
    /// at most that long and never does I/O under it.
    mutable std::mutex mutex;
    std::deque<AudioVoice> voices;
    std::size_t voiceLimit = 16;       // soft cap: start retiring tails
    std::size_t hardVoiceLimit = 24;   // absolute cap: only here is anything dropped
    float retireFadeMs = 25.0f;
    std::uint64_t voiceSequence = 0;
    LimiterSettings limiter;
    /// Per-channel limiter gain, carried across Pump() calls so the release
    /// does not restart at every buffer boundary.
    std::vector<float> limiterGain;
    bool capturing = false;
    std::size_t captureLimit = 0;
    std::vector<float> capture;
    AudioRenderStats stats;
    /// Bounded ring of finished voices. Bounded because a long session would
    /// otherwise accumulate one record per cue forever, and an unbounded
    /// allocation on the render path is exactly what must not happen.
    std::deque<VoiceLifecycleRecord> history;
    std::size_t historyLimit = 256;
    int speakerChannel = -1;
    std::thread renderThread;
    std::atomic<bool> renderRunning{false};
    std::int64_t lastPumpUs = 0;
    /// Set when the audio engine drives the feed itself. Sleeping instead was
    /// measured at a 31 ms worst-case gap against a 22 ms buffer, because a
    /// 1 ms sleep_for is really a ~15.6 ms timer tick.
    HANDLE renderEvent = nullptr;
    bool eventDriven = false;
    /// Frames handed over by the last Pump(). Zero means the buffer was
    /// already full and the thread must NOT immediately try again.
    std::uint32_t lastSubmitted = 0;

    ~Impl() {
        if (client && started) client->Stop();
        if (renderEvent) CloseHandle(renderEvent);
        CoTaskMemFree(format);
    }
    bool Check(HRESULT hr, const char* what) {
        if (SUCCEEDED(hr)) return true;
        std::ostringstream out;
        out << what << " failed: " << Hr(hr);
        error = out.str();
        return false;
    }
};

DualSenseAudioDevice::DualSenseAudioDevice() : impl_(std::make_unique<Impl>()) {}
DualSenseAudioDevice::~DualSenseAudioDevice() = default;
bool DualSenseAudioDevice::IsOpen() const { return impl_->render != nullptr; }
const AudioFormatReport& DualSenseAudioDevice::Format() const { return impl_->report; }
const std::string& DualSenseAudioDevice::Error() const { return impl_->error; }
std::size_t DualSenseAudioDevice::ActiveVoices() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->voices.size();
}
AudioRenderStats DualSenseAudioDevice::RenderStats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}
void DualSenseAudioDevice::ResetRenderStats() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto buffer = impl_->stats;
    impl_->stats = AudioRenderStats{};
    impl_->stats.bufferFrames = buffer.bufferFrames;
    impl_->stats.bufferMs = buffer.bufferMs;
    impl_->stats.writeAheadMs = buffer.writeAheadMs;
    impl_->stats.eventDriven = buffer.eventDriven;
    impl_->lastPumpUs = 0;
}
void DualSenseAudioDevice::SetSpeakerChannel(int channel) { impl_->speakerChannel = channel; }
bool DualSenseAudioDevice::RenderThreadRunning() const { return impl_->renderRunning.load(); }
std::size_t DualSenseAudioDevice::VoiceLimit() const { return impl_->voiceLimit; }
std::size_t DualSenseAudioDevice::HardVoiceLimit() const { return impl_->hardVoiceLimit; }
void DualSenseAudioDevice::SetVoiceLimit(std::size_t limit) { SetVoiceLimit(limit, impl_->retireFadeMs); }
void DualSenseAudioDevice::SetVoiceLimit(std::size_t limit, float retireFadeMs) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->voiceLimit = std::max<std::size_t>(1, limit);
    // Headroom above the soft cap so retirement has time to fade rather than
    // having to cut. Only past this is a voice actually dropped.
    impl_->hardVoiceLimit = impl_->voiceLimit + 8;
    impl_->retireFadeMs = std::isfinite(retireFadeMs) ? std::clamp(retireFadeMs, 1.0f, 500.0f) : 25.0f;
}
void DualSenseAudioDevice::SetLimiter(const LimiterSettings& settings) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->limiter = settings;
}
const LimiterSettings& DualSenseAudioDevice::Limiter() const { return impl_->limiter; }

std::vector<AudioEndpointInfo> DualSenseAudioDevice::Enumerate() {
    ComScope com;
    std::vector<AudioEndpointInfo> result;
    if (!com.Ready()) return result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
        return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return result;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        AudioEndpointInfo info{id, L"(unnamed)", false};
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v{};
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                info.name = v.pwszVal;
            PropVariantClear(&v);
        }
        info.looksLikeDualSense = LooksLikeDualSense(info.name);
        result.push_back(std::move(info));
    }
    return result;
}

namespace {
/// Reads Windows' master level/mute for one endpoint. Reporting only.
void ReadEndpointVolume(IMMDevice* device, AudioFormatReport& report) {
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.GetAddressOf())))) return;
    float scalar = 0.0f;
    float db = 0.0f;
    BOOL muted = FALSE;
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&scalar)) &&
        SUCCEEDED(volume->GetMute(&muted))) {
        report.volumeKnown = true;
        report.endpointVolume = scalar;
        report.endpointMuted = muted != FALSE;
    }
    // The scalar is a perceptual taper, not an amplitude multiplier; the dB
    // value is what predicts how loud this actually is.
    if (SUCCEEDED(volume->GetMasterVolumeLevel(&db))) report.endpointVolumeDb = db;
    float minDb = 0, maxDb = 0, step = 0;
    if (SUCCEEDED(volume->GetVolumeRange(&minDb, &maxDb, &step))) {
        report.endpointVolumeMinDb = minDb;
        report.endpointVolumeMaxDb = maxDb;
    }
    UINT channels = 0;
    if (SUCCEEDED(volume->GetChannelCount(&channels))) {
        report.endpointChannelVolumes.clear();
        for (UINT c = 0; c < channels; ++c) {
            float v = 0.0f;
            if (SUCCEEDED(volume->GetChannelVolumeLevelScalar(c, &v)))
                report.endpointChannelVolumes.push_back(v);
        }
    }
}
} // namespace

VolumeSetResult DualSenseAudioDevice::SetEndpointVolume(const std::wstring& endpointId, float scalar) {
    VolumeSetResult result;
    ComScope com;
    if (!com.Ready() || endpointId.empty()) { result.hresult = "no COM or no endpoint id"; return result; }
    scalar = std::isfinite(scalar) ? std::clamp(scalar, 0.0f, 1.0f) : 0.0f;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(endpointId.c_str(), &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(volume.GetAddressOf()));
    if (FAILED(hr)) { result.hresult = Hr(hr); return result; }
    const HRESULT unmute = volume->SetMute(FALSE, nullptr);
    hr = volume->SetMasterVolumeLevelScalar(scalar, nullptr);
    result.hresult = "SetMute=" + Hr(unmute) + " SetMasterVolumeLevelScalar=" + Hr(hr);
    // Read back: "the call succeeded" and "the device is at that level" are
    // different claims.
    float back = 0.0f, db = 0.0f;
    BOOL muted = TRUE;
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&back)) &&
        SUCCEEDED(volume->GetMasterVolumeLevel(&db)) &&
        SUCCEEDED(volume->GetMute(&muted))) {
        result.readBackOk = true;
        result.readBackScalar = back;
        result.readBackDb = db;
        result.readBackMuted = muted != FALSE;
    }
    return result;
}

void DualSenseAudioDevice::RefreshSessionVolumes() {
    auto& p = *impl_;
    if (!p.client) return;
    ComPtr<ISimpleAudioVolume> session;
    if (SUCCEEDED(p.client->GetService(IID_PPV_ARGS(&session)))) {
        float v = 0.0f;
        BOOL m = FALSE;
        if (SUCCEEDED(session->GetMasterVolume(&v)) && SUCCEEDED(session->GetMute(&m))) {
            p.report.sessionVolumeKnown = true;
            p.report.sessionVolume = v;
            p.report.sessionMuted = m != FALSE;
        }
    }
    ComPtr<IChannelAudioVolume> channel;
    if (SUCCEEDED(p.client->GetService(IID_PPV_ARGS(&channel)))) {
        UINT32 count = 0;
        if (SUCCEEDED(channel->GetChannelCount(&count))) {
            p.report.streamChannelVolumes.clear();
            for (UINT32 c = 0; c < count; ++c) {
                float v = 0.0f;
                if (SUCCEEDED(channel->GetChannelVolume(c, &v)))
                    p.report.streamChannelVolumes.push_back(v);
            }
        }
    }
}

void DualSenseAudioDevice::BeginSubmitCapture(std::size_t maxFrames) {
    impl_->captureLimit = maxFrames;
    impl_->capture.clear();
    impl_->capture.reserve(maxFrames * std::max<unsigned>(1, impl_->report.channels));
    impl_->capturing = true;
}
const std::vector<float>& DualSenseAudioDevice::SubmitCapture() const { return impl_->capture; }
unsigned DualSenseAudioDevice::SubmitCaptureChannels() const { return impl_->report.channels; }
void DualSenseAudioDevice::EndSubmitCapture() { impl_->capturing = false; }

AudioFormatReport DualSenseAudioDevice::Describe(const std::wstring& endpointId) {
    AudioFormatReport report;
    ComScope com;
    if (!com.Ready() || endpointId.empty()) {
        report.error = "COM init failed or no endpoint id given";
        return report;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDevice(endpointId.c_str(), &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) {
        report.error = "could not activate the endpoint";
        return report;
    }
    WAVEFORMATEX* mix = nullptr;
    const HRESULT mixHr = client->GetMixFormat(&mix);
    if (FAILED(mixHr) || !mix) {
        report.error = "GetMixFormat failed: " + Hr(mixHr);
        return report;
    }
    FillReport(*mix, report);
    report.queried = true;
    ReadEndpointVolume(device.Get(), report);

    // Shared mode is what we actually use; ask anyway rather than assume.
    WAVEFORMATEX* closest = nullptr;
    report.sharedSupport = Hr(client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, mix, &closest));
    if (closest) CoTaskMemFree(closest);
    closest = nullptr;
    report.exclusiveSupport = Hr(client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mix, &closest));
    if (closest) CoTaskMemFree(closest);
    CoTaskMemFree(mix);
    return report;
}

bool DualSenseAudioDevice::Open(const std::wstring& endpointId, float bufferMs) {
    auto& p = *impl_;
    if (p.render) { p.error = "already open"; return false; }
    if (!p.com.Ready() || endpointId.empty()) {
        p.error = "COM init failed or no endpoint id given (no default-device fallback)";
        return false;
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (!p.Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "CoCreateInstance") ||
        !p.Check(enumerator->GetDevice(endpointId.c_str(), &device), "GetDevice") ||
        !p.Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(p.client.GetAddressOf())), "Activate") ||
        !p.Check(p.client->GetMixFormat(&p.format), "GetMixFormat")) {
        return false;
    }
    FillReport(*p.format, p.report);
    p.report.queried = true;
    ReadEndpointVolume(device.Get(), p.report);

    WAVEFORMATEX* closest = nullptr;
    const HRESULT shared = p.client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, p.format, &closest);
    p.report.sharedSupport = Hr(shared);
    if (closest) CoTaskMemFree(closest);
    if (shared != S_OK) {
        // Do NOT quietly convert to something else and call it success.
        p.error = "IsFormatSupported(SHARED, mixFormat) returned " + p.report.sharedSupport +
                  "; refusing to substitute a different channel layout";
        return false;
    }
    if (p.report.subFormat == "IEEE_FLOAT" && p.report.bitsPerSample == 32) p.floatSamples = true;
    else if (p.report.subFormat == "PCM" && p.report.bitsPerSample == 16) p.floatSamples = false;
    else {
        p.error = "endpoint offers " + p.report.subFormat + "/" + std::to_string(p.report.bitsPerSample) +
                  " bit, which this output does not write";
        return false;
    }
    // The buffer is BOTH the dropout margin and the worst-case added latency:
    // a cue queued right after a pump waits behind everything already written.
    // So it is a parameter and it is reported, not a constant hidden in here.
    bufferMs = std::isfinite(bufferMs) ? std::clamp(bufferMs, 5.0f, 200.0f) : 20.0f;
    const REFERENCE_TIME requested = static_cast<REFERENCE_TIME>(bufferMs * 10000.0f);

    // Event-driven first. A sleeping feeder is at the mercy of the system
    // timer: sleep_for(1ms) really returns on the next ~15.6 ms tick, which
    // measured a 31 ms worst-case gap against a 22 ms buffer -- the buffer ran
    // dry between two feeds and that is what broke the sound up. In event mode
    // the audio engine wakes us exactly when it wants more.
    p.renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT init = p.renderEvent
                       ? p.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              requested, 0, p.format, nullptr)
                       : E_FAIL;
    if (SUCCEEDED(init)) {
        const HRESULT bind = p.client->SetEventHandle(p.renderEvent);
        if (SUCCEEDED(bind)) p.eventDriven = true;
        else init = bind;
    }
    if (!p.eventDriven) {
        // Fall back, but say so rather than pretending the fast path was taken.
        p.report.error = "event-driven render unavailable (" + Hr(init) +
                         "); falling back to a timed feeder";
        p.client.Reset();
        if (!p.Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(p.client.GetAddressOf())), "Activate(retry)") ||
            !p.Check(p.client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, requested, 0, p.format, nullptr),
                     "Initialize")) {
            return false;
        }
    }
    if (!p.Check(p.client->GetBufferSize(&p.bufferFrames), "GetBufferSize") ||
        !p.Check(p.client->GetService(IID_PPV_ARGS(&p.render)), "GetService(IAudioRenderClient)")) {
        return false;
    }
    // What Windows GAVE us, which is not always what was asked for.
    p.stats.bufferFrames = p.bufferFrames;
    p.stats.bufferMs = p.report.sampleRate
                           ? 1000.0f * static_cast<float>(p.bufferFrames) / static_cast<float>(p.report.sampleRate)
                           : 0.0f;
    p.stats.writeAheadMs = p.stats.bufferMs;
    p.stats.eventDriven = p.eventDriven;
    // Our own session's volume, not the device's: make sure this app is not
    // attenuating itself. Other applications are unaffected.
    ComPtr<ISimpleAudioVolume> sessionVolume;
    if (SUCCEEDED(p.client->GetService(IID_PPV_ARGS(&sessionVolume)))) {
        sessionVolume->SetMute(FALSE, nullptr);
        sessionVolume->SetMasterVolume(1.0f, nullptr);
    }
    if (!Pump()) return false;
    if (!p.Check(p.client->Start(), "Start")) return false;
    p.started = true;
    RefreshSessionVolumes();
    return true;
}

void DualSenseAudioDevice::Close() {
    StopRenderThread();
    auto& p = *impl_;
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        p.voices.clear();
    }
    if (p.client && p.started) { p.client->Stop(); p.started = false; }
    p.render.Reset();
    p.client.Reset();
    CoTaskMemFree(p.format);
    p.format = nullptr;
}

namespace {
/// Files one finished voice into the bounded history. Caller holds the lock.
void RecordVoiceEnd(std::deque<VoiceLifecycleRecord>& history, std::size_t limit,
                    const AudioVoice& voice, VoiceEndReason reason, std::int64_t nowUs) {
    if (limit == 0) return;
    VoiceLifecycleRecord record;
    record.correlationId = voice.correlationId;
    record.sequence = voice.sequence;
    record.channel = voice.channel;
    record.queuedUs = voice.queuedUs;
    record.firstRenderedUs = voice.firstRenderedUs;
    record.endedUs = nowUs;
    record.framesPlayed = voice.position;
    record.clipFrames = voice.clip ? voice.clip->size() : 0;
    record.reason = reason;
    history.push_back(record);
    while (history.size() > limit) history.pop_front();
}
} // namespace

const char* ToString(VoiceEndReason reason) {
    switch (reason) {
        case VoiceEndReason::Completed: return "completed";
        case VoiceEndReason::RetiredAtCap: return "retired-at-cap";
        case VoiceEndReason::HardDropped: return "hard-dropped";
        case VoiceEndReason::Dropped: return "dropped";
    }
    return "unknown";
}

std::vector<VoiceLifecycleRecord> DualSenseAudioDevice::RecentVoiceHistory() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->history.begin(), impl_->history.end()};
}

void DualSenseAudioDevice::SetLifecycleHistoryLimit(std::size_t records) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->historyLimit = records;
    while (impl_->history.size() > records) impl_->history.pop_front();
}

void DualSenseAudioDevice::DropPending() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto now = NowUs();
    for (const auto& voice : impl_->voices)
        RecordVoiceEnd(impl_->history, impl_->historyLimit, voice, VoiceEndReason::Dropped, now);
    impl_->voices.clear();
}

void DualSenseAudioDevice::DuckActive(float factor, float fadeMs) {
    auto& p = *impl_;
    factor = std::isfinite(factor) ? std::clamp(factor, 0.0f, 1.0f) : 1.0f;
    fadeMs = std::isfinite(fadeMs) ? std::clamp(fadeMs, 0.1f, 200.0f) : 5.0f;
    const float rate = static_cast<float>(std::max<std::uint32_t>(1, p.report.sampleRate));
    const float fadeSamples = std::max(1.0f, fadeMs * rate / 1000.0f);
    std::lock_guard<std::mutex> lock(p.mutex);
    for (auto& v : p.voices) {
        // Aim at a lower target and WALK there; never jump. The new impact is
        // queued separately and starts on the very next frame regardless, so
        // the fade never delays it.
        v.duckTarget = std::max(0.0f, v.duckTarget * factor);
        v.duckStep = std::max(1e-6f, (v.duck - v.duckTarget) / fadeSamples);
    }
}

bool DualSenseAudioDevice::StartRenderThread() {
    auto& p = *impl_;
    if (!p.render) { p.error = "cannot start the render thread before Open()"; return false; }
    if (p.renderRunning.exchange(true)) return true;
    p.renderThread = std::thread([this] {
        // The device's interfaces live in the MTA; this thread joins it too.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Audio has to win against the detection loop, which does
        // ReadProcessMemory graph walks and writes to the console.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        if (!impl_->eventDriven) {
            // Only the fallback path sleeps, and only then is the system timer
            // worth raising process-wide.
            timeBeginPeriod(1);
        }
        while (impl_->renderRunning.load()) {
            if (impl_->eventDriven) {
                // The engine signals when it has drained a period. The timeout
                // is a safety net, not the schedule.
                WaitForSingleObject(impl_->renderEvent, 200);
                if (!impl_->renderRunning.load()) break;
                Pump();
                // The wait was measured returning immediately over and over
                // while the buffer was full: 127 million pumps in 20 seconds,
                // spinning a TIME_CRITICAL thread next to the game. Whatever
                // the engine is doing with the event, a pump that had nothing
                // to hand over must not be retried instantly.
                // ::Sleep honours timeBeginPeriod; sleep_for under MinGW
                // does not, and a 1 ms request there becomes a 15.6 ms tick.
                if (impl_->lastSubmitted == 0) ::Sleep(1);
            } else {
                Pump();
                ::Sleep(1);
            }
        }
        if (!impl_->eventDriven) timeEndPeriod(1);
        if (SUCCEEDED(com)) CoUninitialize();
    });
    return true;
}

void DualSenseAudioDevice::StopRenderThread() {
    auto& p = *impl_;
    if (!p.renderRunning.exchange(false)) return;
    if (p.renderEvent) SetEvent(p.renderEvent);   // do not wait out the timeout
    if (p.renderThread.joinable()) p.renderThread.join();
}

namespace {
/// Picks the tail that has the least left to say -- furthest through its clip,
/// already ducked lowest -- and fades it out. Deliberately not "the oldest":
/// a long speaker clip started before a short haptic one still has audible
/// tail, and cutting it is what the overlap policy is meant to prevent.
void RetireQuietestImpl(std::deque<AudioVoice>& voices, float rate, float fadeMs,
                        std::uint64_t& retiredCount) {
    AudioVoice* worst = nullptr;
    float worstScore = 0.0f;
    for (auto& v : voices) {
        if (v.retiring || !v.clip || v.clip->empty()) continue;
        const float remaining = 1.0f - static_cast<float>(v.position) /
                                           static_cast<float>(v.clip->size());
        const float score = remaining * v.duck * v.gain;
        if (!worst || score < worstScore) { worst = &v; worstScore = score; }
    }
    if (!worst) return;
    const float fadeSamples = std::max(1.0f, fadeMs * rate / 1000.0f);
    worst->retiring = true;
    worst->duckTarget = 0.0f;
    worst->duckStep = std::max(1e-6f, worst->duck / fadeSamples);
    ++retiredCount;
}
} // namespace

bool DualSenseAudioDevice::Queue(const std::vector<float>& clip, int channel, float gain) {
    return Queue(clip, channel, gain, 0);
}

bool DualSenseAudioDevice::Queue(const std::vector<float>& clip, int channel, float gain,
                                 std::uint64_t correlationId) {
    auto& p = *impl_;
    if (!p.render || clip.empty()) return false;
    if (channel < 0 || channel >= static_cast<int>(p.report.channels)) {
        p.error = "channel " + std::to_string(channel) + " is outside this endpoint's " +
                  std::to_string(p.report.channels) + " channels";
        return false;
    }
    std::lock_guard<std::mutex> lock(p.mutex);

    // A new event NEVER stops or ducks what is already sounding. The only
    // reason anything is touched here is the cap, and even then the chosen
    // tail is faded, not cut.
    if (p.voices.size() >= p.voiceLimit)
        RetireQuietestImpl(p.voices, static_cast<float>(std::max<std::uint32_t>(1, p.report.sampleRate)),
                           p.retireFadeMs, p.stats.voicesRetired);
    // Absolute cap, so a runaway can still not grow without bound. Reaching
    // this is a bug or an extreme burst, not normal repeated parrying, and it
    // is counted separately from retirement so the two are never confused.
    while (p.voices.size() >= p.hardVoiceLimit) {
        auto oldest = std::min_element(p.voices.begin(), p.voices.end(),
                                       [](const AudioVoice& a, const AudioVoice& b) {
                                           return a.sequence < b.sequence;
                                       });
        RecordVoiceEnd(p.history, p.historyLimit, *oldest, VoiceEndReason::HardDropped, NowUs());
        p.voices.erase(oldest);
        ++p.stats.voicesEvicted;
    }

    AudioVoice voice;
    voice.clip = &clip;
    voice.channel = channel;
    voice.gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 4.0f) : 0.0f;
    voice.sequence = ++p.voiceSequence;
    voice.correlationId = correlationId;
    voice.queuedUs = NowUs();
    p.voices.push_back(voice);
    ++p.stats.voicesStarted;
    p.stats.maxConcurrentVoices = std::max(p.stats.maxConcurrentVoices, p.voices.size());
    return true;
}

bool DualSenseAudioDevice::QueuePair(const std::vector<float>& clip, int left, int right,
                                     float gain, float balance) {
    return QueuePair(clip, left, right, gain, balance, 0);
}

bool DualSenseAudioDevice::QueuePair(const std::vector<float>& clip, int left, int right,
                                     float gain, float balance, std::uint64_t correlationId) {
    balance = std::isfinite(balance) ? std::clamp(balance, -1.0f, 1.0f) : 0.0f;
    const float l = gain * (balance > 0 ? 1.0f - balance : 1.0f);
    const float r = gain * (balance < 0 ? 1.0f + balance : 1.0f);
    bool ok = false;
    if (left >= 0) ok = Queue(clip, left, l, correlationId) || ok;
    if (right >= 0) ok = Queue(clip, right, r, correlationId) || ok;
    return ok;
}

bool DualSenseAudioDevice::Pump() {
    auto& p = *impl_;
    if (!p.render) return false;
    UINT32 padding = 0;
    if (!p.Check(p.client->GetCurrentPadding(&padding), "GetCurrentPadding")) return false;
    if (padding > p.bufferFrames) return false;
    const UINT32 available = p.bufferFrames - padding;
    p.lastSubmitted = 0;

    std::lock_guard<std::mutex> lock(p.mutex);
    const bool active = !p.voices.empty();

    // Feed timing, recorded before the early-out so a pump with nothing to do
    // still counts as "the feeder was here".
    const auto now = NowUs();
    if (p.lastPumpUs != 0) {
        p.stats.lastFeedGapUs = now - p.lastPumpUs;
        p.stats.worstFeedGapUs = std::max(p.stats.worstFeedGapUs, p.stats.lastFeedGapUs);
    }
    p.lastPumpUs = now;
    ++p.stats.pumps;
    p.stats.maxPadding = std::max(p.stats.maxPadding, padding);
    // Padding is zero for the whole of a silent wait. That is normal and is
    // NOT a dropout, so the two cases are counted apart and only the first
    // means the output actually ran dry mid-sound.
    if (padding == 0) { if (active) ++p.stats.starvedWhileActive; else ++p.stats.idleZeroPadding; }

    if (available == 0) return true;
    BYTE* buffer = nullptr;
    if (!p.Check(p.render->GetBuffer(available, &buffer), "GetBuffer")) return false;
    const unsigned channels = p.report.channels;
    std::memset(buffer, 0, static_cast<std::size_t>(available) * p.format->nBlockAlign);

    if (p.limiterGain.size() != channels) p.limiterGain.assign(channels, 1.0f);
    const float sampleRate = static_cast<float>(std::max<std::uint32_t>(1, p.report.sampleRate));
    // Time constants as per-sample coefficients. Attack is fast so a sudden
    // overlap is caught; release is slow so the level does not audibly pump
    // back up between hits.
    const float attack = 1.0f - std::exp(-1.0f / std::max(1.0f, p.limiter.attackMs * sampleRate / 1000.0f));
    const float release = 1.0f - std::exp(-1.0f / std::max(1.0f, p.limiter.releaseMs * sampleRate / 1000.0f));
    const float threshold = std::clamp(p.limiter.threshold, 0.05f, 1.0f);

    std::vector<float> frame(channels, 0.0f);
    for (UINT32 f = 0; f < available; ++f) {
        std::fill(frame.begin(), frame.end(), 0.0f);
        for (std::size_t v = 0; v < p.voices.size(); ++v) {
            auto& voice = p.voices[v];
            const auto& clip = *voice.clip;
            // Each voice reads from its OWN position. Nothing here rewinds or
            // truncates another voice, so a new impact simply sums on top of
            // whatever is still ringing.
            if (voice.position >= clip.size()) continue;
            // Stamped on the first sample that actually reaches a submitted
            // buffer. This is a recordable stage; it is not the instant the
            // actuator moved, which this process cannot observe.
            if (voice.firstRenderedUs == 0) voice.firstRenderedUs = now;
            if (voice.duck > voice.duckTarget)
                voice.duck = std::max(voice.duckTarget, voice.duck - voice.duckStep);
            frame[static_cast<std::size_t>(voice.channel)] +=
                clip[voice.position++] * voice.gain * voice.duck;
        }
        for (unsigned c = 0; c < channels; ++c) {
            const bool isSpeaker = static_cast<int>(c) == p.speakerChannel;
            // Headroom first: this is the "somewhere to go" that keeps the
            // limiter idle during ordinary overlap.
            float x = frame[c] * (isSpeaker ? p.limiter.speakerHeadroom : p.limiter.hapticHeadroom);
            const float magnitude = std::fabs(x);
            if (isSpeaker) p.stats.peakSpeakerBeforeLimit = std::max(p.stats.peakSpeakerBeforeLimit, magnitude);
            else p.stats.peakHapticBeforeLimit = std::max(p.stats.peakHapticBeforeLimit, magnitude);

            // Gain reduction, per channel and NOT divided by the voice count:
            // it only moves when the SUM actually exceeds the threshold, so a
            // second overlapping hit does not make the first one quieter.
            float& gain = p.limiterGain[c];
            const float wanted = magnitude > threshold ? threshold / magnitude : 1.0f;
            gain += (wanted - gain) * (wanted < gain ? attack : release);
            gain = std::clamp(gain, 0.0f, 1.0f);
            if (gain < 0.999f) {
                const float reductionDb = -20.0f * std::log10(std::max(gain, 1e-6f));
                if (isSpeaker) {
                    ++p.stats.limitedSpeakerFrames;
                    p.stats.worstSpeakerReductionDb = std::max(p.stats.worstSpeakerReductionDb, reductionDb);
                } else {
                    ++p.stats.limitedHapticFrames;
                    p.stats.worstHapticReductionDb = std::max(p.stats.worstHapticReductionDb, reductionDb);
                }
            }
            x *= gain;

            // Last guard, zero latency: the smoothed gain cannot react within
            // the very first sample of an over, so a soft knee absorbs it
            // instead of a hard corner. Only engages above the threshold.
            if (std::fabs(x) > threshold) {
                const float sign = x < 0.0f ? -1.0f : 1.0f;
                const float over = (std::fabs(x) - threshold) / std::max(1e-6f, 1.0f - threshold);
                x = sign * (threshold + (1.0f - threshold) * std::tanh(over));
            }
            if (x > 1.0f || x < -1.0f) {
                if (isSpeaker) ++p.stats.clippedSpeakerSamples;
                else ++p.stats.clippedHapticSamples;
            }
            frame[c] = std::clamp(x, -1.0f, 1.0f);
        }
        // Capture what is ACTUALLY submitted, after headroom, limiter and soft
        // knee -- so a dump answers "was the clip cut off" separately from
        // "did the buffer run dry".
        if (p.capturing && p.capture.size() < p.captureLimit * channels)
            for (unsigned c = 0; c < channels; ++c) p.capture.push_back(frame[c]);
        for (unsigned c = 0; c < channels; ++c) {
            const float sample = frame[c];
            const auto index = static_cast<std::size_t>(f) * channels + c;
            if (p.floatSamples) std::memcpy(buffer + index * 4, &sample, 4);
            else {
                const auto value = static_cast<std::int16_t>(std::lround(sample * 32767.0f));
                std::memcpy(buffer + index * 2, &value, 2);
            }
        }
    }
    p.stats.framesSubmitted += available;
    p.lastSubmitted = available;
    for (std::size_t v = p.voices.size(); v-- > 0;) {
        const auto& voice = p.voices[v];
        // A clip that reached its end is done -- this is how a voice normally
        // dies, and it happens at the clip's own length, never at another
        // voice's. A retired voice leaves once its fade has actually reached
        // zero, not when retirement was requested.
        const bool ended = voice.position >= voice.clip->size();
        const bool fadedOut = voice.duckTarget <= 0.0f && voice.duck <= 0.0f;
        if (!ended && !fadedOut) continue;
        if (ended) ++p.stats.voicesFinished;
        RecordVoiceEnd(p.history, p.historyLimit, voice,
                       ended ? VoiceEndReason::Completed : VoiceEndReason::RetiredAtCap, now);
        p.voices.erase(p.voices.begin() + static_cast<std::ptrdiff_t>(v));
    }
    return p.Check(p.render->ReleaseBuffer(available, 0), "ReleaseBuffer");
}

} // namespace sekiro_haptics
