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
//   (add --guard-debounce-ms <n> to override the 120ms default debounce
//   applied to hotkey/controller-guard inputs -- see MonotonicDebounce.hpp)
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
//   combat-capture player-game-data [window-size-bytes] [interval-ms]
//                                 -- starts a bounded raw schema-v3 capture
//                                    (never the full raw block) around the
//                                    currently resolved PlayerGameData
//   combat-capture custom-address <hex-address> [window-size-bytes] [interval-ms]
//                                 -- same, but starting at any runtime
//                                    address the caller already has evidence
//                                    for (e.g. found via begin-disk/filter),
//                                    up to 64KiB forward
//   combat-mark <label>          -- e.g. normal_block/perfect_deflect/
//                                    take_damage/death/... (see docs/07-...)
//   combat-stop                  -- stops the active combat-capture
//   combat-analyze [window-ms] [guard-input-lookback-ms]
//                                 -- offline offset/marker correlation over
//                                    the last capture (not a validated
//                                    signal -- see SekiroCombatCaptureAnalyzer.hpp);
//                                    lookback defaults to 1000ms
//   combat-export                -- reports the capture file path + stats
//   target-resolve               -- EXPLORATORY: resolves WorldChrMan's own
//                                    live address only (AOB+RIP-relative+1
//                                    deref, same mechanism as combat-resolve)
//                                    -- no offset hypothesis exists yet for
//                                    reaching a current-target/enemy object
//                                    from here; not part of the tested
//                                    combat-* command family above
//
// Global hotkeys (work even while the game window has focus -- see
// RunMarkerHotkeyThread() below) inject the exact command text as if typed,
// so both combat markers and the candidate-scan funnel below can be driven
// without alt-tabbing. Plain keys, no modifier -- Sekiro itself uses Ctrl
// for combat arts, so Ctrl+Alt combos would fight with real gameplay input:
//   F1  -> combat-mark idle                   F7  -> combat-mark high_posture_no_break
//   F2  -> combat-mark guard_only              F8  -> combat-mark player_posture_break
//   F3  -> combat-mark attack_miss             F9  -> combat-mark target_posture_break
//   F4  -> combat-mark normal_block            F10 -> combat-mark death
//   F5  -> combat-mark perfect_deflect         F11 -> combat-mark respawn
//   F6  -> combat-mark take_damage
//   PageUp -> filter decreased   Insert -> filter increased   Delete -> filter unchanged
//   Home -> list 10              End -> status
// (not F12 -- Steam's overlay already binds that globally for screenshots)
// "loading"/"rest" have no hotkey (not time-critical -- type "combat-mark
// loading"/"combat-mark rest" normally during downtime).
//
// plan/begin/begin-disk/filter/resume/status(scan fields)/list are handled
// by SignalProbeScanController + SignalProbeCommandProcessor (portable,
// Fake-testable on any OS -- see SignalProbeScanController.hpp).
// combat-plan/combat-resolve/combat-status/combat-capture/combat-mark/
// combat-stop/combat-analyze/combat-export are handled by
// SekiroRawCombatReader + SekiroCombatCaptureSession +
// SekiroCombatCaptureAnalyzer + SekiroCombatSessionController +
// SekiroCombatCommandProcessor (same portability -- see
// SekiroKnownRootResolver.hpp/SekiroRawCombatReader.hpp/
// SekiroCombatCaptureSession.hpp). This file stays a
// thin Win32 adapter: real process attach, real identity lookup, and
// stdin/stdout wiring only -- see docs/06-signal-discovery-probe.md.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/DiscoverySession.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/MonotonicDebounce.hpp"
#include "sekiro_haptics/process/RisingEdgeDetector.hpp"
#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"
#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"
#include "sekiro_haptics/process/SignalProbeCommandProcessor.hpp"
#include "sekiro_haptics/process/SignalProbeScanController.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"

#include <windows.h>
#include <xinput.h>
#if !defined(_MSC_VER)
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
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
#include <stop_token>
#include <unordered_map>
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

// WorldChrMan candidate root -- exploratory only (see the "target-resolve"
// command below). No offset hypothesis exists yet for reaching the current
// target/enemy object from here; this only resolves WorldChrMan's own live
// address as a starting point for further manual investigation (e.g. a
// bounded scan of the memory immediately around it). Kept separate from
// the tested SekiroCombatCommandProcessor/Controller pair -- this is
// throwaway exploration code, not yet a validated building block.
KnownRootSpec MakeWorldChrManSpec(const std::string& moduleName) {
    KnownRootSpec spec;
    spec.rootId = "WorldChrMan";
    spec.moduleName = moduleName;
    AobScanResult parseResult = ParseAobPattern("48 8B 35 ?? ?? ?? ?? 44 0F 28 18", spec.pattern);
    (void)parseResult; // well-formed by construction -- this literal is authored, not user input
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    return spec;
}

// ---------------------------------------------------------------------
// A background thread reads stdin lines into this queue so the main loop
// can interleave "is there a new command?" with periodic watch sampling
// without blocking on either. This is the only place real wall-clock
// sleep_for()/condition_variable waiting happens in this tool.
// ---------------------------------------------------------------------
struct QueuedCommand {
    std::string text;
    /// The precise moment this command's underlying input happened
    /// (captured at the source -- stdin readline return, hotkey WM_HOTKEY
    /// receipt, or controller button-state transition), never the moment
    /// the main loop gets around to dequeuing it (SEK-PROBE-001E Section 7:
    /// "command processor에서 처리된 시각을 marker 시각으로 사용하지 않음").
    std::int64_t inputTimestampUs = 0;
};

class CommandQueue {
public:
    void Push(std::string command, std::int64_t inputTimestampUs) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(QueuedCommand{std::move(command), inputTimestampUs});
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
    bool WaitPop(QueuedCommand& outCommand, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return !queue_.empty() || stdinClosed_; });
        if (queue_.empty()) {
            return false;
        }
        outCommand = std::move(queue_.front());
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
    std::deque<QueuedCommand> queue_;
    bool stdinClosed_ = false;
};

std::int64_t MonotonicNowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Own stdin's blocking read. A stop flag alone cannot wake getline();
// cancel this tool's own reader-thread I/O until it acknowledges shutdown.
// The target game's threads and handles are never involved.
class StdinWorker {
public:
    // std::jthread::native_handle() is already a Win32 HANDLE under MSVC's
    // STL, but libstdc++ on MinGW hands back a winpthreads `pthread_t`, which
    // is an internal id rather than a thread handle. Translate it there.
    static HANDLE Win32ThreadHandle(std::jthread& thread) {
#if defined(_MSC_VER)
        return static_cast<HANDLE>(thread.native_handle());
#else
        return static_cast<HANDLE>(pthread_gethandle(thread.native_handle()));
#endif
    }

    explicit StdinWorker(CommandQueue& queue) : worker_([this, &queue](std::stop_token stop) {
        try {
            std::string line;
            while (!stop.stop_requested() && std::getline(std::cin, line)) {
                if (stop.stop_requested()) break;
                queue.Push(line, MonotonicNowUs());
            }
        } catch (...) {
            // End-of-input/error is reported through the same queue state.
        }
        queue.MarkStdinClosed();
        done_.store(true, std::memory_order_release);
    }) {}

    ~StdinWorker() { StopAndJoin(); }
    void StopAndJoin() {
        if (!worker_.joinable()) return;
        worker_.request_stop();
        while (!done_.load(std::memory_order_acquire)) {
            // Cancellation can race entry into ReadFile/ReadConsole, so
            // repeat it; never detach a worker still referring to the queue.
            CancelSynchronousIo(Win32ThreadHandle(worker_));
            Sleep(5);
        }
        worker_.join();
    }
private:
    std::atomic<bool> done_{false};
    std::jthread worker_;
};

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000 // suppresses OS key-repeat re-triggering while held; older SDKs may lack the macro
#endif

// ---------------------------------------------------------------------
// Global hotkeys: RegisterHotKey() delivers WM_HOTKEY to whichever thread
// registered it via that thread's message queue, regardless of which
// window (if any) currently has focus -- this is what lets the user mark a
// perfect deflect, or narrow a live candidate scan by "filter decreased",
// the instant it happens without alt-tabbing out of the game. Runs on its
// own dedicated thread (RegisterHotKey requires the registering thread to
// pump messages) and pushes the exact command text into the same
// CommandQueue the stdin reader thread feeds, so hotkey-triggered commands
// flow through the exact same, already-timestamped command path -- no
// separate/duplicated logic for "typed" vs "hotkey" input.
// ---------------------------------------------------------------------
struct HotkeyBinding {
    int id;
    UINT virtualKey;
    const char* keyName; // for the startup print / warning messages only
    const char* command; // exact text pushed to the command queue
};

constexpr HotkeyBinding kHotkeys[] = {
    {1, VK_F1, "F1", "combat-mark idle"},
    {2, VK_F2, "F2", "combat-mark guard_only"},
    {3, VK_F3, "F3", "combat-mark attack_miss"},
    {4, VK_F4, "F4", "combat-mark normal_block"},
    {5, VK_F5, "F5", "combat-mark perfect_deflect"},
    {6, VK_F6, "F6", "combat-mark take_damage"},
    {7, VK_F7, "F7", "combat-mark high_posture_no_break"},
    {8, VK_F8, "F8", "combat-mark player_posture_break"},
    {9, VK_F9, "F9", "combat-mark target_posture_break"},
    {10, VK_F10, "F10", "combat-mark death"},
    {11, VK_F11, "F11", "combat-mark respawn"},
    // Candidate-scan funnel (the old private-readable/main-module scan --
    // see "filter"/"list"/"status" above), so narrowing a live scan doesn't
    // require alt-tabbing either. Not F12 -- Steam's overlay already binds
    // that globally for screenshots, which would conflict.
    {12, VK_PRIOR, "PageUp", "filter decreased"},
    {13, VK_INSERT, "Insert", "filter increased"},
    {14, VK_DELETE, "Delete", "filter unchanged"},
    {15, VK_HOME, "Home", "list 10"},
    {16, VK_END, "End", "status"},
};

// Pins this process's own console window on top of the game window, in a
// corner, so the hotkey legend and live output (marks, filter/list/status
// results) stay visible without alt-tabbing. This is a completely separate
// window belonging to *this* process -- it never touches the game window
// or process in any way, and only works if Sekiro is running in windowed
// or borderless-windowed mode (a true exclusive-fullscreen game owns the
// whole screen and would render over this regardless of the topmost flag).
void PinConsoleWindowOnTop() {
    HWND hwnd = GetConsoleWindow();
    if (hwnd == nullptr) {
        return;
    }
    constexpr int kWidth = 560;
    constexpr int kHeight = 420;
    constexpr int kMargin = 20;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int x = screenWidth - kWidth - kMargin;
    int y = kMargin;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, kWidth, kHeight, SWP_SHOWWINDOW);

    // Slightly see-through so it doesn't fully block the game behind it.
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);
}

// ---------------------------------------------------------------------
// Auto-marks the *precise* instant the controller's guard button (LB on an
// Xbox/XInput-compatible pad -- Sekiro's default block/deflect input) is
// pressed, as "combat-mark guard_input". This exists because a human
// judging "that was a perfect deflect" and then pressing F5 lags the real
// event by however long that judgment + reaction takes (easily 200-500ms) --
// far more than the analysis window combat-analyze uses (100-200ms by
// default), so purely human-timed markers can miss the actual delta
// entirely. guard_input's timestamp is precise (bounded by this thread's
// own poll interval, ~8ms); the human's subsequent F4/F5/F6 press still
// supplies the *label* (block/deflect/damage) -- see
// SekiroCombatCaptureAnalyzer.hpp's re-anchoring of a labeled outcome onto
// the nearest preceding guard_input marker.
//
// Pure XInputGetState() polling -- read-only controller state, no hook, no
// injection, nothing sent to the game.
// ---------------------------------------------------------------------
constexpr std::int64_t kDefaultGuardDebounceUs = 120'000; // 120ms -- SEK-PROBE-001E Section 7's 100-150ms range

// Counts inputs the debounce suppressed, purely for end-of-session
// reporting (see main()'s shutdown print) -- never affects control flow.
std::atomic<std::uint64_t> g_guardDebounceSuppressed{0};
std::atomic<std::uint64_t> g_hotkeyDebounceSuppressed{0};

void RunControllerGuardWatchThread(std::stop_token stop, CommandQueue* commandQueue, std::int64_t debounceUs) {
    RisingEdgeDetector edgeDetector;
    MonotonicDebounce debounce(debounceUs);
    bool armed = false;
    while (!stop.stop_requested()) {
        XINPUT_STATE state{};
        DWORD result = XInputGetState(0, &state);
        if (result == ERROR_SUCCESS) {
            bool isPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            const bool rising = edgeDetector.Update(isPressed);
            if (!armed) {
                armed = true; // Do not invent a press when connecting while LB is held.
            } else if (rising) {
                std::int64_t nowUs = MonotonicNowUs();
                commandQueue->Push("combat-mark guard_input", nowUs); // Preserve EVERY raw edge.
                if (!debounce.ShouldFire(nowUs)) {
                    g_guardDebounceSuppressed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            armed = false;
            edgeDetector = RisingEdgeDetector{};
        }
        Sleep(8); // ~120Hz poll -- well under human reaction time, negligible CPU cost
    }
}

void RunMarkerHotkeyThread(std::stop_token stop, CommandQueue* commandQueue, std::int64_t debounceUs) {
    std::vector<int> registeredIds;
    for (const HotkeyBinding& hotkey : kHotkeys) {
        if (RegisterHotKey(nullptr, hotkey.id, MOD_NOREPEAT, hotkey.virtualKey)) {
            registeredIds.push_back(hotkey.id);
        } else {
            std::cerr << "[probe] warning: could not register hotkey \"" << hotkey.keyName
                       << "\" -- another app may already be using it.\n";
        }
    }

    // MOD_NOREPEAT suppresses re-fires while a key is held down, but a
    // human can still double-tap a key faster than they intend to under
    // combat adrenaline -- the debounce below catches that on top of it.
    // One debounce state per hotkey id so pressing F4 doesn't suppress an
    // immediately-following, deliberate F5.
    std::unordered_map<int, MonotonicDebounce> debounceByHotkeyId;
    for (const HotkeyBinding& hotkey : kHotkeys) {
        debounceByHotkeyId.emplace(hotkey.id, MonotonicDebounce(debounceUs));
    }

    MSG msg;
    while (!stop.stop_requested()) {
        if (!PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            continue;
        }
        if (msg.message == WM_QUIT) break;
        if (msg.message == WM_HOTKEY) {
            for (const HotkeyBinding& hotkey : kHotkeys) {
                if (hotkey.id == static_cast<int>(msg.wParam)) {
                    std::int64_t nowUs = MonotonicNowUs();
                    if (debounceByHotkeyId.at(hotkey.id).ShouldFire(nowUs)) {
                        commandQueue->Push(hotkey.command, nowUs);
                    } else {
                        g_hotkeyDebounceSuppressed.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
        }
    }

    for (int id : registeredIds) {
        UnregisterHotKey(nullptr, id);
    }
}

volatile std::sig_atomic_t g_ctrlCRequested = 0;

void OnSigInt(int) {
    g_ctrlCRequested = 1;
}

struct WatchTarget {
    std::uintptr_t address = 0;
    CandidateValueType type = CandidateValueType::U32;
    std::string signalName;
};

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
    /// Debounce applied to both hotkey and controller guard-button inputs
    /// (SEK-PROBE-001E Section 7). Kept in the ticket's 100-150ms range by
    /// default; overridable for live-testing whether a given value swallows
    /// genuine rapid re-presses (see combat-analyze's markersRejected... and
    /// the printed debounce-suppressed count at shutdown).
    std::int64_t guardDebounceMs = kDefaultGuardDebounceUs / 1000;
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
        } else if (arg == "--guard-debounce-ms") {
            std::string value;
            if (!takeValue("--guard-debounce-ms", value)) return false;
            outOptions.guardDebounceMs = std::stoll(value);
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

    std::error_code directoryError;
    std::filesystem::create_directories(options.outputDir, directoryError);
    if (directoryError) {
        std::cerr << "could not create output directory: " << directoryError.message() << "\n";
        return 1;
    }

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

    // A new directory owns this run's identity + captures + watches. Keep
    // disk-scan storage at --output so its existing resume workflow works.
    std::filesystem::path runOutputDir;
    bool createdRun = false;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        runOutputDir = std::filesystem::path(options.outputDir) /
            ("session-" + std::to_string(reader.Pid()) + "-" + std::to_string(MonotonicNowUs()) + "-" +
             std::to_string(attempt));
        createdRun = std::filesystem::create_directory(runOutputDir, directoryError);
        if (createdRun || directoryError) break;
    }
    if (!createdRun) {
        std::cerr << "could not reserve a new session directory: " << directoryError.message() << "\n";
        return 1;
    }
    std::filesystem::path metadataPath = runOutputDir / "session.json";
    if (!WriteDiscoverySessionMetadata(metadataPath.string(), metadata)) {
        std::cerr << "could not write session metadata to " << metadataPath.string() << "\n";
        return 1;
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
    SekiroCombatSessionController combatController(combatReader, reader, reader);
    std::filesystem::path combatCapturePath = runOutputDir / "combat_capture.jsonl";
    SekiroCombatCommandProcessor combatProcessor(combatController, combatCapturePath.string());

    // Exploratory: WorldChrMan resolver, separate from the tested
    // combat-* command path above -- see MakeWorldChrManSpec()'s comment.
    SekiroKnownRootResolver worldChrManResolver(reader, reader, MakeWorldChrManSpec(mainModule.name),
                                                 MakeKnownGoodSekiroIdentity(), identity);

    std::vector<WatchTarget> watches;
    std::optional<DiscoveryWatchWriter> watchWriter;
    std::filesystem::path watchPath = runOutputDir / "watch.jsonl";
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
    StdinWorker stdinWorker(commandQueue);

    std::int64_t guardDebounceUs = options.guardDebounceMs * 1000;

    std::jthread hotkeyThread(RunMarkerHotkeyThread, &commandQueue, guardDebounceUs);

    std::jthread guardWatchThread(RunControllerGuardWatchThread, &commandQueue, guardDebounceUs);
    std::cout << "[probe] controller guard-button watch active -- LB press auto-marks \"guard_input\" with "
                  "host observation timing; all raw edges retained. Debounce is diagnostic only: "
               << options.guardDebounceMs << "ms\n";

    std::jthread captureSamplerThread([&combatController](std::stop_token stop) {
        // Dedicated sampling thread (SEK-PROBE-001E Section 6) -- decoupled
        // from stdin/hotkey/controller command-queue processing so capture
        // cadence is never skewed by however long a command took to handle.
        // CaptureTick()/IsCapturing() are internally mutex-guarded, so this
        // thread never races the main loop's own controller calls.
        while (!stop.stop_requested()) {
            if (combatController.IsCapturing()) {
                if (!combatController.CaptureTick(MonotonicNowUs()) && combatController.CaptureStats().outputFailed) {
                    std::cerr << "[probe] capture stopped: output write failed; file may be incomplete.\n";
                }
            }
            Sleep(1); // finer than any configured capture interval; Tick() itself is a no-op until due
        }
    });

    PinConsoleWindowOnTop();
    std::cout << "[probe] this console window is now pinned on top, top-right corner -- only visible if "
                  "Sekiro is windowed/borderless (not exclusive fullscreen).\n";
    std::cout << "[probe] global hotkeys active (work even while the game window has focus):\n";
    for (const HotkeyBinding& hotkey : kHotkeys) {
        std::cout << "  " << hotkey.keyName << " -> " << hotkey.command << "\n";
    }

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
        QueuedCommand queued;
        bool gotCommand = commandQueue.WaitPop(queued, waitTimeout);

        if (!gotCommand) {
            if (commandQueue.StdinClosed() && watches.empty() && !combatController.IsCapturing()) {
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

        std::istringstream iss(queued.text);
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

        SignalProbeCommandProcessor::ProcessResult processorResult = processor.Process(queued.text, onProgress);
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

        SekiroCombatCommandProcessor::ProcessResult combatResult =
            combatProcessor.Process(queued.text, queued.inputTimestampUs, MonotonicNowUs());
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
        } else if (verb == "target-resolve") {
            // Exploratory only -- see MakeWorldChrManSpec()'s comment.
            ResolvedRoot result = worldChrManResolver.Resolve();
            std::cout << "status=" << ToString(result.result) << "\n";
            if (result.result == RootResolveResult::Resolved) {
                std::cout << "worldChrManAddress=0x" << std::hex << result.objectAddress << std::dec << "\n";
                std::cout << "generation=" << result.generation << "\n";
            }
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
            watchWriter->WriteMarker(queued.inputTimestampUs - sessionStartUs, label);
            std::cout << "marked: " << label << "\n";
        } else if (verb == "stop") {
            watches.clear();
            std::cout << "all watches stopped\n";
        } else {
            std::cout << "unknown command: " << verb << "\n";
        }
    }

    captureSamplerThread.request_stop();
    hotkeyThread.request_stop();
    guardWatchThread.request_stop();
    captureSamplerThread.join();
    hotkeyThread.join();
    guardWatchThread.join();
    stdinWorker.StopAndJoin();
    combatController.StopCapture();
    if (combatController.CaptureStats().outputFailed) {
        endedNormally = false;
        endReason = "capture_output_failed";
    }
    if (watchWriter.has_value()) {
        watchWriter->Close();
    }
    reader.Detach();

    metadata.endedNormally = endedNormally;
    metadata.endReason = endReason;
    if (!WriteDiscoverySessionMetadata(metadataPath.string(), metadata)) {
        std::cerr << "[probe] could not finalize session metadata\n";
        return 1;
    }

    std::cout << "[probe] session ended (" << endReason << "). Metadata: " << metadataPath.string() << "\n";
    std::cout << "[probe] rapid guard edges retained=" << g_guardDebounceSuppressed.load()
               << " hotkey=" << g_hotkeyDebounceSuppressed.load() << "\n";
    return endReason == "capture_output_failed" ? 1 : 0;
}
