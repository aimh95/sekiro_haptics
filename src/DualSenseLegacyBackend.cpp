#include "sekiro_haptics/DualSenseLegacyBackend.hpp"

#include "sekiro_haptics/DualSenseUsbReport.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace sekiro_haptics {

std::ostream& DualSenseLegacyBackend::DefaultLogStream() {
    return std::cout;
}

DualSenseLegacyBackend::DualSenseLegacyBackend(IDualSenseTransport& transport, std::ostream& log)
    : transport_(transport), log_(log) {}

DualSenseLegacyBackend::~DualSenseLegacyBackend() {
    if (vibrating_.load()) {
        Reset(); // best-effort; nothing meaningful to do with a failure here
    }
}

std::uint8_t DualSenseLegacyBackend::ToMotorByte(float normalizedIntensity) {
    float clamped = std::clamp(normalizedIntensity, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

HapticBackendResult DualSenseLegacyBackend::WriteMotorReport(std::uint8_t leftMotor, std::uint8_t rightMotor,
                                                               const char* logVerb) {
    if (!transport_.IsOpen()) {
        log_ << "[DualSenseLegacyBackend] " << logVerb << " result=" << ToString(HapticBackendResult::NotConnected)
             << '\n';
        return HapticBackendResult::NotConnected;
    }

    auto report = dualsense_protocol::BuildRumbleReport(leftMotor, rightMotor);
    TransportResult result = transport_.WriteOutputReport(report.data(), report.size());
    HapticBackendResult backendResult =
        result == TransportResult::Success ? HapticBackendResult::Success : HapticBackendResult::DeviceError;

    log_ << "[DualSenseLegacyBackend] " << logVerb << " left=" << static_cast<int>(leftMotor)
         << " right=" << static_cast<int>(rightMotor) << " result=" << ToString(backendResult) << '\n';

    if (backendResult != HapticBackendResult::Success) {
        return backendResult;
    }

    vibrating_.store(leftMotor != 0 || rightMotor != 0);
    return HapticBackendResult::Success;
}

HapticBackendResult DualSenseLegacyBackend::SendEffect(const HapticEffect& effect) {
    std::uint8_t left = ToMotorByte(effect.intensity.left);
    std::uint8_t right = ToMotorByte(effect.intensity.right);
    return WriteMotorReport(left, right, "Effect dispatched");
}

HapticBackendResult DualSenseLegacyBackend::Reset() {
    return WriteMotorReport(0, 0, "Stop requested");
}

bool DualSenseLegacyBackend::IsConnected() const {
    return transport_.IsOpen();
}

} // namespace sekiro_haptics
