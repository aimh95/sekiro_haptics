#pragma once

// Parses combat-plan/combat-resolve/combat-status/combat-capture/
// combat-mark/combat-stop/combat-analyze/combat-export CLI command lines
// and formats SekiroCombatSessionController results into human-readable
// output lines -- same shape/conventions as SignalProbeCommandProcessor
// (see its own header comment). Recognizes only these verbs; any other verb
// is reported unhandled so main.cpp's existing branches stay untouched. No
// console I/O of its own, and never reads a clock itself (every
// timestamp-needing call takes `nowMonotonicUs` from the caller, matching
// SekiroCombatCaptureSession's own contract) -- portable and Fake-testable.

#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class SekiroCombatCommandProcessor {
public:
    /// `captureOutputPath` is where combat-capture writes its delta JSONL
    /// (truncated fresh on each combat-capture start) and combat-analyze
    /// reads back -- fixed for this processor's lifetime, matching how the
    /// signal probe CLI already fixes watch.jsonl's path once at startup.
    SekiroCombatCommandProcessor(SekiroCombatSessionController& controller, std::string captureOutputPath);

    struct ProcessResult {
        bool handled = false;
        std::vector<std::string> outputLines;
    };

    ProcessResult Process(const std::string& commandLine, std::int64_t nowMonotonicUs);

private:
    ProcessResult HandleCombatPlan();
    ProcessResult HandleCombatResolve();
    ProcessResult HandleCombatStatus();
    ProcessResult HandleCombatCapture(std::istringstream& args, std::int64_t nowMonotonicUs);
    ProcessResult HandleCombatMark(std::istringstream& args, std::int64_t nowMonotonicUs);
    ProcessResult HandleCombatStop();
    ProcessResult HandleCombatAnalyze(std::istringstream& args);
    ProcessResult HandleCombatExport();

    SekiroCombatSessionController& controller_;
    std::string captureOutputPath_;
};

} // namespace sekiro_haptics::process
