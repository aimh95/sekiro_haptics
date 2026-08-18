#pragma once

// Parses one raw signal-probe CLI command line and formats a
// SignalProbeScanController operation's result into human-readable output
// lines (SEK-PROBE-001C-2). Recognizes exactly plan/begin/begin-disk/
// filter/resume/status/list -- any other verb (identity/regions/watch/
// mark/stop/quit/unknown) is reported as unhandled so apps/sekiro_signal_
// probe/main.cpp's existing branches for those stay completely untouched.
// No console I/O of its own (never calls std::cout) and no Win32 --
// portable and Fake-testable like SignalProbeScanController.

#include "sekiro_haptics/process/SignalProbeScanController.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class SignalProbeCommandProcessor {
public:
    explicit SignalProbeCommandProcessor(SignalProbeScanController& controller);

    struct ProcessResult {
        /// False if `commandLine`'s verb isn't one this processor owns --
        /// `outputLines` is empty in that case, and the caller should fall
        /// through to its own handling.
        bool handled = false;
        std::vector<std::string> outputLines;
    };

    /// Parses and executes one command line. Never throws on malformed
    /// input -- an unparseable argument for an owned verb still returns
    /// handled=true, with a usage line explaining what was wrong.
    ProcessResult Process(const std::string& commandLine);

private:
    ProcessResult HandlePlan(std::istringstream& args);
    ProcessResult HandleBegin(std::istringstream& args);
    ProcessResult HandleBeginDisk(std::istringstream& args);
    ProcessResult HandleFilter(std::istringstream& args);
    ProcessResult HandleResume();
    ProcessResult HandleStatus();
    ProcessResult HandleList(std::istringstream& args);

    SignalProbeScanController& controller_;
};

} // namespace sekiro_haptics::process
