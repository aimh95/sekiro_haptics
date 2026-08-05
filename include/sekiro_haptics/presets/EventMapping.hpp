#pragma once

#include <string>

namespace sekiro_haptics {

/// Connects a GameEvent to the HapticPreset that should play for it.
///
/// This is the one place "combat.perfect_deflect happens to feel like
/// sharp_metal_v1" is recorded -- neither GameEvent nor HapticPreset know
/// about each other directly. Normally loaded from JSON via
/// MappingRepository (see docs/trace-format.md).
struct EventMapping {
    std::string gameId;
    std::string eventId;
    std::string presetId;
};

} // namespace sekiro_haptics
