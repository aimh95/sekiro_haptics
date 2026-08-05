// Developer CLI: replays a recorded JSONL signal trace through the full
// GameSignal -> event detector -> mapping -> preset -> HapticScheduler ->
// MockHapticBackend pipeline. Never touches a real game or real hardware --
// see docs/ARCHITECTURE.md and docs/trace-format.md.
//
// Usage:
//   sekiro_haptics_replay --trace <path.jsonl> --presets <presets.json>
//                          --mappings <mappings.json> [--fast]
//
// --fast replays as fast as the trace can be read, ignoring the delays
// between signal timestamps; without it, the CLI sleeps between signals to
// approximate real-time playback.

#include "sekiro_haptics/MockHapticBackend.hpp"
#include "sekiro_haptics/events/ManualLabelEventDetector.hpp"
#include "sekiro_haptics/pipeline/ReplayPipeline.hpp"
#include "sekiro_haptics/signals/ReplaySignalSource.hpp"

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
    bool fast = false;
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
        } else if (arg == "--fast") {
            outOptions.fast = true;
        } else {
            outError = "unknown argument: " + arg;
            return false;
        }
    }

    if (outOptions.tracePath.empty() || outOptions.presetsPath.empty() || outOptions.mappingsPath.empty()) {
        outError =
            "usage: sekiro_haptics_replay --trace <path> --presets <path> --mappings <path> [--fast]";
        return false;
    }
    return true;
}

void PrintLoadWarnings(const char* kind, const std::vector<PresetLoadError>& errors) {
    for (const auto& error : errors) {
        std::cout << "  [warning] " << kind << " entry " << error.entryIndex << ": " << error.message << "\n";
    }
}

void PrintLoadWarnings(const char* kind, const std::vector<MappingLoadError>& errors) {
    for (const auto& error : errors) {
        std::cout << "  [warning] " << kind << " entry " << error.entryIndex << ": " << error.message << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
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
    PrintLoadWarnings("preset", presetOutcome.errors);

    MappingRepository mappings;
    MappingLoadOutcome mappingOutcome = mappings.LoadFromFile(options.mappingsPath);
    if (!mappingOutcome.ok) {
        std::cerr << "failed to load mappings from " << options.mappingsPath << ": " << mappingOutcome.fatalError
                   << "\n";
        return 1;
    }
    std::cout << "Loaded " << mappingOutcome.loadedCount << " mapping(s) from " << options.mappingsPath << "\n";
    PrintLoadWarnings("mapping", mappingOutcome.errors);

    ReplaySignalSource source(options.tracePath);
    if (!source.IsOpen()) {
        std::cerr << "failed to open trace file: " << options.tracePath << "\n";
        return 1;
    }

    ManualLabelEventDetector detector;
    MockHapticBackend backend;
    HapticScheduler scheduler(backend);
    ReplayPipeline pipeline(detector, presets, mappings, scheduler, backend);

    std::cout << "\nReplaying " << options.tracePath << (options.fast ? " (fast mode)" : " (real-time mode)")
              << "...\n";

    std::vector<PipelineStepOutcome> outcomes;
    std::vector<std::string> malformedLines;
    std::size_t signalCount = 0;

    if (options.fast) {
        // RunReplayLoop is shared with the test suite -- it stays
        // sleep-free/deterministic on purpose, which is exactly what fast
        // mode wants.
        signalCount = RunReplayLoop(source, pipeline, outcomes, malformedLines);
    } else {
        GameSignal signal;
        bool haveLastTimestamp = false;
        std::chrono::microseconds lastTimestamp{0};

        while (true) {
            std::string readError;
            SignalSourceResult result = source.Next(signal, &readError);
            if (result == SignalSourceResult::EndOfSource) {
                break;
            }
            if (result == SignalSourceResult::MalformedInput) {
                malformedLines.push_back(readError);
                continue;
            }

            if (haveLastTimestamp && signal.timestamp > lastTimestamp) {
                std::this_thread::sleep_for(signal.timestamp - lastTimestamp);
            }
            haveLastTimestamp = true;
            lastTimestamp = signal.timestamp;

            ++signalCount;
            pipeline.ProcessSignal(signal, outcomes);
        }
        pipeline.Flush(outcomes);
    }

    for (const auto& line : malformedLines) {
        std::cout << "[malformed trace line] " << line << "\n";
    }

    int eventsDetected = 0;
    int dispatchedCount = 0;
    int pipelineErrors = 0;
    int signalsWithNoEvent = 0;

    for (const auto& outcome : outcomes) {
        if (outcome.stage == PipelineStage::NoEventDetected) {
            ++signalsWithNoEvent;
            continue;
        }

        ++eventsDetected;
        const GameEvent& event = *outcome.event;
        std::cout << "[event] " << event.gameId << "/" << event.eventId << " @ " << event.timestamp.count()
                  << "us\n";

        if (outcome.presetId.has_value()) {
            std::cout << "  -> preset: " << *outcome.presetId << "\n";
        }

        if (outcome.stage == PipelineStage::Dispatched) {
            ++dispatchedCount;
            std::cout << "  -> dispatched (scheduler id " << *outcome.scheduledId << ")\n";
        } else {
            ++pipelineErrors;
            std::cout << "  -> stopped at " << ToString(outcome.stage) << ": " << ToString(outcome.error) << " ("
                      << outcome.detail << ")\n";
        }
    }

    if (dispatchedCount > 0) {
        // Give the scheduler's background worker a moment to actually
        // deliver dispatched effects to the backend before summarizing.
        backend.WaitForEffectCount(static_cast<std::size_t>(dispatchedCount), std::chrono::seconds(2));
    }

    std::cout << "\n--- Summary ---\n";
    std::cout << "Signals read: " << signalCount << "\n";
    std::cout << "Malformed trace lines skipped: " << malformedLines.size() << "\n";
    std::cout << "Signals with no event: " << signalsWithNoEvent << "\n";
    std::cout << "Events detected: " << eventsDetected << "\n";
    std::cout << "Effects dispatched: " << dispatchedCount << "\n";
    std::cout << "Pipeline errors: " << pipelineErrors << "\n";
    std::cout << "Effects received by backend: " << backend.History().size() << "\n";

    return 0;
}
