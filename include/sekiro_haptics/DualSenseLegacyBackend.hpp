#pragma once

#include "sekiro_haptics/IDualSenseTransport.hpp"
#include "sekiro_haptics/IHapticBackend.hpp"

#include <atomic>
#include <cstdint>
#include <ostream>

namespace sekiro_haptics {

/// IHapticBackend implementation that plays a HapticEffect's legacy
/// left/right rumble on a real DualSense, through an injected
/// IDualSenseTransport.
///
/// Scope is deliberately narrow: this backend interprets exactly one thing
/// -- `HapticEffect::intensity` (normalized left/right rumble) -- and
/// nothing else. It never inspects `HapticEffect::type`; every
/// HapticEffectType maps to the same legacy-rumble handling, because that
/// is the only representation HapticEffect carries today (see the
/// "Migration" note in docs/01-architecture.md). There is no PCM haptics,
/// adaptive-trigger, or speaker support -- not "not implemented yet" in
/// the sense of a stub that might silently misbehave, but structurally
/// absent: BuildRumbleReport() (the only report this class ever
/// constructs) cannot produce a report touching those fields at all, and
/// this class has no other output path. If HapticEffect ever grows a
/// representation for one of those effect kinds, this class must be
/// extended deliberately to reject or handle it -- it will not
/// automatically start doing something plausible-but-wrong.
///
/// Lifecycle:
/// - SendEffect() converts intensity to raw motor bytes (see ToMotorByte)
///   and writes one output report. It does not loop, sleep, or spawn a
///   thread to sustain the effect for its `duration` -- timing remains
///   HapticScheduler's responsibility, per IHapticBackend's contract.
/// - Reset() writes one all-zero-motor report ("stop"). It is what a
///   caller (HapticScheduler::Reset(), or a CLI/test explicitly ending a
///   session) uses to actually silence the controller; nothing in this
///   class calls it automatically after `duration` elapses.
/// - The destructor sends a best-effort Reset() if the last successful
///   SendEffect() left the motors non-zero, so a caller that forgets to
///   stop explicitly doesn't leave a controller buzzing indefinitely. This
///   is best-effort only (its result isn't checked -- there's nothing
///   meaningful to do with a failure from a destructor).
///
/// Never connected / a write failing: SendEffect()/Reset() return
/// HapticBackendResult::NotConnected / DeviceError rather than pretending
/// to have succeeded, and a failed non-zero SendEffect() never marks the
/// controller as "vibrating" (so the destructor won't attempt a
/// needless/incorrect stop for an effect that never actually landed).
/// Neither method retries.
///
/// Every SendEffect()/Reset() call logs one line to an injectable
/// std::ostream (defaults to std::cout, matching MockHapticBackend and
/// HidApiDualSenseTransport's existing pattern): "Effect dispatched ..."
/// for SendEffect, "Stop requested ..." for Reset(). Reset() is called
/// both for HapticScheduler's automatic duration-based stop and for any
/// caller's explicit/final stop -- these log lines are how the two get
/// distinguished in a combined program log (see apps/replay_hardware).
class DualSenseLegacyBackend final : public IHapticBackend {
public:
    /// `transport` must outlive this backend. Not opened/closed by this
    /// class -- the caller controls the transport's connection lifecycle
    /// (see apps/replay_hardware/main.cpp for the intended usage shape).
    /// `log` must outlive this object.
    explicit DualSenseLegacyBackend(IDualSenseTransport& transport, std::ostream& log = DefaultLogStream());
    ~DualSenseLegacyBackend() override;

    DualSenseLegacyBackend(const DualSenseLegacyBackend&) = delete;
    DualSenseLegacyBackend& operator=(const DualSenseLegacyBackend&) = delete;

    HapticBackendResult SendEffect(const HapticEffect& effect) override;
    HapticBackendResult Reset() override;
    bool IsConnected() const override;

    /// Converts a normalized intensity to a raw DualSense motor byte:
    /// clamps to [0.0, 1.0], multiplies by 255, and rounds to the nearest
    /// integer (half away from zero -- 0.5 -> 127.5 -> 128). Public and
    /// static so the conversion rule is directly unit-testable without a
    /// transport.
    static std::uint8_t ToMotorByte(float normalizedIntensity);

private:
    static std::ostream& DefaultLogStream();

    HapticBackendResult WriteMotorReport(std::uint8_t leftMotor, std::uint8_t rightMotor, const char* logVerb);

    IDualSenseTransport& transport_;
    std::ostream& log_;
    std::atomic<bool> vibrating_{false};
};

} // namespace sekiro_haptics
