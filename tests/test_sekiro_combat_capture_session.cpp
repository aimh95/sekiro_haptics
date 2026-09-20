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
    static const auto run = std::chrono::steady_clock::now().time_since_epoch().count();
    static unsigned counter = 0;
    return std::filesystem::temp_directory_path() / ("sh_combat_capture_" + name + "_" +
        std::to_string(run) + "_" + std::to_string(counter++) + ".jsonl");
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

SH_TEST(SekiroCombatCaptureSession_ExistingFileIsNeverOverwritten) {
    auto path = TempCapturePath("exclusive-v3");
    { std::ofstream file(path); file << "keep-this-record\n"; }
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    SH_CHECK(session.Start({}, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::OutputExists);
    SH_CHECK(ReadWholeFile(path) == "keep-this-record\n");
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_NewGenerationFailedFirstReadCannotDiffOldBytes) {
    auto path = TempCapturePath("failed-new-generation-v3");
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = 4;
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    reader.FailReadAtCall(reader.ReadCalls());
    SH_CHECK(session.Tick(kRegionAddr + 0x1000, 2, 5000));
    const std::uint32_t newValue = 50;
    reader.PokeBytes(kRegionAddr + 0x1000, &newValue, 4);
    SH_CHECK(session.Tick(kRegionAddr + 0x1000, 2, 10000));
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    SH_CHECK(session.Stats().baselineRecordsWritten == 2);
    SH_CHECK(session.Stats().readFailedSamples == 1);
    session.Stop();
    SH_CHECK(CountOccurrences(ReadWholeFile(path), "\"recordKind\":\"delta\"") == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_UnresolvedThenSameGenerationRequiresBaseline) {
    auto path = TempCapturePath("unresolved-recovery-v3");
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = 4;
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    SH_CHECK(session.Tick(0, 1, 5000));
    const std::uint32_t value = 999;
    reader.PokeBytes(kRegionAddr, &value, 4);
    SH_CHECK(session.Tick(kRegionAddr, 1, 10000));
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    SH_CHECK(session.Stats().baselineRecordsWritten == 2);
    session.Stop();
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_AddressChangeWithSameGenerationRequiresBaseline) {
    auto path = TempCapturePath("address-change-v3");
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = 4;
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    const std::uint32_t value = 999;
    reader.PokeBytes(kRegionAddr + 0x1000, &value, 4);
    SH_CHECK(session.Tick(kRegionAddr + 0x1000, 1, 5000));
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    SH_CHECK(session.Stats().discontinuities == 1);
    session.Stop();
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_PartialReadClearsBaseline) {
    auto path = TempCapturePath("partial-recovery-v3");
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = 4;
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    reader.ForcePartialReadAtCall(reader.ReadCalls(), 2);
    SH_CHECK(session.Tick(kRegionAddr, 1, 5000));
    const std::uint32_t value = 42;
    reader.PokeBytes(kRegionAddr, &value, 4);
    SH_CHECK(session.Tick(kRegionAddr, 1, 10000));
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    session.Stop();
    SH_CHECK(ReadWholeFile(path).find("PartialRead") != std::string::npos);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_SampleCommitsIncludeUnchangedObservations) {
    auto path = TempCapturePath("unchanged-v3");
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    SH_CHECK(session.Start({}, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    SH_CHECK(session.Tick(kRegionAddr, 1, 5000));
    SH_CHECK(session.Tick(kRegionAddr, 1, 10000));
    session.Stop();
    const auto contents = ReadWholeFile(path);
    SH_CHECK(CountOccurrences(contents, "\"recordKind\":\"baseline\"") == 1);
    SH_CHECK(CountOccurrences(contents, "\"recordKind\":\"sample\"") == 2);
    SH_CHECK(CountOccurrences(contents, "\"recordKind\":\"capture_end\"") == 1);
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_ChangedOwnerDuringReadDiscardsSample) {
    auto path = TempCapturePath("owner-validation-v3");
    FakeProcessReader reader;
    bool sameOwner = true;
    SekiroCombatCaptureSession session(reader, [&](auto, auto) { return sameOwner; });
    SH_CHECK(session.Start({}, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    sameOwner = false;
    SH_CHECK(session.Tick(kRegionAddr, 1, 5000));
    SH_CHECK(session.Stats().inconsistentSamples == 1);
    sameOwner = true;
    SH_CHECK(session.Tick(kRegionAddr, 1, 10000));
    SH_CHECK(session.Stats().deltaRecordsWritten == 0);
    SH_CHECK(session.Stats().baselineRecordsWritten == 2);
    session.Stop();
    std::filesystem::remove(path);
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

SH_TEST(SekiroCombatCaptureSession_Start_CustomAddressScope_ClampsToItsOwnCap) {
    FakeProcessReader reader;
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.scope = CombatCaptureScope::CustomAddress;
    config.requestedWindowSizeBytes = kCustomAddressMaxCaptureBytes + 10000; // over this scope's cap

    auto path = TempCapturePath("custom-clamp");
    CombatCaptureStartResult result = session.Start(config, kRegionAddr, 1, path.string(), 0);
    SH_CHECK(result == CombatCaptureStartResult::Started);
    SH_CHECK(session.EffectiveWindowSizeBytes() == kCustomAddressMaxCaptureBytes);
    session.Stop();
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_CustomAddressScope_DetectsChange) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(256, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.scope = CombatCaptureScope::CustomAddress;
    config.requestedWindowSizeBytes = 256;
    auto path = TempCapturePath("custom-tick");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);

    std::int32_t newValue = 42;
    reader.PokeBytes(kRegionAddr + 0x10, &newValue, sizeof(newValue));
    SH_CHECK(session.Tick(kRegionAddr, 1, kDefaultCombatCaptureIntervalMs.count() * 1000));
    session.Stop();

    SH_CHECK(session.Stats().deltaRecordsWritten == 1);
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
    SH_CHECK(stats.unresolvedSamples == 1);
    SH_CHECK(stats.droppedSamplesTotal == 1);
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

    SH_CHECK(session.Stats().readFailedSamples == 1);
    SH_CHECK(session.Stats().droppedSamplesTotal == 1);
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

    SH_CHECK(session.Mark("perfect_deflect", 5000, 5200));
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
    SH_CHECK(!session.Mark("idle", 0, 0));
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

SH_TEST(SekiroCombatCaptureSession_Tick_ExactlyOneIntervalLate_CountsAsLate) {
    // lateSamples uses >= against lateToleranceUs (default: exactly one
    // configured interval), not >. A sample arriving at *exactly* one
    // interval late has already missed that one whole cycle -- this must
    // count as late, not be waved through as "right on the boundary."
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.samplingInterval = std::chrono::milliseconds(10);
    auto path = TempCapturePath("n");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    // First scheduled slot is t=10'000us; calling at exactly t=20'000us is
    // exactly one interval (10'000us) late.
    SH_CHECK(session.Tick(kRegionAddr, 1, 20'000));
    session.Stop();

    SH_CHECK(session.Stats().lateSamples == 1);
    SH_CHECK(session.Stats().missedScheduleSamples == 1);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_JustUnderOneIntervalLate_DoesNotCountAsLate) {
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.samplingInterval = std::chrono::milliseconds(10);
    auto path = TempCapturePath("o");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    // t=19'999us -- 1us short of a full interval late.
    SH_CHECK(session.Tick(kRegionAddr, 1, 19'999));
    session.Stop();

    SH_CHECK(session.Stats().lateSamples == 0);
    SH_CHECK(session.Stats().missedScheduleSamples == 0);
    std::filesystem::remove(path);
}

SH_TEST(SekiroCombatCaptureSession_Tick_LargeGap_NoCatchUpBurst_OneSampleAndScheduleJumpsPastMissedSlots) {
    // If the sampler thread was stalled (e.g. process suspended, or a long
    // command blocked the old cadence), Tick() must not "replay" every
    // missed interval as a burst of samples once it's called again -- it
    // takes exactly one sample for the current instant, counts the missed
    // slots as dropped, and advances the schedule to the next slot after
    // *now* (not one-by-one through every missed slot).
    FakeProcessReader reader;
    std::vector<std::uint8_t> data(kPlayerGameDataMaxCaptureBytes, 0x00);
    reader.PokeBytes(kRegionAddr, data.data(), data.size());

    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.samplingInterval = std::chrono::milliseconds(10);
    auto path = TempCapturePath("m");
    SH_CHECK(session.Start(config, kRegionAddr, 1, path.string(), 0) == CombatCaptureStartResult::Started);
    // First scheduled slot is t=10'000us.

    // Stall for 100ms (9 slots missed beyond the first due one) before the
    // sampler thread gets to call Tick() again.
    SH_CHECK(session.Tick(kRegionAddr, 1, 100'000));
    SH_CHECK(session.Stats().samplesTaken == 1); // one sample, not one per missed slot
    SH_CHECK(session.Stats().missedScheduleSamples == 9); // the 9 skipped slots, counted not replayed
    SH_CHECK(session.Stats().droppedSamplesTotal == 9);
    SH_CHECK(session.Stats().readFailedSamples == 0);
    SH_CHECK(session.Stats().unresolvedSamples == 0);

    // A dedicated sampler thread calls Tick() again almost immediately
    // (e.g. 1us later, matching main.cpp's ~1ms poll loop) -- this must be
    // a silent no-op, not a second catch-up sample, because the schedule
    // already jumped to the next slot strictly after the stall ended.
    SH_CHECK(session.Tick(kRegionAddr, 1, 100'001));
    SH_CHECK(session.Stats().samplesTaken == 1);

    session.Stop();
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
    SH_CHECK(second == CombatCaptureStartResult::OutputExists);
    const auto freshPath = TempCapturePath("restart-fresh");
    SH_CHECK(session.Start(config, kRegionAddr, 1, freshPath.string(), 0) == CombatCaptureStartResult::Started);
    session.Stop();
    std::filesystem::remove(path);
    std::filesystem::remove(freshPath);
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
