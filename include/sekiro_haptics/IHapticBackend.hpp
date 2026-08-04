#pragma once

#include "sekiro_haptics/HapticEffect.hpp"

namespace sekiro_haptics {

/// Outcome of a request made to an IHapticBackend.
///
/// A real backend talks to hardware that can be unplugged, busy, or
/// otherwise unable to service a request; callers (HapticScheduler, tests,
/// the console app) need to be able to tell success from failure rather
/// than assuming every call lands.
enum class HapticBackendResult {
    Success,
    NotConnected,
    DeviceError,
};

/// Returns a human-readable name for a HapticBackendResult, e.g. for logging.
const char* ToString(HapticBackendResult result);

/// Abstraction over a device capable of playing haptic effects.
///
/// Implementations own all device-specific detail (HID, USB, Bluetooth,
/// vendor SDKs, ...). This project currently ships only MockHapticBackend;
/// a real DualSense backend is out of scope for now (see docs/ARCHITECTURE.md).
class IHapticBackend {
public:
    virtual ~IHapticBackend() = default;

    /// Play a single effect. Implementations must return quickly; long or
    /// scheduled playback is HapticScheduler's responsibility, not the
    /// backend's. Returns whether the effect was actually accepted by the
    /// device.
    virtual HapticBackendResult SendEffect(const HapticEffect& effect) = 0;

    /// Stop any effect currently playing and return the backend to a
    /// neutral (zero-intensity) state. Returns whether the device
    /// acknowledged the reset.
    virtual HapticBackendResult Reset() = 0;

    /// Whether the backend is currently able to accept effects.
    virtual bool IsConnected() const = 0;
};

} // namespace sekiro_haptics
