#pragma once
#include "sekiro_haptics/FeedbackOutput.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sekiro_haptics {
struct AudioEndpoint { std::wstring id; std::wstring name; };
// Windows-only USB audio implementation, linked separately from the core.
// The caller must explicitly select the controller's render endpoint.
// No default-device fallback, system volume change, or microphone access.
class WasapiSpeakerOutput final : public ISpeakerOutput {
public:
    WasapiSpeakerOutput();
    ~WasapiSpeakerOutput() override;
    WasapiSpeakerOutput(const WasapiSpeakerOutput&) = delete;
    WasapiSpeakerOutput& operator=(const WasapiSpeakerOutput&) = delete;
    static std::vector<AudioEndpoint> Enumerate();
    bool Open(const std::wstring& endpointId, float gain = .20f);
    const std::string& Error() const;
    bool Play(SpeakerCue cue) override;
    bool Pump() override; // call every 5 ms on the creating thread
    void Stop() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
