// Console test application for SekiroHaptics.
//
// This does not touch any real controller or hook into the game. It wires
// a MockHapticBackend to a HapticScheduler and triggers a PerfectDeflect
// effect, the way a future Sekiro event source eventually would.

#include "sekiro_haptics/HapticScheduler.hpp"
#include "sekiro_haptics/MockHapticBackend.hpp"
#include "sekiro_haptics/Presets.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace sekiro_haptics;
    using namespace std::chrono_literals;

    std::cout << "SekiroHaptics console test\n";
    std::cout << "Backend: MockHapticBackend (no real hardware access)\n\n";

    MockHapticBackend backend;
    HapticScheduler scheduler(backend);

    std::cout << "Triggering PerfectDeflect...\n";
    scheduler.Schedule(presets::PerfectDeflect());

    // The scheduler dispatches from a background thread; wait for the
    // effect to actually reach the backend before printing the summary.
    backend.WaitForEffectCount(1, 1s);

    std::cout << "\nEffects received by backend: " << backend.History().size() << '\n';
    std::cout << "Done.\n";
    return 0;
}
