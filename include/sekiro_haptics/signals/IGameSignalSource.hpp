#pragma once

#include "sekiro_haptics/signals/GameSignal.hpp"

#include <string>

namespace sekiro_haptics {

/// Outcome of a single IGameSignalSource::Next() call.
enum class SignalSourceResult {
    Success,
    EndOfSource,
    MalformedInput,
};

/// Returns a human-readable name for a SignalSourceResult, e.g. for logging.
const char* ToString(SignalSourceResult result);

/// Abstraction over "a stream of GameSignal observations, in timestamp
/// order" -- a pipeline consumer (see ReplayPipeline) calls Next()
/// repeatedly without caring whether the signals came from a recorded
/// trace file or a live game.
///
/// Only ReplaySignalSource and VectorSignalSource exist today, both
/// replay-only. A future `LiveSekiroSignalSource` would implement this
/// interface on top of process observation (XInput reads, memory
/// reads, hooks, etc.) -- none of that exists in this repo. Such a source
/// would return `false` from Reset() (a live stream can't rewind); replay
/// sources return `true`. See docs/ARCHITECTURE.md.
class IGameSignalSource {
public:
    virtual ~IGameSignalSource() = default;

    /// Fills `outSignal` with the next signal in order. On any
    /// non-Success result, `*outError` (if non-null) may be set with a
    /// human-readable reason.
    virtual SignalSourceResult Next(GameSignal& outSignal, std::string* outError = nullptr) = 0;

    /// Restarts this source from its beginning. Returns false if this
    /// source cannot replay (e.g. a future live source).
    virtual bool Reset() = 0;
};

} // namespace sekiro_haptics
