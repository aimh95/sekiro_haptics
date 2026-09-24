#pragma once

// Lifetime for adaptive-trigger effects: who owns L2 and R2 right now, when
// what they are holding expires, and what happens on cancel, replace, reset,
// device loss and reconnect.
//
// THE RULE THAT MOTIVATES THE EFFECT ID
// -------------------------------------
// An effect with a duration has to be turned off when it runs out. If the
// only state kept were "this side has something scheduled", then applying a
// new effect while an old one is still pending leaves the old one's expiry
// armed -- and a moment later it releases the trigger, killing the effect the
// caller just set. So every Apply() mints a monotonically increasing effect
// id, each side remembers the id it is currently holding, and an expiry only
// releases the trigger when the id that expired is still the one held. The
// same check makes Cancel() safe against a stale id.
//
// WHAT THIS TYPE DOES NOT CLAIM
// -----------------------------
// It can return the triggers to neutral only while it can still write to the
// device. If the controller is unplugged or the process is killed, no write
// happens and whatever the firmware was last told stays in effect until
// something else changes it. OnDeviceLost() therefore drops this process's
// claims WITHOUT asserting the hardware was reset, and OnReconnect() starts
// from neutral rather than replaying what was set before -- an effect whose
// lifetime elapsed while the cable was out must not come back.

#include "sekiro_haptics/IDualSenseTransport.hpp"
#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"
#include "sekiro_haptics/dualsense/DualSenseOutputState.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace sekiro_haptics::dualsense {

/// Passed to Apply() as the effect's lifetime. Zero means "hold until
/// something replaces or cancels it" -- there is no implicit timeout, because
/// a resistance that quietly vanishes is worse than one that stays.
inline constexpr std::int64_t kHoldUntilReplaced = 0;

struct TriggerApplyResult {
    bool accepted = false;
    /// Non-zero only when accepted. Pass to Cancel() to release exactly this
    /// effect and nothing that replaced it.
    std::uint64_t effectId = 0;
    /// The parameters were valid but described no effect, so the trigger was
    /// released instead (see AdaptiveTrigger.hpp).
    bool reducedToOff = false;
    /// Set when the spec was rejected, or when the HID write failed. The
    /// distinction is in `transport`.
    std::string error;
    TransportResult transport = TransportResult::Success;
};

/// What a side is holding, for status output.
struct ActiveTriggerEffect {
    bool active = false;
    std::uint64_t effectId = 0;
    TriggerEffectSpec spec;
    std::int64_t startedUs = 0;
    /// 0 when the effect holds until replaced.
    std::int64_t expiresAtUs = 0;
};

struct TriggerRuntimeStats {
    std::uint64_t applied = 0;
    /// Applies that landed on a side already holding something.
    std::uint64_t replaced = 0;
    std::uint64_t expired = 0;
    std::uint64_t cancelled = 0;
    /// Cancel() calls naming an id that had already been replaced or expired.
    /// These are no-ops by design, and counting them is what proves the
    /// replacement rule is doing its job.
    std::uint64_t staleCancelsIgnored = 0;
    std::uint64_t rejectedSpecs = 0;
    std::uint64_t writeFailures = 0;
    std::uint64_t resets = 0;
    std::uint64_t reconnects = 0;
};

/// Serialises access to one DualSenseOutputState so trigger changes made from
/// a detection thread cannot interleave with audio/rumble changes made
/// elsewhere. The state and the transport must outlive this object.
class AdaptiveTriggerRuntime {
public:
    AdaptiveTriggerRuntime(IDualSenseTransport& transport, DualSenseOutputState& state);

    /// Applies `spec` to one side for `durationUs` (kHoldUntilReplaced for no
    /// expiry). Submits the COMPLETE output state, so speaker routing, the
    /// other trigger and any legacy-rumble values survive unchanged.
    TriggerApplyResult Apply(TriggerSide side, const TriggerEffectSpec& spec,
                             std::int64_t durationUs, std::int64_t nowUs);

    /// Releases `side` only if it is still holding `effectId`. Returns false
    /// (and counts a stale cancel) otherwise.
    bool Cancel(TriggerSide side, std::uint64_t effectId, std::int64_t nowUs);

    /// Releases whatever `side` holds, whatever its id.
    void CancelSide(TriggerSide side, std::int64_t nowUs);

    void CancelAll(std::int64_t nowUs);

    /// Releases any effect whose lifetime has elapsed. Cheap enough to call
    /// from a polling loop: it writes only when something actually expired.
    void Tick(std::int64_t nowUs);

    /// Both triggers Off and transmitted. Does not touch audio routing or
    /// legacy rumble -- resetting triggers is not a reason to drop the
    /// speaker.
    void ResetToNeutral(std::int64_t nowUs);

    /// A device has just appeared (or reappeared). Drops every remembered
    /// effect, forces a neutral state onto the device, and deliberately
    /// replays nothing.
    void OnReconnect(std::int64_t nowUs);

    /// The device went away. Forgets this process's claims. Writes nothing
    /// and makes no claim about the hardware's physical state.
    void OnDeviceLost();

    ActiveTriggerEffect Active(TriggerSide side) const;
    TriggerRuntimeStats Stats() const;
    std::string LastError() const;

private:
    /// Caller holds the lock. Releases the side in memory and marks it.
    void ReleaseLocked(TriggerSide side);
    TransportResult SubmitLocked(bool force);
    ActiveTriggerEffect& SlotLocked(TriggerSide side);
    const ActiveTriggerEffect& SlotLocked(TriggerSide side) const;

    IDualSenseTransport& transport_;
    DualSenseOutputState& state_;

    mutable std::mutex mutex_;
    ActiveTriggerEffect left_;
    ActiveTriggerEffect right_;
    std::uint64_t nextEffectId_ = 1;
    TriggerRuntimeStats stats_;
    std::string lastError_;
};

} // namespace sekiro_haptics::dualsense
