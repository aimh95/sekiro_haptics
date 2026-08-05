#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace sekiro_haptics {

/// A single raw, timestamped observation from a game (or a recorded trace
/// of one). This is intentionally generic and low-level -- it does not know
/// what "perfect deflect" means, only that some named signal had some value
/// at some time.
///
/// `signal` is a stable string identifier for a trace schema, e.g.
/// "player.hp_delta" or "manual.perfect_deflect" -- these names describe
/// what this project's trace format records, not any real Sekiro internals
/// (no memory addresses, offsets, or engine-specific IDs are implied). See
/// docs/trace-format.md for the full schema and docs/ARCHITECTURE.md for
/// how this fits with GameEvent (an interpreted event) and HapticEffect (a
/// haptic waveform).
///
/// `value` is always a string, even when the JSONL source field was a JSON
/// number or bool -- callers that need a typed value convert it themselves.
/// `extra` preserves any other JSON keys present on the source line,
/// stringified, so a detector can look at auxiliary fields a trace author
/// added without the schema needing to anticipate every future need.
struct GameSignal {
    std::chrono::microseconds timestamp{0};
    std::string signal;
    std::string value;
    std::unordered_map<std::string, std::string> extra;
};

} // namespace sekiro_haptics
