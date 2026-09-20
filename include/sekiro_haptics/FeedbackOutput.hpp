#pragma once
#include "sekiro_haptics/ActionFeedback.hpp"
#include "sekiro_haptics/IDualSenseTransport.hpp"

namespace sekiro_haptics {
class ISpeakerOutput {
public:
    virtual ~ISpeakerOutput() = default;
    virtual bool Play(SpeakerCue cue) = 0;
    virtual bool Pump() = 0;
    virtual void Stop() = 0;
};

// Single writer. Transport and speaker must outlive this object. A failed
// write latches a fault; reconnect explicitly with a new neutral session.
class FeedbackOutput {
public:
    FeedbackOutput(IDualSenseTransport& transport, ISpeakerOutput* speaker = nullptr);
    ~FeedbackOutput();
    bool ConfigureSpeakerRouting(); // call after explicitly opening the USB audio endpoint
    bool Send(const FeedbackFrame& frame);
    bool PumpAudio();
    bool Stop();
    bool Close(); // neutral + relinquish speaker routing; idempotent
    bool Faulted() const { return faulted_; }
private:
    IDualSenseTransport& transport_;
    ISpeakerOutput* speaker_;
    bool faulted_ = false;
    bool speakerRouting_ = false;
    bool closed_ = false;
};
}
