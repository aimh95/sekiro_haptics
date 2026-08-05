#pragma once

#include "sekiro_haptics/events/GameEvent.hpp"
#include "sekiro_haptics/signals/GameSignal.hpp"

#include <vector>

namespace sekiro_haptics {

/// Turns raw GameSignal observations into interpreted GameEvent facts.
///
/// A detector sees signals one at a time, in timestamp order, and may emit
/// zero or more events per signal (a signal might mean nothing on its own,
/// or might complete a pattern spanning several prior signals).
class IGameEventDetector {
public:
    virtual ~IGameEventDetector() = default;

    /// Called once per incoming signal. Implementations append zero or more
    /// GameEvent instances to `outEvents`; they must not clear it.
    virtual void OnSignal(const GameSignal& signal, std::vector<GameEvent>& outEvents) = 0;

    /// Called once after the signal stream ends. Default is a no-op.
    /// Override this for a detector that buffers signals to correlate
    /// across a time window and needs to flush any pending state (e.g. "no
    /// closing signal arrived before the trace ended"). No detector in this
    /// repo currently buffers anything -- this exists so a future
    /// correlating detector doesn't require an interface break.
    virtual void Flush(std::vector<GameEvent>& outEvents) {
        (void)outEvents;
    }
};

} // namespace sekiro_haptics
