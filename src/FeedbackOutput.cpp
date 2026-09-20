#include "sekiro_haptics/FeedbackOutput.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"

namespace sekiro_haptics {
FeedbackOutput::FeedbackOutput(IDualSenseTransport& transport, ISpeakerOutput* speaker)
    : transport_(transport), speaker_(speaker) {}
FeedbackOutput::~FeedbackOutput() { Close(); }
bool FeedbackOutput::ConfigureSpeakerRouting() {
    if (!speaker_ || closed_ || faulted_) return false;
    // Remember ownership even on a failed/partial write so Close attempts release.
    speakerRouting_ = true;
    const auto report = dualsense_protocol::BuildSpeakerRoutingReport(true);
    if (transport_.WriteOutputReport(report.data(),report.size()) == TransportResult::Success) return true;
    faulted_ = true; Stop(); return false;
}
bool FeedbackOutput::Stop() {
    if (closed_) return !faulted_;
    if (speaker_) speaker_->Stop();
    const auto neutral = dualsense_protocol::BuildFeedbackReport({});
    const bool ok = transport_.WriteOutputReport(neutral.data(),neutral.size()) == TransportResult::Success;
    faulted_ = faulted_ || !ok;
    return ok;
}
bool FeedbackOutput::Close() {
    if (closed_) return !faulted_;
    bool ok = Stop();
    if (speakerRouting_) {
        const auto report = dualsense_protocol::BuildSpeakerRoutingReport(false);
        ok = transport_.WriteOutputReport(report.data(),report.size()) == TransportResult::Success && ok;
        speakerRouting_ = false;
    }
    closed_ = true; faulted_ = faulted_ || !ok;
    return ok;
}
bool FeedbackOutput::Send(const FeedbackFrame& frame) {
    if (faulted_ || closed_) return false;
    const auto report = dualsense_protocol::BuildFeedbackReport(frame.state);
    if (transport_.WriteOutputReport(report.data(),report.size()) != TransportResult::Success) {
        faulted_ = true; Stop(); return false;
    }
    if (speaker_) {
        if (frame.stopAudio) speaker_->Stop();
        for (auto cue : frame.cues) {
            if (!speaker_->Play(cue)) { faulted_ = true; Stop(); return false; }
        }
    }
    return true;
}
bool FeedbackOutput::PumpAudio() {
    if (faulted_ || closed_) return false;
    if (speaker_ && !speaker_->Pump()) { faulted_ = true; Stop(); return false; }
    return true;
}
}
