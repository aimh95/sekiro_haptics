#define NOMINMAX
#include "sekiro_haptics/AudioClipLoader.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace sekiro_haptics {
using Microsoft::WRL::ComPtr;

namespace {

struct MfScope {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ~MfScope() {
        if (SUCCEEDED(mf)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
    bool Ready() const {
        return (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) && SUCCEEDED(mf);
    }
};

std::string Hr(const char* what, HRESULT hr) {
    std::ostringstream out;
    out << what << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
    return out.str();
}

} // namespace

namespace {

/// The shared decode. `channels` is what Media Foundation is ASKED for, and
/// anything else coming back is refused rather than accepted -- the caller
/// then learns it did not get what it wanted instead of silently being handed
/// a different layout.
std::vector<float> LoadInterleaved(const std::filesystem::path& path,
                                   std::uint32_t targetSampleRate, std::uint32_t channels,
                                   AudioClipInfo& info) {
    info = AudioClipInfo{};
    if (targetSampleRate < 8'000 || targetSampleRate > 192'000) {
        info.error = "unsupported target sample rate";
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        info.error = "file not found: " + path.string();
        return {};
    }

    MfScope mf;
    if (!mf.Ready()) { info.error = "Media Foundation startup failed"; return {}; }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader);
    if (FAILED(hr)) { info.error = Hr("MFCreateSourceReaderFromURL", hr); return {}; }

    // What the file actually is, for reporting.
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native))) {
        UINT32 v = 0;
        if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v))) info.sourceSampleRate = v;
        if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v))) info.sourceChannels = v;
    }

    // Ask for exactly what we want. If MF cannot provide it we report that
    // instead of accepting some other rate or channel count.
    ComPtr<IMFMediaType> wanted;
    hr = MFCreateMediaType(&wanted);
    if (SUCCEEDED(hr)) hr = wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (SUCCEEDED(hr)) hr = wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, targetSampleRate);
    if (SUCCEEDED(hr)) hr = wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    if (SUCCEEDED(hr)) hr = wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4 * channels);
    if (SUCCEEDED(hr))
        hr = wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, targetSampleRate * 4 * channels);
    if (SUCCEEDED(hr)) hr = wanted->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr))
        hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                         nullptr, wanted.Get());
    if (FAILED(hr)) { info.error = Hr("SetCurrentMediaType(float)", hr); return {}; }

    // Confirm what we were actually given.
    ComPtr<IMFMediaType> actual;
    if (SUCCEEDED(reader->GetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actual))) {
        UINT32 rate = 0, decodedChannels = 0;
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &decodedChannels);
        info.decodedSampleRate = rate;
        if (rate != targetSampleRate || decodedChannels != channels) {
            info.error = "decoder produced " + std::to_string(rate) + " Hz / " +
                         std::to_string(decodedChannels) + " ch instead of the requested " +
                         std::to_string(channels) + " ch / " +
                         std::to_string(targetSampleRate) + " Hz";
            return {};
        }
    }

    std::vector<float> pcm;
    while (true) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                                nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { info.error = Hr("ReadSample", hr); return {}; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
        const std::size_t count = length / sizeof(float);
        const auto* samples = reinterpret_cast<const float*>(data);
        pcm.insert(pcm.end(), samples, samples + count);
        buffer->Unlock();

        if (pcm.size() > targetSampleRate * 30u * channels) {   // 30 s is far beyond a hit sound
            info.error = "clip is longer than 30 seconds; refusing";
            return {};
        }
    }

    // Do NOT clamp here. These sources genuinely decode above +-1.0 (float MP3
    // output is not bounded), and clamping at load would (a) clip the loudest
    // part of every hit and (b) hide the fact that blind amplification is
    // unsafe. The caller normalises with real headroom instead; clamping
    // happens once, at the point the samples are written to the device.
    for (float& s : pcm) {
        if (!std::isfinite(s)) s = 0.0f;
        info.peak = std::max(info.peak, std::fabs(s));
    }
    info.clippedInSource = info.peak > 1.0f;
    info.frames = pcm.size() / channels;
    info.ok = !pcm.empty();
    if (pcm.empty()) info.error = "decoded to zero samples";
    return pcm;
}

} // namespace

std::vector<float> LoadAudioClipMono(const std::filesystem::path& path,
                                     std::uint32_t targetSampleRate, AudioClipInfo& info) {
    return LoadInterleaved(path, targetSampleRate, 1, info);
}

void LoadAudioClipStereo(const std::filesystem::path& path, std::uint32_t targetSampleRate,
                         std::vector<float>& outLeft, std::vector<float>& outRight,
                         AudioClipInfo& info) {
    outLeft.clear();
    outRight.clear();
    const std::vector<float> interleaved = LoadInterleaved(path, targetSampleRate, 2, info);
    if (!info.ok) return;
    const std::size_t frames = interleaved.size() / 2;
    outLeft.reserve(frames);
    outRight.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        outLeft.push_back(interleaved[i * 2]);
        outRight.push_back(interleaved[i * 2 + 1]);
    }
}

void NormalizePeak(std::vector<float>& clip, float targetPeak) {
    if (clip.empty() || !std::isfinite(targetPeak) || targetPeak <= 0.0f) return;
    float peak = 0.0f;
    for (float s : clip) peak = std::max(peak, std::fabs(s));
    if (peak <= 1e-6f) return;
    const float scale = std::clamp(targetPeak, 0.0f, 1.0f) / peak;
    for (float& s : clip) s *= scale;   // already below the target by construction
}

void ApplyFadeOut(std::vector<float>& clip, std::uint32_t sampleRate, float milliseconds) {
    if (clip.empty() || milliseconds <= 0.0f) return;
    const auto fade = std::min(clip.size(),
                               static_cast<std::size_t>(milliseconds / 1000.0f *
                                                        static_cast<float>(sampleRate)));
    if (fade == 0) return;
    const std::size_t start = clip.size() - fade;
    for (std::size_t i = 0; i < fade; ++i) {
        const float g = 1.0f - static_cast<float>(i) / static_cast<float>(fade);
        clip[start + i] *= g;
    }
}

std::size_t TrimLeadingSilence(std::vector<float>& clip, float threshold) {
    if (clip.empty()) return 0;
    threshold = std::max(0.0f, threshold);
    std::size_t first = 0;
    while (first < clip.size() && std::fabs(clip[first]) <= threshold) ++first;
    if (first == 0 || first == clip.size()) return 0;
    clip.erase(clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(first));
    return first;
}

} // namespace sekiro_haptics
