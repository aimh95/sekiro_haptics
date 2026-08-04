#pragma once

#include "sekiro_haptics/IHapticBackend.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace sekiro_haptics {

/// Opaque handle returned by HapticScheduler::Schedule, used to cancel a
/// still-pending effect.
using HapticEffectId = std::uint64_t;

/// Dispatches HapticEffect instances to an IHapticBackend, optionally after
/// a delay, from a single dedicated background thread.
///
/// HapticScheduler exists to decouple "something happened in-game" from
/// "the controller should buzz right now": callers schedule an effect and
/// the scheduler takes care of timing, ordering, and cancellation. The
/// scheduler does not own the backend and never blocks the caller of
/// Schedule()/Cancel()/Reset() on I/O.
class HapticScheduler {
public:
    /// `backend` must outlive this scheduler.
    explicit HapticScheduler(IHapticBackend& backend);
    ~HapticScheduler();

    HapticScheduler(const HapticScheduler&) = delete;
    HapticScheduler& operator=(const HapticScheduler&) = delete;

    /// Queue `effect` for dispatch after `delay` (default: as soon as the
    /// worker thread can service it). Returns an id that can be passed to
    /// Cancel().
    HapticEffectId Schedule(HapticEffect effect, std::chrono::milliseconds delay = std::chrono::milliseconds{0});

    /// Prevent a pending effect from being dispatched. No-op if the effect
    /// has already been dispatched or does not exist (e.g. wrong/expired
    /// id). Returns true if a pending effect was actually cancelled.
    bool Cancel(HapticEffectId id);

    /// Cancel every pending effect and reset the backend to a neutral
    /// state. Effects already in flight are unaffected.
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sekiro_haptics
