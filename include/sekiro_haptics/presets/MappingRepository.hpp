#pragma once

#include "sekiro_haptics/presets/EventMapping.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sekiro_haptics {

/// One problem found while loading a single entry from a mappings JSON
/// file. `entryIndex` is the entry's position in the top-level "mappings"
/// array (0-based).
struct MappingLoadError {
    std::size_t entryIndex = 0;
    std::string message;
};

/// Result of MappingRepository::LoadFromFile().
struct MappingLoadOutcome {
    /// False only if the file could not be opened or its top-level content
    /// was not valid JSON / not the expected shape.
    bool ok = false;
    std::size_t loadedCount = 0;
    std::vector<MappingLoadError> errors;
    std::string fatalError;
};

/// Loads and looks up EventMapping instances by (gameId, eventId).
///
/// An entry missing "gameId", "eventId", or "presetId" is rejected --
/// recorded in `errors`, not added. This repository does NOT check that
/// `presetId` refers to an existing PresetRepository entry -- the two
/// repositories are loaded independently of each other; cross-referencing
/// happens in ReplayPipeline, which is what actually needs both.
class MappingRepository {
public:
    MappingLoadOutcome LoadFromFile(const std::string& path);

    /// Inserts or replaces the mapping for this (gameId, eventId) pair.
    void AddMapping(EventMapping mapping);

    /// Returns nullptr if no mapping exists for this (gameId, eventId).
    const EventMapping* Find(const std::string& gameId, const std::string& eventId) const;

    std::size_t Size() const;

private:
    std::map<std::pair<std::string, std::string>, EventMapping> mappings_;
};

} // namespace sekiro_haptics
