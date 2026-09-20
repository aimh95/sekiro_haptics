#include "sekiro_haptics/ActionFeedback.hpp"
#include "sekiro_haptics/ActionScript.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "sekiro_haptics/FeedbackOutput.hpp"
#ifdef SH_ACTION_HID
#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#endif
#ifdef SH_ACTION_AUDIO
#include "sekiro_haptics/WasapiSpeakerOutput.hpp"
#endif
#ifdef _WIN32
#include <conio.h>
#endif
#include <chrono>
#include <csignal>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace sekiro_haptics;
namespace {
volatile std::sig_atomic_t stopped = 0;
void OnSignal(int) { stopped = 1; }
void Help() {
    std::cout << "Sekiro action feedback: authored DEMO/REPLAY, not automatic Sekiro detection.\n"
        "  --scenario <JSONL> [--dry-run] [--fast]\n"
        "  --manual                       Windows console keys, no game attachment\n"
        "  --list-devices                  list USB controllers and audio endpoints\n"
        "  --hardware --device <index>     explicit USB output; default is dry-run\n"
        "  --audio-device <index>          explicitly choose the controller render endpoint\n"
        "  --rumble-gain <0..1> --trigger-gain <0..1> --speaker-gain <0..1>\n"
        "Manual keys: p deflect, b block, d damage, n next tool, u use, j launch, k latch,\n"
        "             l pull, e end, space pause, r resume, q quit.\n";
}
int Index(const std::string& text) {
    std::size_t end = 0; int n = std::stoi(text,&end);
    if (end != text.size() || n < 0) throw std::runtime_error("device index must be nonnegative integer");
    return n;
}
float Gain(const std::string& text) {
    std::size_t end = 0; float n = std::stof(text,&end);
    if (end != text.size() || !std::isfinite(n) || n < 0 || n > 1) throw std::runtime_error("gain must be 0..1");
    return n;
}
}
int main(int argc, char** argv) {
    try {
        bool hardware = false, dryRequested = false, list = false, fast = false, manual = false;
        int deviceIndex = -1, audioIndex = -1;
        float speakerGain = .20f;
        FeedbackOptions options;
        std::string scenario;
        for (int i=1;i<argc;++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string { if (++i >= argc) throw std::runtime_error("missing value for "+arg); return argv[i]; };
            if (arg == "--help") { Help(); return 0; }
            else if (arg == "--hardware") hardware = true;
            else if (arg == "--dry-run") dryRequested = true;
            else if (arg == "--list-devices") list = true;
            else if (arg == "--fast") fast = true;
            else if (arg == "--manual") manual = true;
            else if (arg == "--scenario") scenario = value();
            else if (arg == "--device") deviceIndex = Index(value());
            else if (arg == "--audio-device") audioIndex = Index(value());
            else if (arg == "--rumble-gain") options.rumbleGain = Gain(value());
            else if (arg == "--trigger-gain") options.triggerGain = Gain(value());
            else if (arg == "--speaker-gain") speakerGain = Gain(value());
            else throw std::runtime_error("unknown argument: "+arg);
        }
        if ((hardware && (fast || dryRequested)) || (manual && fast) || (audioIndex >= 0 && !hardware))
            throw std::runtime_error("incompatible output options");
        if (manual && !scenario.empty()) throw std::runtime_error("choose manual or scenario");
#ifndef _WIN32
        if (manual) throw std::runtime_error("manual keyboard mode requires Windows");
#endif
        std::ostringstream transportLog;
#ifdef SH_ACTION_HID
        HidApiDualSenseTransport transport(transportLog);
        const auto devices = (list || hardware) ? transport.EnumerateCandidates() : std::vector<HidDeviceInfo>{};
        if (list) for (std::size_t i=0;i<devices.size();++i)
            std::cout << "USB " << i << ": " << devices[i].path << '\n';
#else
        if (hardware) throw std::runtime_error("built without HIDAPI transport; configure with transport ON");
#endif
#ifdef SH_ACTION_AUDIO
        const auto audioDevices = (list || audioIndex >= 0) ? WasapiSpeakerOutput::Enumerate() : std::vector<AudioEndpoint>{};
        if (list) for (std::size_t i=0;i<audioDevices.size();++i)
            std::wcout << L"Audio " << i << L": " << audioDevices[i].name << L"\n  " << audioDevices[i].id << L'\n';
#else
        if (audioIndex >= 0) throw std::runtime_error("controller speaker output requires Windows WASAPI build");
#endif
        if (list) return 0;
        if (!manual && scenario.empty()) { Help(); return 2; }
        std::vector<TimedAction> events;
        if (!manual) {
            std::ifstream input(scenario);
            if (!input) throw std::runtime_error("cannot open scenario");
            events = LoadActionScript(input); // validate entire file before hardware output
        }
        std::unique_ptr<ISpeakerOutput> speaker;
        std::unique_ptr<FeedbackOutput> output;
#ifdef SH_ACTION_HID
        if (hardware) {
            if (deviceIndex < 0 || static_cast<std::size_t>(deviceIndex) >= devices.size())
                throw std::runtime_error("choose a USB controller using --list-devices then --device");
            if (transport.Open(devices[deviceIndex].path) != TransportResult::Success)
                throw std::runtime_error("USB controller open failed");
            // Establish an output owner immediately so later setup failures neutralize.
            output = std::make_unique<FeedbackOutput>(transport);
            if (!output->Stop()) throw std::runtime_error("initial neutral report failed");
#ifdef SH_ACTION_AUDIO
            if (audioIndex >= 0) {
                if (static_cast<std::size_t>(audioIndex) >= audioDevices.size()) throw std::runtime_error("invalid audio index");
                auto audio = std::make_unique<WasapiSpeakerOutput>();
                std::wcout << L"Selected audio endpoint: " << audioDevices[audioIndex].name << L'\n';
                if (!audio->Open(audioDevices[audioIndex].id,speakerGain)) throw std::runtime_error(audio->Error());
                output.reset();
                speaker = std::move(audio);
                output = std::make_unique<FeedbackOutput>(transport,speaker.get());
                if (!output->ConfigureSpeakerRouting()) throw std::runtime_error("speaker routing report failed");
            }
#endif
        }
#endif
        (void)speakerGain; (void)deviceIndex;
        std::signal(SIGINT,OnSignal); std::signal(SIGTERM,OnSignal);
        std::cout << "Mode: " << (hardware ? "USB hardware" : "dry-run") << "; event source: "
                  << (manual ? "manual commands" : "authored scenario") << "; NOT live Sekiro detection.\n";
        if (manual) Help();
        SekiroFeedbackEngine engine(options);
        std::uint64_t sequence = 0;
        std::size_t next = 0;
#ifdef _WIN32
        unsigned selected = 0;
#endif
        const auto start = std::chrono::steady_clock::now();
        std::int64_t nextFrame = 0, syntheticNow = 0;
        const auto endUs = manual ? std::int64_t{0} : events.back().atUs+800'000;
        std::array<std::uint8_t,64> previous{};
        auto submit = [&](SekiroAction action, Prosthetic tool, std::int64_t now) {
            if (!engine.Submit({action,tool,sequence++},now)) throw std::runtime_error("action ordering rejected");
            if (action != SekiroAction::Heartbeat)
                std::cout << "action t=" << now << " " << ToString(action) << " " << ToString(tool) << '\n';
        };
        while (!stopped) {
            const auto now = fast ? syntheticNow : std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-start).count();
            if (!manual && now > endUs) break;
#ifdef _WIN32
            if (manual && _kbhit()) {
                const int key = _getch();
                if (key == 'q') break;
                if (key == 'n') {
                    selected = selected%10+1;
                    submit(SekiroAction::SelectProsthetic,static_cast<Prosthetic>(selected),now);
                } else {
                    const std::string keys = "pbdujkle r";
                    const std::array<SekiroAction,10> actions{SekiroAction::Deflect,SekiroAction::Block,SekiroAction::Damage,
                        SekiroAction::UseProsthetic,SekiroAction::GrappleLaunch,SekiroAction::GrappleLatch,
                        SekiroAction::GrapplePull,SekiroAction::GrappleEnd,SekiroAction::Pause,SekiroAction::Resume};
                    const auto found = keys.find(static_cast<char>(key));
                    if (found != std::string::npos) submit(actions[found],Prosthetic::None,now);
                }
            }
#endif
            while (next < events.size() && events[next].atUs <= now) {
                // Do not replay a burst of obsolete impacts after a long stall.
                if (!fast && now-events[next].atUs > options.sourceTimeoutUs)
                    throw std::runtime_error("scenario scheduling gap: stopped before stale actions");
                submit(events[next].action,events[next].tool,now); ++next;
            }
            if (now >= nextFrame) {
                // Demo producer heartbeat only. A future game adapter must supply
                // actual observation health; it must not synthesize this heartbeat.
                submit(SekiroAction::Heartbeat,Prosthetic::None,now);
                auto frame = engine.Tick(now);
                const auto report = dualsense_protocol::BuildFeedbackReport(frame.state);
                if (report != previous || !frame.cues.empty()) {
                    std::cout << "frame t=" << now << " motors=" << unsigned(report[4]) << ',' << unsigned(report[3])
                        << " L2=" << unsigned(report[23]) << ':' << unsigned(report[24])
                        << " R2=" << unsigned(report[12]) << ':' << unsigned(report[13]);
                    for (auto cue : frame.cues) std::cout << " sound=" << ToString(cue);
                    std::cout << '\n'; previous = report;
                }
                if (output && !output->Send(frame)) throw std::runtime_error("feedback output failed; session stopped");
                nextFrame = now+10'000;
                transportLog.str(""); transportLog.clear(); // keep transport diagnostics bounded
            }
            if (output && !output->PumpAudio()) throw std::runtime_error("speaker output failed; session stopped");
            if (fast) syntheticNow += 5'000;
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        engine.Reset();
        if (output && !output->Close()) throw std::runtime_error("final neutral/release report failed (controller may be disconnected)");
        std::cout << "Stopped; motors and triggers neutral.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n'; return 1;
    }
}
