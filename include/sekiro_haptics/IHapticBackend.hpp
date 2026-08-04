#pragma once

#include "sekiro_haptics/HapticEffect.hpp"

namespace sekiro_haptics {

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
    /// backend's.
    virtual void SendEffect(const HapticEffect& effect) = 0;

    /// Stop any effect currently playing and return the backend to a
    /// neutral (zero-intensity) state.
    virtual void Reset() = 0;

    /// Whether the backend is currently able to accept effects.
    virtual bool IsConnected() const = 0;
};

} // namespace sekiro_haptics
