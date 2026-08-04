// Manual hardware test: makes a real, USB-connected DualSense rumble for a
// couple of seconds so a human can confirm the transport + packet-building
// pieces actually work end to end. Requires a DualSense to be plugged in
// over USB; this is not part of the automated test suite because "did it
// buzz" is not something a unit test can assert.
//
// Usage: sekiro_haptics_dualsense_rumble_test [motorStrength(0-255)]
// Defaults to 180. Pass a low value (e.g. 15-30) to feel a subtle/fine
// vibration instead of full strength.

#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "sekiro_haptics/HidApiDualSenseTransport.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    using namespace sekiro_haptics;
    using namespace std::chrono_literals;

    int motorStrength = 180;
    if (argc > 1) {
        std::from_chars(argv[1], argv[1] + std::strlen(argv[1]), motorStrength);
    }
    auto motor = static_cast<std::uint8_t>(std::clamp(motorStrength, 0, 255));

    std::cout << "SekiroHaptics DualSense rumble test (USB only), motor strength=" << static_cast<int>(motor)
              << "/255\n";

    HidApiDualSenseTransport transport;

    auto candidates = transport.EnumerateCandidates();
    if (candidates.empty()) {
        std::cout << "No DualSense found over USB. Plug one in and try again.\n";
        return 1;
    }

    if (transport.Open(candidates.front().path) != TransportResult::Success) {
        std::cout << "Failed to open DualSense.\n";
        return 1;
    }

    std::cout << "Opened DualSense. Rumbling for 1.5s...\n";

    auto rumble = dualsense_protocol::BuildRumbleReport(/*leftMotor=*/motor, /*rightMotor=*/motor);
    auto stop = dualsense_protocol::BuildRumbleReport(/*leftMotor=*/0, /*rightMotor=*/0);

    // The DualSense will let rumble motors time out if not refreshed, so
    // resend the same report periodically for the test's duration.
    const auto testDuration = 1500ms;
    const auto resendInterval = 50ms;
    for (auto elapsed = 0ms; elapsed < testDuration; elapsed += resendInterval) {
        transport.WriteOutputReport(rumble.data(), rumble.size());
        std::this_thread::sleep_for(resendInterval);
    }

    transport.WriteOutputReport(stop.data(), stop.size());
    transport.Close();

    std::cout << "Done.\n";
    return 0;
}
