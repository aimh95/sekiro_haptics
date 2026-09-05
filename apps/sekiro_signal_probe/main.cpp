// SEK-PROBE-001A: developer-only, read-only live signal-discovery probe.
//
// This is NOT the production haptics runtime -- it is a standalone tool
// for finding raw memory-value *candidates* that change alongside user
// actions (HP, posture, animation state, ...) while a human plays. It
// never judges a game event, never writes to the target process, and
// never invents/hardcodes a real address -- see docs/05-process-access.md
// for the full safety boundary this tool operates inside.
//
// Usage:
//   sekiro_haptics_signal_probe --pid <pid> --output <dir>
//   sekiro_haptics_signal_probe --process-name sekiro.exe --output <dir>
//
// Then, interactively:
//   identity
//   regions
//   plan <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>
//   begin <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>
//   begin-disk <u8|u16|u32|i32|f32> <main-module|private-readable|all-readable>
//   filter changed|unchanged|increased|decreased
//   filter exact <value>
//   resume
//   status
//   list <count>
//   watch <address> <u8|u16|u32|i32|f32> <signal-name>
//   mark <label>
//   stop
//   quit
//
//   combat-plan                 -- SEK-PROBE-001D targeted path: AOB scan
//                                   range/expected bytes-per-sample, always
//                                   fullScanUsed=false (never a full
//                                   private-readable scan)
//   combat-resolve               -- AOB-scan + resolve GameDataMan ->
//                                    PlayerGameData once (not per sample)
//   combat-status                -- one HP/Posture snapshot read + validate
//                                    against the currently resolved address
//
// plan/begin/begin-disk/filter/resume/status(scan fields)/list are handled
// by SignalProbeScanController + SignalProbeCommandProcessor (portable,
// Fake-testable on any OS -- see SignalProbeScanController.hpp).
// combat-plan/combat-resolve/combat-status are handled by
// SekiroRawCombatReader + SekiroCombatSessionController +
// SekiroCombatCommandProcessor (same portability -- see
// SekiroKnownRootResolver.hpp/SekiroRawCombatReader.hpp). This file stays a
// thin Win32 adapter: real process attach, real identity lookup, and
// stdin/stdout wiring only -- see docs/06-signal-discovery-probe.md.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/DiscoverySession.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"
#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"
#include "sekiro_haptics/process/SignalProbeCommandProcessor.hpp"
#include "sekiro_haptics/process/SignalProbeScanController.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

constexpr const char* kToolVersion = "sekiro_signal_probe/0.1 (SEK-PROBE-001A)";
constexpr std::int64_t kDefaultSamplingIntervalMs = 200;

// ---------------------------------------------------------------------
// SEK-PROBE-001D: targeted combat-signal reader wiring. The GameDataMan AOB
// pattern/offsets below are the ticket's own hypothesis, not yet
// cross-checked against a real attach -- SekiroKnownRootResolver's identity
// gating (see MakeKnownGoodSekiroIdentity()) refuses to even attempt the
// scan against any build other than the one this was authored against, so
// an unrecognized build fails closed (UnsupportedBuild) rather than
// guessing at an RVA.
// ---------------------------------------------------------------------

Sha256Digest ParseSha256HexLiteral(const char* hex) {
    Sha256Digest digest;
    auto nibble = [](char c) -> int { return (c <= '9') ? (c - '0') : (c - 'a' + 10); };
    for (std::size_t i = 0; i < 32; ++i) {
        digest.bytes[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
    return digest;
}

// Fingerprint of the sekiro.exe build this project actually measured live
// (fileSizeBytes + sha256 -- matches this tool's own "identity" command
// output), not the ticket's v1.06 RVA hypothesis and not a guess. If the
// attached process's own identity doesn't match this exactly, every
// combat-resolve attempt fails closed with UnsupportedBuild and never scans.
ExecutableIdentity MakeKnownGoodSekiroIdentity() {
    ExecutableIdentity id;
    id.fileSizeBytes = 68005144;
    id.sha256 = ParseSha256HexLiteral("637aca527538c0ec6e1f136c8ed66046e95dfbdbb1f51926e134d9916398b856");
    return id;
}

KnownRootSpec MakeGameDataManSpec(const std::string& moduleName) {
    KnownRootSpec spec;
    spec.rootId = "GameDataMan";
    spec.moduleName = moduleName;
    AobScanResult parseResult =
        ParseAobPattern("48 8B 05 ?? ?? ?? ?? 32 D2 48 8B 48 08 48 85 C9 74 13 80 B9 BA", spec.pattern);
    (void)parseResult; // well-formed by construction -- this literal is authored, not user input
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    return spec;
}

// GameDataMan + 0x8 -> PlayerGameData* (the ticket's own hypothesis).
constexpr std::int64_t kPlayerGameDataOffsetFromGameDataMan = 0x8;

// ---------------------------------------------------------------------
// A background thread reads stdin lines into this queue so the main loop
// can interleave "is there a new command?" with periodic watch sampling
// without blocking on either. This is the only place real wall-clock
// sleep_for()/condition_variable waiting happens in this tool.
// ---------------------------------------------------------------------
class CommandQueue {
public:
    void Push(std::string command) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(command));
        }
        cv_.notify_one();
    }

    void MarkStdinClosed() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stdinClosed_ = true;
        }
        cv_.notify_one();
    }

    // Blocks up to `timeout` for a command. Returns false on timeout (no
    // command available) -- the caller distinguishes that from stdin
    // having closed via StdinClosed().
    bool WaitPop(std::string& outCommand, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return !queue_.empty() || stdinClosed_; });
        if (queue_.empty()) {
            return false;
        }
        outCommand = queue_.front();
        queue_.pop_front();
        return true;
    }

    bool StdinClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stdinClosed_ && queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    bool stdinClosed_ = false;
};

volatile std::sig_atomic_t g_ctrlCRequested = 0;

void OnSigInt(int) {
    g_ctrlCRequested = 1;
}

struct WatchTarget {
    std::uintptr_t address = 0;
    CandidateValueType type = CandidateValueType::U32;
    std::string signalName;
};

std::int64_t MonotonicNowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string WallClockNowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tmValue{};
    gmtime_s(&tmValue, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmValue, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::optional<std::uintptr_t> ParseAddress(const std::string& text) {
    try {
        std::size_t consumed = 0;
        unsigned long long value = std::stoull(text, &consumed, 0); // base 0: honors an optional "0x" prefix
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::uintptr_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

struct CliOptions {
    std::optional<std::uint32_t> pid;
    std::optional<std::string> processName;
    std::string outputDir;
    std::size_t maxCandidates = kDefaultMaxCandidates;
    std::uint64_t maxScanBytes = kDefaultMaxScanBytes;
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

        if (arg == "--pid") {
            std::string value;
            if (!takeValue("--pid", value)) return false;
            outOptions.pid = static_cast<std::uint32_t>(std::stoul(value));
        } else if (arg == "--process-name") {
            std::string value;
            if (!takeValue("--process-name", value)) return false;
            outOptions.processName = value;
        } else if (arg == "--output") {
            if (!takeValue("--output", outOptions.outputDir)) return false;
        } else if (arg == "--max-candidates") {
            std::string value;
            if (!takeValue("--max-candidates", value)) return false;
            outOptions.maxCandidates = static_cast<std::size_t>(std::stoull(value));
        } else if (arg == "--max-scan-bytes") {
            std::string value;
            if (!takeValue("--max-scan-bytes", value)) return false;
            outOptions.maxScanBytes = static_cast<std::uint64_t>(std::stoull(value));
        } else {
            outError = "unknown argument: " + arg;
            return false;
        }
    }

    if (!outOptions.pid.has_value() && !outOptions.processName.has_value()) {
        outError = "one of --pid or --process-name is required";
        return false;
    }
    if (outOptions.outputDir.empty()) {
        outError = "--output <local-session-directory> is required";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions options;
    std::string parseError;
    if (!ParseArgs(argc, argv, options, parseError)) {
        std::cerr << parseError << "\n";
        std::cerr << "usage: sekiro_haptics_signal_probe (--pid <pid> | --process-name <name>) --output <dir>\n"
                      "                                    [--max-candidates <N>] [--max-scan-bytes <N>]\n";
        return 1;
    }

    std::filesystem::create_directories(options.outputDir);

    Win32ProcessReader reader;
    ProcessReaderResult attachResult = options.pid.has_value() ? reader.AttachByPid(*options.pid)
                                                                : reader.AttachByName(*options.processName);
    if (attachResult != ProcessReaderResult::Success) {
        std::cerr << "attach failed: " << ToString(attachResult) << "\n";
        std::cerr << "STOP: attach permission/ambiguity problem -- not proceeding.\n";
        return 1;
    }
    std::cout << "[probe] attached to pid " << reader.Pid() << "\n";

    ExecutableIdentity identity;
    ProcessInspectionResult identityResult = BuildExecutableIdentity(reader, identity);
    if (identityResult != ProcessInspectionResult::Success) {
        std::cerr << "STOP: could not build executable identity: " << ToString(identityResult) << "\n";
        std::cerr << "Not proceeding without a real build identity.\n";
        return 1;
    }

    ModuleInfo mainModule;
    ProcessInspectionResult mainModuleResult = reader.GetMainModule(mainModule);
    if (mainModuleResult != ProcessInspectionResult::Success) {
        std::cerr << "STOP: could not identify main module: " << ToString(mainModuleResult) << "\n";
        return 1;
    }

    DiscoverySessionMetadata metadata;
    metadata.processImagePath = identity.executablePath.string();
    metadata.executableFileSizeBytes = identity.fileSizeBytes;
    metadata.sha256Hex = ToHex(identity.sha256);
    metadata.mainModuleName = mainModule.name;
    metadata.mainModuleBaseAddress = mainModule.baseAddress;
    metadata.mainModuleImageSize = mainModule.imageSize;
    metadata.pid = reader.Pid();
    metadata.sessionStartWallClock = WallClockNowIso8601();
    metadata.samplingIntervalMs = kDefaultSamplingIntervalMs;
    metadata.toolVersion = kToolVersion;
    metadata.endedNormally = false;
    metadata.endReason = "not_yet_ended";

    std::filesystem::path metadataPath = std::filesystem::path(options.outputDir) / "session.json";
    if (!WriteDiscoverySessionMetadata(metadataPath.string(), metadata)) {
        std::cerr << "warning: could not write session metadata to " << metadataPath.string() << "\n";
    }

    std::cout << "[probe] build identity: fileSizeBytes=" << identity.fileSizeBytes
              << " sha256=" << metadata.sha256Hex << "\n";
    std::cout << "[probe] main module: " << mainModule.name << " base=0x" << std::hex << mainModule.baseAddress
              << " size=0x" << mainModule.imageSize << std::dec << "\n";
    std::cout << "[probe] session metadata written to " << metadataPath.string() << "\n";
    std::cout << "[probe] this tool never writes to the target process -- read-only for the whole session.\n";
    std::cout << "[probe] in-memory secondary caps: --max-candidates=" << options.maxCandidates
               << " --max-scan-bytes=" << options.maxScanBytes
               << " -- if \"begin\" is refused or fails, use \"plan\" then \"begin-disk\" instead of raising these.\n";

    SignalProbeControllerConfig controllerConfig;
    controllerConfig.outputDir = options.outputDir;
    controllerConfig.maxCandidates = options.maxCandidates;
    controllerConfig.maxScanBytes = options.maxScanBytes;

    ScanControllerIdentity controllerIdentity;
    controllerIdentity.executableFileSizeBytes = identity.fileSizeBytes;
    controllerIdentity.sha256Hex = metadata.sha256Hex;
    controllerIdentity.pid = reader.Pid();
    controllerIdentity.mainModuleBaseAddress = mainModule.baseAddress;
    controllerIdentity.mainModuleImageSize = mainModule.imageSize;

    SignalProbeScanController controller(reader, reader, reader, controllerConfig, controllerIdentity);
    SignalProbeCommandProcessor processor(controller);

    // SEK-PROBE-001D: targeted combat-signal reader -- independent of the
    // full-scan controller above, never touches private-readable memory.
    SekiroRawCombatReader combatReader(reader, reader, MakeGameDataManSpec(mainModule.name),
                                        kPlayerGameDataOffsetFromGameDataMan, MakeKnownGoodSekiroIdentity(), identity);
    SekiroCombatSessionController combatController(combatReader, reader);
    SekiroCombatCommandProcessor combatProcessor(combatController);

    std::vector<WatchTarget> watches;
    std::optional<DiscoveryWatchWriter> watchWriter;
    std::filesystem::path watchPath = std::filesystem::path(options.outputDir) / "watch.jsonl";
    std::int64_t sessionStartUs = MonotonicNowUs();

    auto ensureWatchWriter = [&]() -> bool {
        if (watchWriter.has_value()) {
            return watchWriter->IsOpen();
        }
        watchWriter.emplace(watchPath.string());
        return watchWriter->IsOpen();
    };

    std::signal(SIGINT, OnSigInt);

    CommandQueue commandQueue;
    std::thread readerThread([&commandQueue] {
        std::string line;
        while (std::getline(std::cin, line)) {
            commandQueue.Push(line);
        }
        commandQueue.MarkStdinClosed();
    });
    readerThread.detach();

    std::cout << "[probe] ready. Type a command (e.g. \"identity\", \"regions\", \"begin u32 main-module\").\n";

    bool endedNormally = false;
    std::string endReason = "quit";

    while (true) {
        if (g_ctrlCRequested != 0) {
            std::cout << "\n[probe] Ctrl+C received -- shutting down cleanly.\n";
            endedNormally = false;
            endReason = "ctrl_c";
            break;
        }
        if (!reader.IsAlive()) {
            std::cout << "[probe] target process exited.\n";
            endedNormally = false;
            endReason = "process_exited";
            break;
        }

        std::chrono::milliseconds waitTimeout = watches.empty() ? std::chrono::milliseconds(250)
                                                                  : std::chrono::milliseconds(kDefaultSamplingIntervalMs);
        std::string command;
        bool gotCommand = commandQueue.WaitPop(command, waitTimeout);

        if (!gotCommand) {
            if (commandQueue.StdinClosed() && watches.empty()) {
                endedNormally = true;
                endReason = "stdin_closed";
                break;
            }
            if (!watches.empty()) {
                if (!ensureWatchWriter()) {
                    std::cerr << "[probe] could not open watch file -- stopping watches.\n";
                    watches.clear();
                    continue;
                }
                std::int64_t t = MonotonicNowUs() - sessionStartUs;
                for (const WatchTarget& w : watches) {
                    std::size_t size = CandidateValueTypeSize(w.type);
                    std::uint8_t buffer[4] = {};
                    ProcessReaderResult readResult = reader.ReadBytes(w.address, buffer, size);
                    if (readResult != ProcessReaderResult::Success) {
                        continue; // this address's sample is skipped this tick, not fabricated
                    }
                    CandidateValue value = DecodeCandidateValue(w.type, buffer);
                    watchWriter->WriteSample(t, w.signalName, w.address, w.type, value);
                }
            }
            continue;
        }

        std::istringstream iss(command);
        std::string verb;
        iss >> verb;

        if (verb.empty()) {
            continue;
        }

        // begin-disk/filter can legitimately take minutes against a
        // multi-gigabyte real process -- without this, the CLI shows
        // nothing at all until the whole command finishes, which reads as
        // a hang. Throttled to once per second so it doesn't flood the
        // terminal; every other verb ignores this callback entirely.
        std::optional<std::chrono::steady_clock::time_point> lastProgressPrint;
        auto onProgress = [&](const DiskScanStats& stats) {
            auto now = std::chrono::steady_clock::now();
            if (lastProgressPrint.has_value() && now - *lastProgressPrint < std::chrono::seconds(1)) {
                return;
            }
            lastProgressPrint = now;
            std::cout << "[probe] progress: ";
            if (stats.regionsTotal > 0) {
                std::cout << "regions=" << stats.regionsProcessed << "/" << stats.regionsTotal << " ";
            }
            std::cout << "coverage=" << std::fixed << std::setprecision(1) << stats.coveragePercent << "% "
                       << "candidates=" << stats.survivingCandidateCount << " "
                       << "processedBytes=" << stats.processedBytes << "\n";
            std::cout.flush();
        };

        SignalProbeCommandProcessor::ProcessResult processorResult = processor.Process(command, onProgress);
        if (processorResult.handled) {
            for (const std::string& line : processorResult.outputLines) {
                std::cout << line << "\n";
            }
            if (verb == "status") {
                std::cout << "watches=" << watches.size() << "\n";
            }
            if (controller.Mode() != ScanMode::None) {
                StatusSnapshot snapshot = controller.Status();
                if (controller.CurrentValueType().has_value()) {
                    metadata.selectedValueType = ToString(*controller.CurrentValueType());
                }
                if (snapshot.mode == ScanMode::InMemory && snapshot.inMemoryScope.has_value()) {
                    metadata.selectedScope = ToString(*snapshot.inMemoryScope);
                } else if (snapshot.mode == ScanMode::Disk && snapshot.diskManifest.has_value()) {
                    metadata.selectedScope = snapshot.diskManifest->identity.scope;
                }
            }
            continue;
        }

        SekiroCombatCommandProcessor::ProcessResult combatResult = combatProcessor.Process(command);
        if (combatResult.handled) {
            for (const std::string& line : combatResult.outputLines) {
                std::cout << line << "\n";
            }
            continue;
        }

        if (verb == "quit") {
            endedNormally = true;
            endReason = "quit";
            break;
        } else if (verb == "identity") {
            std::cout << "path=" << identity.executablePath.string() << "\n";
            std::cout << "fileSizeBytes=" << identity.fileSizeBytes << "\n";
            std::cout << "sha256=" << metadata.sha256Hex << "\n";
            std::cout << "mainModule=" << mainModule.name << " base=0x" << std::hex << mainModule.baseAddress
                       << " size=0x" << mainModule.imageSize << std::dec << "\n";
        } else if (verb == "regions") {
            std::vector<ProcessMemoryRegion> allRegions;
            MemoryMapResult mapResult = reader.EnumerateReadableRegions(allRegions);
            if (mapResult != MemoryMapResult::Success) {
                std::cout << "region enumeration failed: " << ToString(mapResult) << "\n";
                continue;
            }
            std::size_t imageCount = 0, mappedCount = 0, privateCount = 0;
            std::uint64_t imageBytes = 0, mappedBytes = 0, privateBytes = 0;
            for (const ProcessMemoryRegion& r : allRegions) {
                if (r.kind == MemoryRegionKind::Image) { ++imageCount; imageBytes += r.sizeBytes; }
                else if (r.kind == MemoryRegionKind::Mapped) { ++mappedCount; mappedBytes += r.sizeBytes; }
                else { ++privateCount; privateBytes += r.sizeBytes; }
            }
            std::cout << "all-readable: " << allRegions.size() << " region(s), "
                       << (imageBytes + mappedBytes + privateBytes) << " byte(s)\n";
            std::cout << "  image-kind: " << imageCount << " region(s), " << imageBytes << " byte(s)\n";
            std::cout << "  mapped-kind: " << mappedCount << " region(s), " << mappedBytes << " byte(s)\n";
            std::cout << "private-readable: " << privateCount << " region(s), " << privateBytes << " byte(s)\n";
        } else if (verb == "watch") {
            std::string addrText, typeText, nameText;
            iss >> addrText >> typeText;
            std::getline(iss, nameText);
            while (!nameText.empty() && nameText.front() == ' ') nameText.erase(nameText.begin());
            auto address = ParseAddress(addrText);
            CandidateValueType parsedType;
            std::optional<CandidateValueType> type =
                ParseCandidateValueType(typeText, parsedType) ? std::optional<CandidateValueType>(parsedType) : std::nullopt;
            if (!address.has_value() || !type.has_value() || nameText.empty()) {
                std::cout << "usage: watch <address> <u8|u16|u32|i32|f32> <signal-name>\n";
                continue;
            }
            if (!ensureWatchWriter()) {
                std::cout << "could not open watch file at " << watchPath.string() << "\n";
                continue;
            }
            watches.push_back(WatchTarget{*address, *type, nameText});
            std::cout << "watching 0x" << std::hex << *address << std::dec << " as \"" << nameText << "\"\n";
        } else if (verb == "mark") {
            std::string label;
            std::getline(iss, label);
            while (!label.empty() && label.front() == ' ') label.erase(label.begin());
            if (label.empty()) {
                std::cout << "usage: mark <label>\n";
                continue;
            }
            if (!ensureWatchWriter()) {
                std::cout << "could not open watch file at " << watchPath.string() << "\n";
                continue;
            }
            watchWriter->WriteMarker(MonotonicNowUs() - sessionStartUs, label);
            std::cout << "marked: " << label << "\n";
        } else if (verb == "stop") {
            watches.clear();
            std::cout << "all watches stopped\n";
        } else {
            std::cout << "unknown command: " << verb << "\n";
        }
    }

    if (watchWriter.has_value()) {
        watchWriter->Close();
    }
    reader.Detach();

    metadata.endedNormally = endedNormally;
    metadata.endReason = endReason;
    WriteDiscoverySessionMetadata(metadataPath.string(), metadata);

    std::cout << "[probe] session ended (" << endReason << "). Metadata: " << metadataPath.string() << "\n";
    return 0;
}
