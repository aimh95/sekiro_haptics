// Hardware integration entry point (OUT-LEGACY-002): replays a versioned
// JSONL trace through the full pipeline and dispatches onto a REAL,
// USB-connected DualSense via DualSenseLegacyBackend.
//
// This is the only place in the repo that wires a real hardware backend to
// the replay pipeline -- apps/replay_cli never links HIDAPI and never
// touches a real controller, so running *this* executable is the explicit
// "I want real hardware output" choice; nothing else makes it for you.
//
// Always uses RunReplayLoopStrict(): a trace with no metadata sidecar (or
// any other validation problem) never reaches the device -- see
// docs/03-trace-format.md. No non-zero report is ever sent before trace
// validation finishes.
//
// Usage:
//   sekiro_haptics_replay_hardware --trace <path.jsonl> --presets <presets.json>
//                                   --mappings <mappings.json>

#include "sekiro_haptics/DualSenseLegacyBackend.hpp"
#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "sekiro_haptics/pipeline/ReplayPipeline.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace sekiro_haptics;

struct CliOptions {
    std::string tracePath;
    std::string presetsPath;
    std::string mappingsPath;
};

bool ParseArgs(int argc, char** argv, CliOptions& outOptions, std::string& outError) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto takeValue = [&](const char* flag, std::string& outValue) -> bool {
            if (i + 1 >= argc) {
                outError = std::string(flag) + " requires a value";
                return false;
            }
            outValue = argv[++i];
            return true;
        };

        if (arg == "--trace") {
            if (!takeValue("--trace", outOptions.tracePath)) return false;
        } else if (arg == "--presets") {
            if (!takeValue("--presets", outOptions.presetsPath)) return false;
        } else if (arg == "--mappings") {
            if (!takeValue("--mappings", outOptions.mappingsPath)) return false;
        } else {
            outError = "unknown argument: " + arg;
            return false;
        }
    }

    if (outOptions.tracePath.empty() || outOptions.presetsPath.empty() || outOptions.mappingsPath.empty()) {
        outError = "usage: sekiro_haptics_replay_hardware --trace <path> --presets <path> --mappings <path>";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace std::chrono_literals;

    CliOptions options;
    std::string error;
    if (!ParseArgs(argc, argv, options, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    PresetRepository presets;
    PresetLoadOutcome presetOutcome = presets.LoadFromFile(options.presetsPath);
    if (!presetOutcome.ok) {
        std::cerr << "failed to load presets from " << options.presetsPath << ": " << presetOutcome.fatalError
                   << "\n";
        return 1;
    }
    std::cout << "Loaded " << presetOutcome.loadedCount << " preset(s) from " << options.presetsPath << "\n";

    MappingRepository mappings;
    MappingLoadOutcome mappingOutcome = mappings.LoadFromFile(options.mappingsPath);
    if (!mappingOutcome.ok) {
        std::cerr << "failed to load mappings from " << options.mappingsPath << ": " << mappingOutcome.fatalError
                   << "\n";
        return 1;
    }
    std::cout << "Loaded " << mappingOutcome.loadedCount << " mapping(s) from " << options.mappingsPath << "\n";

    HidApiDualSenseTransport transport;
    auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) {
        std::cerr << "No DualSense found over USB. Plug one in and try again.\n";
        return 1;
    }
    if (transport.Open(candidates.front().path) != TransportResult::Success) {
        std::cerr << "Failed to open DualSense.\n";
        return 1;
    }
    std::cout << "Opened DualSense.\n";

    DualSenseLegacyBackend backend(transport);
    HapticScheduler scheduler(backend);
    ManualLabelEventDetector detector;
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::cout << "\nValidating and replaying " << options.tracePath << " (strict -- no metadata, no hardware output)...\n";

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformedLines;
    std::vector<std::string> validationErrors;

    bool replayed =
        RunReplayLoopStrict(options.tracePath, pipeline, outcomes, malformedLines, validationErrors);

    if (!replayed) {
        std::cerr << "Trace validation failed -- no hardware output was sent:\n";
        for (const auto& validationError : validationErrors) {
            std::cerr << "  " << validationError << "\n";
        }
        transport.Close();
        return 1;
    }

    int dispatchedCount = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.stage != PipelineStage::NoEventDetected) {
            std::cout << "[event] " << outcome.event->gameId << "/" << outcome.event->eventId << "\n";
        }
        if (outcome.stage == PipelineStage::Dispatched) {
            ++dispatchedCount;
            std::cout << "  -> dispatched preset " << *outcome.presetId << "\n";
        } else if (outcome.stage != PipelineStage::NoEventDetected) {
            std::cout << "  -> stopped at " << ToString(outcome.stage) << ": " << ToString(outcome.error) << "\n";
        }
    }
    std::cout << "\nDispatched " << dispatchedCount << " effect(s) to hardware.\n";

    // Each dispatched effect now auto-stops on its own (HapticScheduler
    // fires backend.Reset() -- logged by DualSenseLegacyBackend as "Stop
    // requested" -- after that effect's own `duration` elapses; see
    // HapticScheduler.hpp). This wait is just CLI-presentation pacing so
    // the program doesn't exit mid-effect before that automatic stop has
    // had a chance to happen; it is not what stops the controller.
    std::this_thread::sleep_for(1s);

    // Safety net only, per docs/03-trace-format.md / HapticScheduler's
    // contract: every effect should already be stopped by the automatic
    // duration-based Reset() above. This final call exists only to cover
    // "somehow still active at exit" (e.g. this program is killed and
    // restarted moments after a very long custom duration), not as the
    // mechanism that ends normal playback.
    HapticBackendResult finalResetResult = backend.Reset();
    std::cout << "Final safety reset result: " << ToString(finalResetResult) << "\n";

    transport.Close();
    std::cout << "Done.\n";
    return 0;
}
