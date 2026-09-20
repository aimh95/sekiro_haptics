#pragma once
#include "sekiro_haptics/ActionFeedback.hpp"
#include <istream>
#include <vector>
namespace sekiro_haptics {
struct TimedAction { std::int64_t atUs; SekiroAction action; Prosthetic tool; };
// Strict finite demo/replay schedule. Throws with line number on invalid input.
// Independent from live-memory traces: these commands are author-supplied.
std::vector<TimedAction> LoadActionScript(std::istream& input);
}
