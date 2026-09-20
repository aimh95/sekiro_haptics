#include "sekiro_haptics/ActionFeedback.hpp"
#include "sekiro_haptics/ActionScript.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "sekiro_haptics/FeedbackOutput.hpp"
#include "FakeDualSenseTransport.hpp"
#include "testing.hpp"
#include <cmath>
#include <limits>
#include <sstream>
using namespace sekiro_haptics;
namespace {
bool Neutral(const FeedbackFrame& f) {
    return f.state.leftMotor == 0 && f.state.rightMotor == 0 &&
        f.state.leftTrigger.force == 0 && f.state.rightTrigger.force == 0;
}
struct FakeSpeaker : ISpeakerOutput {
    unsigned plays = 0, stops = 0; bool fail = false;
    bool Play(SpeakerCue) override { ++plays; return !fail; }
    bool Pump() override { return !fail; }
    void Stop() override { ++stops; }
};
}
SH_TEST(ActionFeedback_RapidDeflectsEmitSeparateCuesWithoutGuardDebounce) {
    SekiroFeedbackEngine e;
    SH_CHECK(e.Submit({SekiroAction::Deflect,Prosthetic::None,1},0));
    auto f = e.Tick(0);
    SH_CHECK(f.cues.size() == 1 && f.state.rightMotor > .5f);
    SH_CHECK(e.Submit({SekiroAction::Deflect,Prosthetic::None,2},60'000));
    f = e.Tick(60'000);
    SH_CHECK(f.cues.size() == 1 && f.state.rightMotor > .5f);
    SH_CHECK(!e.Submit({SekiroAction::Deflect,Prosthetic::None,2},60'000));
    SH_CHECK(e.Tick(70'000).cues.empty());
    SH_CHECK(Neutral(e.Tick(200'000)));
}
SH_TEST(ActionFeedback_SelectedToolsDifferAndUseReturnsToIdle) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::Shuriken,0},0);
    auto shuriken = e.Tick(0).state.rightTrigger.force;
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::Axe,1},10'000);
    auto idle = e.Tick(10'000).state.rightTrigger.force;
    SH_CHECK(idle > shuriken);
    e.Submit({SekiroAction::UseProsthetic,Prosthetic::None,2},20'000);
    SH_CHECK(e.Tick(20'000).state.rightTrigger.force > idle);
    SH_CHECK(e.Tick(400'000).state.rightTrigger.force == idle);
}
SH_TEST(ActionFeedback_UnknownToolCannotRetainOldResistance) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::Axe,0},0);
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::None,1},10'000);
    e.Submit({SekiroAction::UseProsthetic,Prosthetic::None,2},20'000);
    SH_CHECK(Neutral(e.Tick(20'000)));
}
SH_TEST(ActionFeedback_GrapplePhasesTensionPulseAndRelease) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::GrapplePull,Prosthetic::None,0},0);
    SH_CHECK(Neutral(e.Tick(0))); // cannot pull before launch/latch
    e.Submit({SekiroAction::GrappleLaunch,Prosthetic::None,1},100'000);
    auto launch = e.Tick(100'000).state.leftTrigger.force;
    e.Submit({SekiroAction::GrappleLatch,Prosthetic::None,2},250'000);
    SH_CHECK(e.Tick(250'000).state.leftTrigger.force > launch);
    e.Submit({SekiroAction::GrapplePull,Prosthetic::None,3},300'000);
    auto low = e.Tick(300'000).state.leftTrigger.force;
    SH_CHECK(e.Tick(331'250).state.leftTrigger.force > low);
    e.Submit({SekiroAction::GrappleEnd,Prosthetic::None,4},450'000);
    SH_CHECK(Neutral(e.Tick(450'000)));
}
SH_TEST(ActionFeedback_PullHasHardLimitEvenWithHeartbeat) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::GrappleLaunch,Prosthetic::None,0},0);
    e.Submit({SekiroAction::GrappleLatch,Prosthetic::None,1},1);
    e.Submit({SekiroAction::GrapplePull,Prosthetic::None,2},2);
    for (unsigned i=1;i<=6;++i) {
        e.Submit({SekiroAction::Heartbeat,Prosthetic::None,i+2},i*450'000);
        auto frame = e.Tick(i*450'000);
        if (i == 6) SH_CHECK(Neutral(frame));
    }
}
SH_TEST(ActionFeedback_SourceLossReleasesAndRequiresReselect) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::Axe,1},0);
    e.Submit({SekiroAction::UseProsthetic,Prosthetic::None,2},10'000);
    auto lost = e.Tick(510'001);
    SH_CHECK(Neutral(lost) && lost.stopAudio && lost.cues.empty());
    SH_CHECK(e.SelectedTool() == Prosthetic::None);
    e.Submit({SekiroAction::Heartbeat,Prosthetic::None,3},520'000);
    SH_CHECK(Neutral(e.Tick(520'000)));
}
SH_TEST(ActionFeedback_PauseCannotBeUndoneByQueuedActionsOrHeartbeat) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::SelectProsthetic,Prosthetic::Axe,0},0);
    e.Submit({SekiroAction::Pause,Prosthetic::None,1},10'000);
    e.Submit({SekiroAction::Deflect,Prosthetic::None,2},20'000);
    e.Submit({SekiroAction::Heartbeat,Prosthetic::None,3},30'000);
    auto f = e.Tick(30'000);
    SH_CHECK(Neutral(f) && f.cues.empty());
    e.Submit({SekiroAction::Resume,Prosthetic::None,4},40'000);
    e.Submit({SekiroAction::Deflect,Prosthetic::None,5},50'000);
    SH_CHECK(e.Tick(50'000).cues.size() == 1);
}
SH_TEST(ActionFeedback_ClockReversalNeutralizesAndDiscardsAudio) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::Deflect,Prosthetic::None,1},50'000);
    SH_CHECK(!e.Submit({SekiroAction::Deflect,Prosthetic::None,2},40'000));
    auto frame = e.Tick(40'000);
    SH_CHECK(Neutral(frame) && frame.cues.empty() && frame.stopAudio);
}
SH_TEST(ActionFeedback_StalledAudioCueDoesNotPlayAfterImpactExpires) {
    SekiroFeedbackEngine e;
    e.Submit({SekiroAction::Deflect,Prosthetic::None,1},0);
    auto frame = e.Tick(200'000);
    SH_CHECK(Neutral(frame) && frame.cues.empty());
}
SH_TEST(ActionFeedback_AllToolsAndActionsRoundTripAndUnknownsFail) {
    for (int i=0;i<11;++i) SH_CHECK(ParseProsthetic(ToString(static_cast<Prosthetic>(i))) == static_cast<Prosthetic>(i));
    for (int i=0;i<16;++i) SH_CHECK(ParseSekiroAction(ToString(static_cast<SekiroAction>(i))) == static_cast<SekiroAction>(i));
    SH_CHECK(!ParseSekiroAction("memory_flag_1"));
    SH_CHECK(!ParseProsthetic("unverified_id_123"));
}
SH_TEST(ActionFeedback_UsbReportEncodesEachSideAndExplicitRelease) {
    FeedbackState state;
    state.leftMotor = 1; state.rightMotor = .5f;
    state.leftTrigger = {.25f,.5f}; state.rightTrigger = {.5f,1};
    auto r = dualsense_protocol::BuildFeedbackReport(state);
    SH_CHECK(r[0] == 2 && r[1] == 15 && r[2] == 0);
    SH_CHECK(r[4] == 255 && r[3] == 128);
    SH_CHECK(r[11] == 1 && r[12] == 128 && r[13] == 255);
    SH_CHECK(r[22] == 1 && r[23] == 64 && r[24] == 128);
    for (std::size_t i=5;i<11;++i) SH_CHECK(r[i] == 0); // unrelated controls unchanged
    r = dualsense_protocol::BuildFeedbackReport({});
    SH_CHECK(r[1] == 15 && r[11] == 0 && r[22] == 0 && r[3] == 0 && r[4] == 0);
}
SH_TEST(ActionFeedback_NonfiniteParametersCannotProduceForce) {
    FeedbackState state;
    state.leftMotor = std::numeric_limits<float>::quiet_NaN();
    state.rightMotor = 2;
    state.leftTrigger = {.2f,std::numeric_limits<float>::infinity()};
    state.rightTrigger = {std::numeric_limits<float>::quiet_NaN(),1};
    auto r = dualsense_protocol::BuildFeedbackReport(state);
    SH_CHECK(r[4] == 0 && r[3] == 255 && r[11] == 0 && r[22] == 0);
}
SH_TEST(ActionFeedback_SpeakerRoutingUsesDedicatedFlagsAndReleasesOnClose) {
    auto report = dualsense_protocol::BuildSpeakerRoutingReport(true);
    SH_CHECK(report[0] == 2 && report[1] == 0xa0 && report[2] == 0);
    SH_CHECK(report[6] == 0x50 && report[8] == 0x30);
    SH_CHECK(report[7] == 0 && report[10] == 0 && report[11] == 0 && report[22] == 0);
    FakeDualSenseTransport transport; transport.SetOpen(true);
    FakeSpeaker speaker; FeedbackOutput output(transport,&speaker);
    SH_CHECK(output.ConfigureSpeakerRouting());
    SH_CHECK(output.Close());
    auto all = transport.WrittenReports();
    SH_CHECK(all.size() == 3 && all[1][1] == 15 && all[1][11] == 0);
    SH_CHECK(all[2][1] == 0xa0 && all[2][6] == 0 && all[2][8] == 0);
    SH_CHECK(output.Close() && transport.WrittenReports().size() == 3);
    SH_CHECK(!output.Send({}));
}
SH_TEST(ActionFeedback_ProceduralAudioIsFiniteBoundedAndDistinct) {
    const auto metal = SynthesizeCue(SpeakerCue::Deflect,48'000,.2f);
    const auto block = SynthesizeCue(SpeakerCue::Block,48'000,.2f);
    SH_CHECK(metal.size() > block.size() && metal.front() == 0);
    float energy = 0;
    for (auto sample : metal) { SH_CHECK(std::isfinite(sample) && std::abs(sample) <= .2f); energy += sample*sample; }
    SH_CHECK(energy > 1);
    for (auto sample : SynthesizeCue(SpeakerCue::Deflect,48'000,0)) SH_CHECK(sample == 0);
}
SH_TEST(ActionFeedback_OutputFailureNeutralizesAndCannotReplayStaleState) {
    FakeDualSenseTransport transport; transport.SetOpen(true);
    FakeSpeaker speaker;
    FeedbackOutput output(transport,&speaker);
    FeedbackFrame frame; frame.state.rightTrigger = {.1f,.8f}; frame.cues.push_back(SpeakerCue::Deflect);
    SH_CHECK(output.Send(frame) && speaker.plays == 1);
    transport.FailNextWrite();
    SH_CHECK(!output.Send(frame) && output.Faulted());
    auto reports = transport.WrittenReports();
    SH_CHECK(reports.back()[11] == 0 && reports.back()[22] == 0);
    SH_CHECK(speaker.stops == 1 && speaker.plays == 1);
    SH_CHECK(!output.Send(frame));
}
SH_TEST(ActionFeedback_AudioFailureAlsoReleasesTriggers) {
    FakeDualSenseTransport transport; transport.SetOpen(true);
    FakeSpeaker speaker; FeedbackOutput output(transport,&speaker);
    speaker.fail = true;
    SH_CHECK(!output.PumpAudio() && output.Faulted());
    SH_CHECK(transport.WrittenReports().back()[22] == 0);
}
SH_TEST(ActionFeedback_ScriptValidationBeforeOutput) {
    std::istringstream good("{\"atMs\":0,\"action\":\"select\",\"tool\":\"axe\"}\n{\"atMs\":100,\"action\":\"use\"}\n");
    auto events = LoadActionScript(good);
    SH_CHECK(events.size() == 2 && events[1].atUs == 100'000 && events[0].tool == Prosthetic::Axe);
    for (const auto* text : {"{\"atMs\":-1,\"action\":\"deflect\"}","{\"atMs\":0,\"action\":\"select\",\"tool\":\"bad\"}",
                            "{\"atMs\":0,\"action\":\"deflect\",\"tool\":\"axe\"}","{\"atMs\":0.5,\"action\":\"deflect\"}"}) {
        bool threw = false;
        try { std::istringstream input(text); LoadActionScript(input); } catch (const std::exception&) { threw = true; }
        SH_CHECK(threw);
    }
}
