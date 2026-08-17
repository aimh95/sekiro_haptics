// Real Win32 integration test for SEK-PROBE-001A's signal-discovery
// building blocks: launches the actual helper executable
// (process_reader_helper_main.cpp), attaches read-only, enumerates its
// readable memory regions, runs a real candidate scan over its main
// module, narrows via pipe-driven value changes (changed / increased /
// decreased / exact), and exercises the watch JSONL writer. No sleep is
// used -- HelperProcess's pipe/process-handle synchronization (including
// the new command/"OK"-ack protocol) is reused as-is.

#include "sekiro_haptics/Json.hpp"
#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/DiscoverySession.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "HelperProcess.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace sekiro_haptics::process;
using namespace sekiro_haptics::json;

namespace {

bool Contains(const std::vector<Candidate>& candidates, std::uintptr_t address) {
    return std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c) { return c.address == address; });
}

std::filesystem::path SignalProbeTestDir() {
    return std::filesystem::temp_directory_path() / "sh_signal_probe_integration_tests";
}

} // namespace

SH_TEST(SignalProbe_Integration_RealHelperProcess_ScanFilterAndWatchCapture) {
    HelperProcess helper;
    SH_CHECK(helper.Start(SH_PROCESS_READER_HELPER_EXE));

    std::string readyLine;
    SH_CHECK(helper.ReadLine(readyLine));
    SH_CHECK(readyLine.rfind("READY ", 0) == 0);

    std::uint32_t pid = 0;
    std::uintptr_t sentinelAddr = 0, incrementAddr = 0, decrementAddr = 0, toggleAddr = 0, noiseAddr = 0;
    {
        std::istringstream iss(readyLine);
        std::string tag, pidTok, addrTok, lenTok, aobAddrTok, aobLenTok, matchOffsetTok, targetTok, sentinelTok,
            incrementTok, decrementTok, toggleTok, noiseTok;
        iss >> tag >> pidTok >> addrTok >> lenTok >> aobAddrTok >> aobLenTok >> matchOffsetTok >> targetTok >>
            sentinelTok >> incrementTok >> decrementTok >> toggleTok >> noiseTok;
        pid = static_cast<std::uint32_t>(ParseHelperKeyValue(pidTok));
        sentinelAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(sentinelTok));
        incrementAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(incrementTok));
        decrementAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(decrementTok));
        toggleAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(toggleTok));
        noiseAddr = static_cast<std::uintptr_t>(ParseHelperKeyValue(noiseTok));
    }

    // 1-2: launch (done above) + PID-based read-only attach.
    Win32ProcessReader reader;
    SH_CHECK(reader.AttachByPid(pid) == ProcessReaderResult::Success);

    // (regions) readable-region enumeration succeeds and is non-empty for
    // a real, running process.
    std::vector<ProcessMemoryRegion> regions;
    SH_CHECK(reader.EnumerateReadableRegions(regions) == MemoryMapResult::Success);
    SH_CHECK(!regions.empty());

    // 3: initial scan over the helper's own main module (the watch values
    // are static globals -- part of the module image, not private heap).
    std::vector<Candidate> candidates;
    CandidateScanResult scanResult =
        BeginCandidateScan(reader, reader, reader, CandidateScanScope::MainModule, CandidateValueType::U32, candidates);
    SH_CHECK(scanResult == CandidateScanResult::Success);
    std::size_t initialCount = candidates.size();
    SH_CHECK(initialCount > 0);
    // Every known watch address must appear as a 4-byte-aligned candidate
    // in the initial, unfiltered scan.
    SH_CHECK(Contains(candidates, sentinelAddr));
    SH_CHECK(Contains(candidates, incrementAddr));
    SH_CHECK(Contains(candidates, decrementAddr));
    SH_CHECK(Contains(candidates, toggleAddr));
    SH_CHECK(Contains(candidates, noiseAddr));

    // 4: change three of the five values via real pipe commands, each
    // synchronized by the helper's "OK" acknowledgement -- no sleep.
    SH_CHECK(helper.SendCommandAndWaitForAck("INCREMENT")); // watchIncrement 1000 -> 1010, watchNoise 0 -> 1
    SH_CHECK(helper.SendCommandAndWaitForAck("DECREMENT")); // watchDecrement 1000 -> 990, watchNoise 1 -> 2
    SH_CHECK(helper.SendCommandAndWaitForAck("TOGGLE"));    // watchToggle 0 -> 1, watchNoise 2 -> 3

    // 5: "changed" narrows sharply -- the untouched sentinel (and the vast
    // majority of otherwise-static module content) must be excluded, while
    // every value actually touched survives.
    std::vector<Candidate> changed = candidates;
    SH_CHECK(FilterCandidates(reader, changed, CandidateFilterKind::Changed) == CandidateFilterResult::Success);
    SH_CHECK(changed.size() < initialCount);
    SH_CHECK(!Contains(changed, sentinelAddr));
    SH_CHECK(Contains(changed, incrementAddr));
    SH_CHECK(Contains(changed, decrementAddr));
    SH_CHECK(Contains(changed, toggleAddr));
    SH_CHECK(Contains(changed, noiseAddr));

    // 6a: "increased", applied fresh against the original pre-command
    // baseline (NOT the `changed` copy above -- a Filter() call always
    // advances its survivors' baseline to the just-read current value, so
    // chaining directly off `changed` here would compare each value
    // against itself and never show as increased), excludes the one value
    // that went down.
    std::vector<Candidate> increased = candidates;
    SH_CHECK(FilterCandidates(reader, increased, CandidateFilterKind::Increased) == CandidateFilterResult::Success);
    SH_CHECK(!Contains(increased, decrementAddr));
    SH_CHECK(Contains(increased, incrementAddr));

    // 6b: "decreased", applied fresh against the same original baseline,
    // isolates exactly the one value that went down -- unrelated
    // survivors (increment, toggle, noise, all of which went up) are
    // removed.
    std::vector<Candidate> decreased = candidates;
    SH_CHECK(FilterCandidates(reader, decreased, CandidateFilterKind::Decreased) == CandidateFilterResult::Success);
    SH_CHECK(decreased.size() == 1);
    SH_CHECK(decreased[0].address == decrementAddr);

    // 7: "exact", applied to the increased set, fully isolates the
    // expected address -- watchNoise also increased but never equals
    // 1010, so it (an unrelated survivor) is removed here.
    CandidateValue exactTarget = std::uint32_t{1010};
    SH_CHECK(FilterCandidates(reader, increased, CandidateFilterKind::ExactValue, &exactTarget) ==
             CandidateFilterResult::Success);
    SH_CHECK(increased.size() == 1);
    SH_CHECK(increased[0].address == incrementAddr);

    // 8-9: watch JSONL -- a sample, then a marker, then another sample,
    // written in that order and read back in that order.
    std::filesystem::create_directories(SignalProbeTestDir());
    std::filesystem::path watchPath = SignalProbeTestDir() / "watch.jsonl";
    {
        DiscoveryWatchWriter writer(watchPath.string());
        SH_CHECK(writer.IsOpen());
        SH_CHECK(writer.WriteSample(100, "candidate/watch_increment", incrementAddr, CandidateValueType::U32,
                                     CandidateValue{std::uint32_t{1010}}));
        SH_CHECK(writer.WriteMarker(150, "test_marker"));
        SH_CHECK(writer.WriteSample(200, "candidate/watch_increment", incrementAddr, CandidateValueType::U32,
                                     CandidateValue{std::uint32_t{1010}}));
        writer.Close();
    }

    {
        std::ifstream in(watchPath);
        SH_CHECK(in.is_open());
        std::string line;
        std::vector<std::string> recordKinds;
        std::vector<std::int64_t> timestamps;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            JsonParseResult parsed = ParseJson(line);
            SH_CHECK(parsed.ok);
            const JsonValue* kind = parsed.value.Find("recordKind");
            SH_CHECK(kind != nullptr && kind->IsString());
            recordKinds.push_back(kind->AsString());
            const JsonValue* ts = parsed.value.Find("timestampUs");
            SH_CHECK(ts != nullptr && ts->IsNumber());
            timestamps.push_back(static_cast<std::int64_t>(ts->AsNumber()));
        }
        SH_CHECK(recordKinds.size() == 3);
        SH_CHECK(recordKinds[0] == "sample");
        SH_CHECK(recordKinds[1] == "marker");
        SH_CHECK(recordKinds[2] == "sample");
        SH_CHECK(timestamps[0] == 100);
        SH_CHECK(timestamps[1] == 150);
        SH_CHECK(timestamps[2] == 200);
    }
    std::filesystem::remove(watchPath); // 12: output cleanup

    // 10: after the helper exits, a scan against the same (still
    // attached, no longer alive) reader must not succeed -- no sleep,
    // WaitForExit() blocks on the real process handle.
    helper.SendExit();
    SH_CHECK(helper.WaitForExit(5000));

    std::vector<Candidate> afterExit;
    CandidateScanResult afterExitResult = BeginCandidateScan(reader, reader, reader, CandidateScanScope::MainModule,
                                                               CandidateValueType::U32, afterExit);
    SH_CHECK(afterExitResult == CandidateScanResult::ProcessExited);

    // 11: detach -> subsequent scans report NotAttached.
    reader.Detach();
    std::vector<Candidate> afterDetach;
    CandidateScanResult afterDetachResult = BeginCandidateScan(reader, reader, reader, CandidateScanScope::MainModule,
                                                                 CandidateValueType::U32, afterDetach);
    SH_CHECK(afterDetachResult == CandidateScanResult::NotAttached);

    // 12: HelperProcess's destructor (end of scope) closes its own
    // process/thread/pipe handles; Win32ProcessReader already released its
    // handle inside Detach() above.
}
