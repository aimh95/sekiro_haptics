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
/// a delay, from a single dedicated background thread -- and, once an
/// effect is dispatched, ends it after its `duration` by calling
/// `backend.Reset()`, so a caller never needs to schedule its own
/// stop/follow-up. Duration bookkeeping lives entirely here, deliberately:
/// no IHapticBackend implementation (see DualSenseLegacyBackend) sleeps or
/// runs a timer thread of its own.
///
/// HapticScheduler exists to decouple "something happened in-game" from
/// "the controller should buzz right now": callers schedule an effect and
/// the scheduler takes care of timing, ordering, cancellation, and now
/// duration/stop. The scheduler does not own the backend and never blocks
/// the caller of Schedule()/Cancel()/Reset() on I/O.
///
/// Because legacy rumble can only represent one left/right state at a
/// time, the policy for a new effect dispatching before the previous one's
/// duration has elapsed is **latest effect wins**: the new effect is sent
/// immediately (never queued/delayed behind the old one), and it -- not
/// the old effect -- now owns the eventual auto-stop. The old effect's
/// now-superseded expiry is never allowed to fire a stale Reset() that
/// would cut off the new effect; see src/HapticScheduler.cpp for how a
/// generation counter enforces this.
class HapticScheduler {
public:
    /// `backend` must outlive this scheduler.
    explicit HapticScheduler(IHapticBackend& backend);

    /// Stops and joins the worker thread. If an effect was still active
    /// (dispatched, not yet auto-stopped or explicitly Reset()) at that
    /// point, sends one best-effort `backend.Reset()` before returning --
    /// its result isn't checked, since there's nothing left to do with a
    /// failure once the scheduler is going away.
    ~HapticScheduler();

    HapticScheduler(const HapticScheduler&) = delete;
    HapticScheduler& operator=(const HapticScheduler&) = delete;

    /// Queue `effect` for dispatch after `delay` (default: as soon as the
    /// worker thread can service it). Once dispatched, the effect
    /// auto-stops (backend.Reset()) after `effect.duration` -- unless a
    /// newer effect dispatches first, in which case that newer effect's
    /// duration governs the next stop instead (latest effect wins; see the
    /// class comment). A non-positive duration stops on the very next
    /// worker iteration, effectively "on" for one report only. Returns an
    /// id that can be passed to Cancel().
    HapticEffectId Schedule(HapticEffect effect, std::chrono::milliseconds delay = std::chrono::milliseconds{0});

    /// Prevent a pending effect from being dispatched. No-op if the effect
    /// has already been dispatched or does not exist (e.g. wrong/expired
    /// id). Returns true if a pending effect was actually cancelled. Does
    /// not affect an already-dispatched, still-active effect's pending
    /// auto-stop -- use Reset() for that.
    bool Cancel(HapticEffectId id);

    /// Cancels every pending (not yet dispatched) effect, cancels any
    /// pending auto-stop for an already-active effect, and resets the
    /// backend to a neutral state right now. Effects already in flight
    /// (mid-SendEffect-call) are unaffected.
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sekiro_haptics
