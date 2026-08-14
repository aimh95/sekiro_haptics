#include "sekiro_haptics/replay/ReplayComparator.hpp"

namespace sekiro_haptics::replay {

ReplayComparisonResult CompareEvents(const std::vector<ExpectedEvent>& expected, const std::vector<GameEvent>& actual,
                                      std::chrono::microseconds maxLatency) {
    ReplayComparisonResult result;
    result.expectedCount = expected.size();
    result.detectedCount = actual.size();

    std::vector<bool> matched(expected.size(), false);

    for (const GameEvent& event : actual) {
        bool matchedThisEvent = false;
        bool duplicateCandidate = false;

        for (std::size_t i = 0; i < expected.size(); ++i) {
            const ExpectedEvent& exp = expected[i];
            if (exp.gameId != event.gameId || exp.eventId != event.eventId) {
                continue;
            }
            if (event.timestamp < exp.timestamp || event.timestamp > exp.timestamp + maxLatency) {
                continue;
            }

            if (!matched[i]) {
                matched[i] = true;
                result.latencies.push_back(event.timestamp - exp.timestamp);
                ++result.matchedCount;
                matchedThisEvent = true;
                break;
            }
            duplicateCandidate = true;
        }

        if (!matchedThisEvent) {
            if (duplicateCandidate) {
                result.duplicates.push_back(event);
            } else {
                result.falsePositives.push_back(event);
            }
        }
    }

    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!matched[i]) {
            result.missed.push_back(expected[i]);
        }
    }

    return result;
}

} // namespace sekiro_haptics::replay
