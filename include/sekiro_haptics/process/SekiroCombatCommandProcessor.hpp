#pragma once

// Parses combat-plan/combat-resolve/combat-status CLI command lines and
// formats SekiroCombatSessionController results into human-readable output
// lines -- same shape/conventions as SignalProbeCommandProcessor (see its
// own header comment). Recognizes only these three verbs; any other verb is
// reported unhandled so main.cpp's existing branches stay untouched. No
// console I/O of its own -- portable and Fake-testable.

#include "sekiro_haptics/process/SekiroCombatSessionController.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class SekiroCombatCommandProcessor {
public:
    explicit SekiroCombatCommandProcessor(SekiroCombatSessionController& controller);

    struct ProcessResult {
        bool handled = false;
        std::vector<std::string> outputLines;
    };

    ProcessResult Process(const std::string& commandLine);

private:
    ProcessResult HandleCombatPlan();
    ProcessResult HandleCombatResolve();
    ProcessResult HandleCombatStatus();

    SekiroCombatSessionController& controller_;
};

} // namespace sekiro_haptics::process
