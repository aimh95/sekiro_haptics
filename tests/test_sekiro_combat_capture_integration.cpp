// SEK-PROBE-001E Unit 1 live-acceptance prep: integration-level tests that
// exercise SekiroCombatSessionController/SekiroCombatCommandProcessor the
// way apps/sekiro_signal_probe/main.cpp's real threads actually drive them
// (a dedicated sampler thread ticking independently of command processing,
// input timestamps captured well before the command that carries them gets
// processed, and a marker arriving after combat-stop) -- main.cpp itself
// stays untested (real Win32 threads/hotkeys/controller), but every piece
// of logic it depends on is exercised here directly.

#include "sekiro_haptics/process/SekiroCombatCommandProcessor.hpp"
#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessReader.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace sekiro_haptics::process;

namespace {

constexpr std::uintptr_t kCustomAddr = 0x500000000ULL;

/// A CustomAddress-scoped capture never resolves anything (no AOB, no
/// GameDataMan/PlayerGameData chain), so the SekiroRawCombatReader/identity
/// plumbing SekiroCombatSessionController's constructor requires is never
/// actually exercised by these tests -- it just needs to exist.
SekiroRawCombatReader MakeUnusedRawReader(FakeProcessReader& reader, FakeProcessInspector& inspector) {
    return SekiroRawCombatReader(reader, inspector, KnownRootSpec{}, 0, ExecutableIdentity{}, ExecutableIdentity{});
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

} // namespace

SH_TEST(SekiroCombatSessionController_DuplicateStartPreservesActiveCapture) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    auto raw = MakeUnusedRawReader(reader, inspector);
    SekiroCombatSessionController controller(raw, inspector, reader);
    auto path = std::filesystem::temp_directory_path() / "sh_duplicate_start_v3.jsonl";
    CombatCaptureConfig config;
    config.scope = CombatCaptureScope::CustomAddress;
    config.customBaseAddress = kCustomAddr;
    config.requestedWindowSizeBytes = 4;
    SH_CHECK(controller.StartCapture(config, path.string(), 0) == CombatCaptureStartResult::Started);
    SH_CHECK(controller.CaptureMark("first_record", 100, 100));
    SH_CHECK(controller.StartCapture(config, path.string(), 200) == CombatCaptureStartResult::AlreadyRunning);
    SH_CHECK(controller.IsCapturing());
    controller.StopCapture();
    SH_CHECK(CountOccurrences(ReadWholeFile(path), "first_record") == 1);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCommandProcessor_RestartUsesNewPathAndPreservesFirstCapture) {
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    auto raw = MakeUnusedRawReader(reader, inspector);
    SekiroCombatSessionController controller(raw, inspector, reader);
    auto path = std::filesystem::temp_directory_path() / "sh_restart_v3.jsonl";
    auto sibling = path.parent_path() / "sh_restart_v3_1.jsonl";
    SekiroCombatCommandProcessor processor(controller, path.string());
    const std::string command = "combat-capture custom-address 0x500000000 4 5";
    processor.Process(command, 0, 0);
    SH_CHECK(controller.IsCapturing());
    processor.Process("combat-mark keep", 100, 100);
    processor.Process("combat-stop", 200, 200);
    const auto first = ReadWholeFile(path);
    processor.Process(command, 1000, 1000);
    SH_CHECK(controller.IsCapturing());
    processor.Process("combat-stop", 1100, 1100);
    SH_CHECK(ReadWholeFile(path) == first);
    SH_CHECK(std::filesystem::exists(sibling));
    std::filesystem::remove(path);
    std::filesystem::remove(sibling);
}

SH_TEST(SekiroCombatSessionController_CaptureTick_ProgressesDespiteConcurrentControllerCallsFromAnotherThread) {
    // Simulates main.cpp's design: a dedicated sampler thread ticks the
    // capture independently while a separate thread (standing in for heavy
    // command-queue processing -- combat-mark/status calls arriving back to
    // back) continuously contends for the same controller's mutex. Every
    // one of 200 deterministic ticks must land -- if the mutex serialization
    // were broken (a tick silently lost under contention, or a deadlock),
    // samplesTaken would come out short or the test would hang.
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(64, 0x00);
    reader.PokeBytes(kCustomAddr, data.data(), data.size());
    FakeProcessInspector inspector;
    SekiroRawCombatReader rawReader = MakeUnusedRawReader(reader, inspector);
    SekiroCombatSessionController controller(rawReader, inspector, reader);

    CombatCaptureConfig config;
    config.scope = CombatCaptureScope::CustomAddress;
    config.customBaseAddress = kCustomAddr;
    config.requestedWindowSizeBytes = 64;
    config.samplingInterval = std::chrono::milliseconds(5);
    auto path = std::filesystem::temp_directory_path() / "sh_controller_concurrency.jsonl";
    SH_CHECK(controller.StartCapture(config, path.string(), 0) == CombatCaptureStartResult::Started);

    std::atomic<bool> stop{false};
    std::thread contender([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            controller.CaptureMark("noise", 0, 0);
            controller.IsCapturing();
        }
    });

    constexpr int kTicks = 200;
    constexpr std::int64_t kIntervalUs = 5000;
    for (int i = 1; i <= kTicks; ++i) {
        SH_CHECK(controller.CaptureTick(static_cast<std::int64_t>(i) * kIntervalUs));
    }

    stop.store(true, std::memory_order_relaxed);
    contender.join();

    CombatCaptureStats stats = controller.CaptureStats();
    controller.StopCapture();
    SH_CHECK(stats.samplesTaken == static_cast<std::uint64_t>(kTicks));
    SH_CHECK(stats.missedScheduleSamples == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCommandProcessor_CombatMark_ProcessingLag_MarkerTimestampUsesInputNotProcessedTime) {
    // Mirrors main.cpp's CommandQueue: a hotkey press timestamps itself at
    // the moment it happened (inputTimestampUs), but the command may not
    // reach Process() until much later if the queue was backed up
    // (processedTimestampUs). The persisted marker record must anchor on
    // the former, never drift to the latter.
    FakeProcessReader reader;
    reader.PokeBytes(kCustomAddr, "\x00\x00\x00\x00", 4);
    FakeProcessInspector inspector;
    SekiroRawCombatReader rawReader = MakeUnusedRawReader(reader, inspector);
    SekiroCombatSessionController controller(rawReader, inspector, reader);
    auto path = std::filesystem::temp_directory_path() / "sh_marker_lag_capture.jsonl";
    SekiroCombatCommandProcessor processor(controller, path.string());

    std::ostringstream startCmd;
    startCmd << "combat-capture custom-address 0x" << std::hex << kCustomAddr << std::dec << " 64 5";
    SH_CHECK(processor.Process(startCmd.str(), 0, 0).handled);

    constexpr std::int64_t kInputTimestampUs = 1000; // the real moment F5 was pressed
    constexpr std::int64_t kProcessedTimestampUs = 500000; // 499ms of queue backlog before Process() ran
    auto markResult = processor.Process("combat-mark perfect_deflect", kInputTimestampUs, kProcessedTimestampUs);
    SH_CHECK(markResult.handled);

    SH_CHECK(processor.Process("combat-stop", kProcessedTimestampUs + 100, kProcessedTimestampUs + 100).handled);

    std::string content = ReadWholeFile(path);
    SH_CHECK(content.find("\"timestampUs\":1000,\"processedTimestampUs\":500000") != std::string::npos);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCommandProcessor_CombatMark_AfterCombatStop_RejectedAndNotWrittenStale) {
    // A marker command that was already sitting in the queue when
    // combat-stop ran must not append a stale record once it finally gets
    // processed -- Mark() must reject it (the session is no longer
    // running) and the file's content must be exactly what combat-stop left
    // it as.
    FakeProcessReader reader;
    reader.PokeBytes(kCustomAddr, "\x00\x00\x00\x00", 4);
    FakeProcessInspector inspector;
    SekiroRawCombatReader rawReader = MakeUnusedRawReader(reader, inspector);
    SekiroCombatSessionController controller(rawReader, inspector, reader);
    auto path = std::filesystem::temp_directory_path() / "sh_marker_after_stop.jsonl";
    SekiroCombatCommandProcessor processor(controller, path.string());

    std::ostringstream startCmd;
    startCmd << "combat-capture custom-address 0x" << std::hex << kCustomAddr << std::dec << " 64 5";
    SH_CHECK(processor.Process(startCmd.str(), 0, 0).handled);
    SH_CHECK(processor.Process("combat-stop", 1000, 1000).handled);

    std::string contentAfterStop = ReadWholeFile(path);
    SH_CHECK(CountOccurrences(contentAfterStop, "\"recordKind\":\"marker\"") == 0);

    // The stale marker: its input timestamp (500) predates combat-stop
    // (1000), simulating a command that was already queued when stop ran.
    auto lateResult = processor.Process("combat-mark perfect_deflect", 500, 2000);
    SH_CHECK(lateResult.handled);
    SH_CHECK(!lateResult.outputLines.empty());
    SH_CHECK(lateResult.outputLines.front().find("failed") != std::string::npos);

    std::string contentAfterLateMark = ReadWholeFile(path);
    SH_CHECK(contentAfterLateMark == contentAfterStop); // byte-for-byte unchanged -- nothing appended
    SH_CHECK(CountOccurrences(contentAfterLateMark, "\"recordKind\":\"marker\"") == 0);
    std::filesystem::remove(path);
}
