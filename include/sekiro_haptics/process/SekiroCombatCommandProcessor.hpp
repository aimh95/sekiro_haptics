#pragma once

// Parses combat-plan/combat-resolve/combat-status/combat-capture/
// combat-mark/combat-stop/combat-analyze/combat-export CLI command lines
// and formats SekiroCombatSessionController results into human-readable
// output lines -- same shape/conventions as SignalProbeCommandProcessor
// (see its own header comment). Recognizes only these verbs; any other verb
// is reported unhandled so main.cpp's existing branches stay untouched. No
// console I/O of its own, and never reads a clock itself (every
// timestamp-needing call takes timestamps from the caller, matching
// SekiroCombatCaptureSession's own contract) -- portable and Fake-testable.
//
// `inputTimestampUs` (SEK-PROBE-001E Section 7) is the precise moment the
// underlying input happened -- for a hotkey/controller-triggered command
// this is captured at the source thread, *not* whenever this Process()
// call actually gets around to running (which can lag behind by however
// long the command queue was backed up) -- see main.cpp's CommandQueue.
// `processedTimestampUs` is when this call executes, forwarded to
// combat-mark purely for diagnosing queue lag, never used for correlation.

#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class SekiroCombatCommandProcessor {
public:
    /// Preferred capture path. Existing paths are preserved; starts choose
    /// a new numbered sibling via exclusive creation. Export reports the
    /// path of the last successfully started capture.
    SekiroCombatCommandProcessor(SekiroCombatSessionController& controller, std::string captureOutputPath);

    struct ProcessResult {
        bool handled = false;
        std::vector<std::string> outputLines;
    };

    ProcessResult Process(const std::string& commandLine, std::int64_t inputTimestampUs,
                          std::int64_t processedTimestampUs);

private:
    ProcessResult HandleCombatPlan();
    ProcessResult HandleCombatResolve();
    ProcessResult HandleCombatStatus();
    ProcessResult HandleCombatCapture(std::istringstream& args, std::int64_t nowMonotonicUs);
    ProcessResult HandleCombatMark(std::istringstream& args, std::int64_t inputTimestampUs,
                                    std::int64_t processedTimestampUs);
    ProcessResult HandleCombatStop();
    ProcessResult HandleCombatAnalyze(std::istringstream& args);
    ProcessResult HandleCombatExport();

    SekiroCombatSessionController& controller_;
    std::string captureOutputPath_;
    std::string preferredCaptureOutputPath_;
    std::uint64_t capturePathSuffix_ = 0;
};

} // namespace sekiro_haptics::process
