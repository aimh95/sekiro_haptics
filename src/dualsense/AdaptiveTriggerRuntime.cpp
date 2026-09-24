#include "sekiro_haptics/dualsense/AdaptiveTriggerRuntime.hpp"

namespace sekiro_haptics::dualsense {

AdaptiveTriggerRuntime::AdaptiveTriggerRuntime(IDualSenseTransport& transport,
                                               DualSenseOutputState& state)
    : transport_(transport), state_(state) {}

ActiveTriggerEffect& AdaptiveTriggerRuntime::SlotLocked(TriggerSide side) {
    return side == TriggerSide::Left ? left_ : right_;
}

const ActiveTriggerEffect& AdaptiveTriggerRuntime::SlotLocked(TriggerSide side) const {
    return side == TriggerSide::Left ? left_ : right_;
}

TransportResult AdaptiveTriggerRuntime::SubmitLocked(bool force) {
    const auto result = state_.Submit(transport_, force);
    if (result != TransportResult::Success) {
        ++stats_.writeFailures;
        lastError_ = state_.LastError();
    }
    return result;
}

void AdaptiveTriggerRuntime::ReleaseLocked(TriggerSide side) {
    state_.ReleaseTrigger(side);
    SlotLocked(side) = ActiveTriggerEffect{};
}

TriggerApplyResult AdaptiveTriggerRuntime::Apply(TriggerSide side, const TriggerEffectSpec& spec,
                                                 std::int64_t durationUs, std::int64_t nowUs) {
    TriggerApplyResult out;
    std::lock_guard<std::mutex> lock(mutex_);

    if (durationUs < 0) {
        out.error = "duration must not be negative";
        ++stats_.rejectedSpecs;
        lastError_ = out.error;
        return out;
    }

    const auto encoded = state_.SetTrigger(side, spec);
    if (!encoded.ok) {
        // The state was left untouched, so whatever the side was holding is
        // still both in memory and on the device.
        out.error = encoded.error;
        ++stats_.rejectedSpecs;
        lastError_ = out.error;
        return out;
    }

    auto& slot = SlotLocked(side);
    if (slot.active) ++stats_.replaced;

    // The new id is what makes the replaced effect's pending expiry harmless:
    // Tick() will find the slot holding a different id and do nothing.
    slot.active = true;
    slot.effectId = nextEffectId_++;
    slot.spec = spec;
    slot.startedUs = nowUs;
    slot.expiresAtUs = durationUs == kHoldUntilReplaced ? 0 : nowUs + durationUs;

    ++stats_.applied;
    out.accepted = true;
    out.effectId = slot.effectId;
    out.reducedToOff = encoded.reducedToOff;

    out.transport = SubmitLocked(false);
    if (out.transport != TransportResult::Success) {
        // A handled error releases this trigger rather than leaving the caller
        // believing an effect is on the device. The slot is cleared so nothing
        // later tries to expire an effect that never arrived.
        state_.ReleaseTrigger(side);
        slot = ActiveTriggerEffect{};
        out.accepted = false;
        out.effectId = 0;
        out.error = lastError_;
    }
    return out;
}

bool AdaptiveTriggerRuntime::Cancel(TriggerSide side, std::uint64_t effectId, std::int64_t nowUs) {
    (void)nowUs;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = SlotLocked(side);
    if (!slot.active || slot.effectId != effectId) {
        ++stats_.staleCancelsIgnored;
        return false;
    }
    ReleaseLocked(side);
    ++stats_.cancelled;
    SubmitLocked(false);
    return true;
}

void AdaptiveTriggerRuntime::CancelSide(TriggerSide side, std::int64_t nowUs) {
    (void)nowUs;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!SlotLocked(side).active && !state_.TriggerClaimed(side)) return;
    const bool wasActive = SlotLocked(side).active;
    ReleaseLocked(side);
    if (wasActive) ++stats_.cancelled;
    SubmitLocked(false);
}

void AdaptiveTriggerRuntime::CancelAll(std::int64_t nowUs) {
    (void)nowUs;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto side : {TriggerSide::Left, TriggerSide::Right}) {
        if (SlotLocked(side).active) ++stats_.cancelled;
        ReleaseLocked(side);
    }
    SubmitLocked(false);
}

void AdaptiveTriggerRuntime::Tick(std::int64_t nowUs) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (const auto side : {TriggerSide::Left, TriggerSide::Right}) {
        auto& slot = SlotLocked(side);
        if (!slot.active || slot.expiresAtUs == 0) continue;
        if (nowUs < slot.expiresAtUs) continue;
        ReleaseLocked(side);
        ++stats_.expired;
        changed = true;
    }
    if (changed) SubmitLocked(false);
}

void AdaptiveTriggerRuntime::ResetToNeutral(std::int64_t nowUs) {
    (void)nowUs;
    std::lock_guard<std::mutex> lock(mutex_);
    // Only the trigger sections. Audio routing and legacy rumble belong to
    // other owners and a trigger reset is not a reason to disturb them.
    for (const auto side : {TriggerSide::Left, TriggerSide::Right}) ReleaseLocked(side);
    ++stats_.resets;
    SubmitLocked(true);
}

void AdaptiveTriggerRuntime::OnReconnect(std::int64_t nowUs) {
    (void)nowUs;
    std::lock_guard<std::mutex> lock(mutex_);
    left_ = ActiveTriggerEffect{};
    right_ = ActiveTriggerEffect{};
    // Claim both sides at Off and force the write: the device's state after a
    // reconnect is unknown, so "nothing changed" is not a safe assumption.
    state_.SetTriggerBlock(TriggerSide::Left, OffTriggerBlock());
    state_.SetTriggerBlock(TriggerSide::Right, OffTriggerBlock());
    ++stats_.reconnects;
    SubmitLocked(true);
}

void AdaptiveTriggerRuntime::OnDeviceLost() {
    std::lock_guard<std::mutex> lock(mutex_);
    left_ = ActiveTriggerEffect{};
    right_ = ActiveTriggerEffect{};
    state_.ForgetClaims();
    // Nothing is written. Whether the controller is physically neutral is
    // unknown from here and is not asserted.
}

ActiveTriggerEffect AdaptiveTriggerRuntime::Active(TriggerSide side) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return SlotLocked(side);
}

TriggerRuntimeStats AdaptiveTriggerRuntime::Stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::string AdaptiveTriggerRuntime::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace sekiro_haptics::dualsense
