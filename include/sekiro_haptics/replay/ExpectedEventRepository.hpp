#pragma once

#include "sekiro_haptics/replay/ExpectedEvent.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sekiro_haptics::replay {

/// One problem found while loading a single entry from an expected-events
/// JSON file. `entryIndex` is the entry's position in the top-level
/// "expectedEvents" array (0-based).
struct ExpectedEventLoadError {
    std::size_t entryIndex = 0;
    std::string message;
};

/// Result of ExpectedEventRepository::LoadFromFile().
struct ExpectedEventLoadOutcome {
    /// False only if the file could not be opened or its top-level content
    /// was not the expected shape (a JSON object with an "expectedEvents"
    /// array). Individual malformed entries do not set this to false --
    /// see `errors`.
    bool ok = false;
    std::size_t loadedCount = 0;
    std::vector<ExpectedEventLoadError> errors;
    std::string fatalError;
};

/// Loads ExpectedEvent instances from JSON:
///
/// {"expectedEvents": [{"gameId": "sekiro", "eventId":
/// "combat.perfect_deflect", "timestampUs": 1040}]}
///
/// An entry missing/empty "gameId" or "eventId", or missing/non-numeric
/// "timestampUs", is rejected on its own (recorded in `errors`) -- the
/// rest of the file still loads.
class ExpectedEventRepository {
public:
    ExpectedEventLoadOutcome LoadFromFile(const std::string& path);

    const std::vector<ExpectedEvent>& Events() const;

private:
    std::vector<ExpectedEvent> events_;
};

} // namespace sekiro_haptics::replay
