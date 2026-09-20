#define NOMINMAX
#include "sekiro_haptics/WasapiSpeakerOutput.hpp"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>

namespace sekiro_haptics {
using Microsoft::WRL::ComPtr;
namespace {
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool Ready() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
}
struct WasapiSpeakerOutput::Impl {
    ComScope com; // destroyed after all interfaces
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* format = nullptr;
    UINT32 bufferFrames = 0;
    bool floatSamples = false;
    bool fault = false;
    std::string error;
    std::array<std::vector<float>,7> clips;
    struct Voice { std::size_t cue, position; };
    std::vector<Voice> voices;
    ~Impl() { if (client) client->Stop(); CoTaskMemFree(format); }
    bool Check(HRESULT hr, const char* operation) {
        if (SUCCEEDED(hr)) return true;
        std::ostringstream out; out << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
        error = out.str(); fault = true; return false;
    }
};
WasapiSpeakerOutput::WasapiSpeakerOutput() : impl_(std::make_unique<Impl>()) {}
WasapiSpeakerOutput::~WasapiSpeakerOutput() = default;
const std::string& WasapiSpeakerOutput::Error() const { return impl_->error; }
std::vector<AudioEndpoint> WasapiSpeakerOutput::Enumerate() {
    ComScope com;
    std::vector<AudioEndpoint> result;
    if (!com.Ready()) return result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))) return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection))) return result;
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return result;
    for (UINT i=0; i<count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i,&device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        AudioEndpoint endpoint{id,L"(unnamed endpoint)"}; CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
            PROPVARIANT value{};
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt == VT_LPWSTR && value.pwszVal)
                endpoint.name = value.pwszVal;
            PropVariantClear(&value);
        }
        result.push_back(std::move(endpoint));
    }
    return result;
}
bool WasapiSpeakerOutput::Open(const std::wstring& id, float gain) {
    auto& p = *impl_;
    if (p.client) { p.error = "audio endpoint already open"; return false; }
    if (!p.com.Ready() || id.empty()) { p.error = "COM initialization or explicit endpoint ID required"; return false; }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (!p.Check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"enumerator") ||
        !p.Check(enumerator->GetDevice(id.c_str(),&device),"selected endpoint") ||
        !p.Check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(p.client.GetAddressOf())),"activate") ||
        !p.Check(p.client->GetMixFormat(&p.format),"mix format")) return false;
    const auto& f = *p.format;
    bool pcm = f.wFormatTag == WAVE_FORMAT_PCM;
    bool floating = f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (f.wFormatTag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&f);
        pcm = IsEqualGUID(ext->SubFormat,KSDATAFORMAT_SUBTYPE_PCM);
        floating = IsEqualGUID(ext->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (ext->Samples.wValidBitsPerSample != f.wBitsPerSample) pcm = floating = false;
    }
    if (!((floating && f.wBitsPerSample == 32) || (pcm && f.wBitsPerSample == 16)) ||
        (f.nChannels != 1 && f.nChannels != 2 && f.nChannels != 4) ||
        f.nBlockAlign != f.nChannels*(f.wBitsPerSample/8) || f.nSamplesPerSec < 8'000 || f.nSamplesPerSec > 192'000) {
        p.error = "requires mono/stereo/quad PCM16 or float32 USB render endpoint"; p.fault = true; return false;
    }
    p.floatSamples = floating;
    if (!p.Check(p.client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,200'000,0,p.format,nullptr),"initialize") ||
        !p.Check(p.client->GetBufferSize(&p.bufferFrames),"buffer size") ||
        !p.Check(p.client->GetService(IID_PPV_ARGS(&p.render)),"render service")) return false;
    gain = std::isfinite(gain) ? std::clamp(gain,0.0f,1.0f) : 0.0f;
    for (std::size_t i=0;i<p.clips.size();++i) p.clips[i] = SynthesizeCue(static_cast<SpeakerCue>(i),f.nSamplesPerSec,gain);
    return Pump() && p.Check(p.client->Start(),"start");
}
bool WasapiSpeakerOutput::Play(SpeakerCue cue) {
    auto& p = *impl_;
    if (!p.render || p.fault || static_cast<std::size_t>(cue) >= p.clips.size()) return false;
    if (p.voices.size() == 8) p.voices.erase(p.voices.begin());
    p.voices.push_back({static_cast<std::size_t>(cue),0}); return true;
}
bool WasapiSpeakerOutput::Pump() {
    auto& p = *impl_;
    if (!p.render || p.fault) return false;
    UINT32 padding = 0;
    if (!p.Check(p.client->GetCurrentPadding(&padding),"padding") || padding > p.bufferFrames) return false;
    const auto available = p.bufferFrames-padding;
    if (available == 0) return true;
    BYTE* buffer = nullptr;
    if (!p.Check(p.render->GetBuffer(available,&buffer),"get buffer")) return false;
    const auto channels = p.format->nChannels;
    std::memset(buffer,0,static_cast<std::size_t>(available)*p.format->nBlockAlign);
    for (UINT32 frame=0;frame<available;++frame) {
        float sample = 0;
        for (auto& voice : p.voices) {
            const auto& clip = p.clips[voice.cue];
            if (voice.position < clip.size()) sample += clip[voice.position++];
        }
        sample = std::clamp(sample,-1.0f,1.0f);
        // Front pair carries speaker/headphone audio; quad actuator channels
        // remain zero. Motor effects travel through HID legacy rumble here.
        for (unsigned channel=0;channel<std::min<unsigned>(channels,2);++channel) {
            const auto index = static_cast<std::size_t>(frame)*channels+channel;
            if (p.floatSamples) std::memcpy(buffer+index*4,&sample,4);
            else {
                const auto value = static_cast<std::int16_t>(std::lround(sample*32767));
                std::memcpy(buffer+index*2,&value,2);
            }
        }
    }
    std::erase_if(p.voices,[&p](const Impl::Voice& v) { return v.position >= p.clips[v.cue].size(); });
    return p.Check(p.render->ReleaseBuffer(available,0),"release buffer");
}
void WasapiSpeakerOutput::Stop() {
    auto& p = *impl_; p.voices.clear();
    if (!p.client) return;
    if (!p.Check(p.client->Stop(),"stop") || p.fault || !p.Check(p.client->Reset(),"reset")) return;
    if (Pump()) p.Check(p.client->Start(),"restart silence");
}
}
