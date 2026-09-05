// Unit tests for SEK-PROBE-001D Stage C's SekiroCombatCaptureSession.
// Fake-based, no real process. Every timestamp is caller-supplied (never a
// real sleep_for()), matching the class's own no-internal-clock contract.

#include "sekiro_haptics/process/SekiroCombatCaptureSession.hpp"
#include "testing.hpp"

#include "FakeProcessReader.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path TempCapturePath(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("sh_combat_capture_" + name + ".jsonl");
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

int CountOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

constexpr std::uintptr_t kRegionAddr = 0x140010000;

} // namespace

SH_TEST(SekiroCombatCaptureSession_Start_NotResolved_ReturnsInvalidConfig) {
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;

    CombatCaptureStartResult result = session.Start(config, /*regionBaseAddress=*/0, 1, TempCapturePath("a").string(), 0);
    SH_CHECK(result == CombatCaptureStartResult::InvalidConfig);
    SH_CHECK(!session.IsRunning());
}

SH_TEST(SekiroCombatCaptureSession_Start_InitialReadFails_ReturnsInitialReadFailed) {
    FakeProcessReader reader;
    reader.SetAlive(false); // every read fails
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;

    auto path = TempCapturePath("b");
    CombatCaptureStartResult result = session.Start(config, kRegionAddr, 1, path.string(), 0);
    SH_CHECK(result == CombatCaptureStartResult::InitialReadFailed);
    SH_CHECK(!session.IsRunning());
    SH_CHECK(!std::filesystem::exists(path)); // nothing opened
}

SH_TEST(SekiroCombatCaptureSession_Start_ValidConfig_ClampsWindowAndInterval) {
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = kPlayerGameDataMaxCaptureBytes + 10000; // over the scope's cap
    config.samplingInterval = std::chrono::milliseconds(1);                   // under the min

    auto path = TempCapturePath("c");
    CombatCaptureStartResult result = session.Start(config, kRegionAddr, 1, path.string(), 0);
    SH_CHECK(result == CombatCaptureStartResult::Started);
    SH_CHECK(session.EffectiveWindowSizeBytes() == kPlayerGameDataMaxCaptureBytes);
    SH_CHECK(session.SamplingInterval() == kMinCombatCaptureIntervalMs);
    session.Stop();
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_NoChange_WritesNoDeltaRecords) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x11);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("d");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    // Interval elapsed, region bytes unchanged.
    SH_CHECK(session.Tick(kRegionAddr, 1, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    CombatCaptureStats stats = session.Stats();
    SH_CHECK(stats.samplesTaken == 1);
    SH_CHECK(stats.deltaRecordsWritten == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_TooSoon_IsSilentNoOp) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x11);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("e");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    // Called again immediately -- before even the minimum interval elapsed.
    SH_CHECK(session.Tick(kRegionAddr, 1, 1000)); // only 1ms later
    session.Stop();

    SH_CHECK(session.Stats().samplesTaken == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_OneCellChanges_WritesExactlyOneDeltaRecord) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("f");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    std::int32_t newValue = 500;
    reader.PokeBytes(kRegionAddr + 0x18, &newValue, sizeof(newValue));

    SH_CHECK(session.Tick(kRegionAddr, 1, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    SH_CHECK(session.Stats().deltaRecordsWritten == 1);

    std::string content = ReadWholeFile(path);
    SH_CHECK(CountOccurrences(content, "\"recordKind\":\"delta\"") == 1);
    SH_CHECK(content.find("\"offset\":24") != std::string::npos); // 0x18 == 24
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_RegionUnavailable_CountsAsDropped) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("g");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    SH_CHECK(session.Tick(/*regionBaseAddress=*/0, 1, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    CombatCaptureStats stats = session.Stats();
    SH_CHECK(stats.droppedSamples == 1);
    SH_CHECK(stats.deltaRecordsWritten == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_ReadFails_CountsAsDropped) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("h");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    reader.SetAlive(false);
    SH_CHECK(session.Tick(kRegionAddr, 1, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    SH_CHECK(session.Stats().droppedSamples == 1);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_GenerationChanges_RecordsDiscontinuityAndRebaselines) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0xAA);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("i");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    // A different "instance" (generation 2) with entirely different bytes --
    // must never be diffed against generation 1's baseline.
    std::vector<std::uint8_t> newInstanceData(kPlayerGameDataMaxCaptureBytes, 0xBB);
    reader.PokeBytes(kRegionAddr, newInstanceData.data(), newInstanceData.size());

    SH_CHECK(session.Tick(kRegionAddr, 2, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    CombatCaptureStats stats = session.Stats();
    SH_CHECK(stats.discontinuities == 1);
    SH_CHECK(stats.deltaRecordsWritten == 0); // re-baselined, not diffed

    std::string content = ReadWholeFile(path);
    SH_CHECK(content.find("\"recordKind\":\"discontinuity\"") != std::string::npos);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Mark_WhileRunning_WritesMarkerRecord) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("j");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    SH_CHECK(session.Mark("perfect_deflect", 5000));
    session.Stop();

    SH_CHECK(session.Stats().markersWritten == 1);
    std::string content = ReadWholeFile(path);
    SH_CHECK(content.find("\"recordKind\":\"marker\"") != std::string::npos);
    SH_CHECK(content.find("\"label\":\"perfect_deflect\"") != std::string::npos);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Mark_NotRunning_ReturnsFalse) {
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    SH_CHECK(!session.Mark("idle", 0));
}

SH_TEST(SekiroCombatCaptureSession_Tick_NotRunning_ReturnsFalse) {
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    SH_CHECK(!session.Tick(kRegionAddr, 1, 0));
}

SH_TEST(SekiroCombatCaptureSession_Tick_LargeGapBetweenSamples_CountsAsLate) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.samplingInterval = std::chrono::milliseconds(10);
    auto path = TempCapturePath("k");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    // 100ms later -- 10x the configured interval, well over the 2x "late" threshold.
    SH_CHECK(session.Tick(kRegionAddr, 1, 100 * 1000));
    session.Stop();

    SH_CHECK(session.Stats().lateSamples == 1);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Stop_ThenStart_CanRestart) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("l");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    session.Stop();
    SH_CHECK(!session.IsRunning());

    CombatCaptureStartResult second = session.Start(config, kRegionAddr, 1, path.string(), 0);
    SH_CHECK(second == CombatCaptureStartResult::Started);
    session.Stop();
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Start_WhileAlreadyRunning_ReturnsAlreadyRunning) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    auto path = TempCapturePath("m");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    CombatCaptureStartResult second = session.Start(config, kRegionAddr, 1, path.string(), 1000);
    SH_CHECK(second == CombatCaptureStartResult::AlreadyRunning);
    session.Stop();
    std::filesystem::remove(path);
}
