#include "sekiro_haptics/MockHapticBackend.hpp"
#include "sekiro_haptics/Presets.hpp"
#include "testing.hpp"

#include <chrono>
#include <sstream>

using namespace sekiro_haptics;
using namespace std::chrono_literals;

SH_TEST(MockHapticBackend_SendEffect_RecordsHistory) {
    std::ostringstream log;
    MockHapticBackend backend(log);

    HapticBackendResult result = backend.SendEffect(presets::PerfectDeflect());

    SH_CHECK(result == HapticBackendResult::Success);
    auto history = backend.History();
    SH_CHECK(history.size() == 1);
    SH_CHECK(history[0].type == HapticEffectType::PerfectDeflect);
}

SH_TEST(MockHapticBackend_SendEffect_LogsEffect) {
    std::ostringstream log;
    MockHapticBackend backend(log);

    backend.SendEffect(presets::PerfectDeflect());

    SH_CHECK(log.str().find("PerfectDeflect") != std::string::npos);
}

SH_TEST(MockHapticBackend_Reset_IncrementsResetCountWithoutClearingHistory) {
    std::ostringstream log;
    MockHapticBackend backend(log);

    backend.SendEffect(presets::PerfectDeflect());
    HapticBackendResult result = backend.Reset();
    backend.Reset();

    SH_CHECK(result == HapticBackendResult::Success);
    SH_CHECK(backend.ResetCount() == 2);
    SH_CHECK(backend.History().size() == 1);
}

SH_TEST(MockHapticBackend_IsConnected_IsAlwaysTrue) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    SH_CHECK(backend.IsConnected());
}

SH_TEST(MockHapticBackend_WaitForEffectCount_ReturnsFalseOnTimeoutWhenNothingArrives) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    SH_CHECK(backend.WaitForEffectCount(1, 20ms) == false);
}

SH_TEST(MockHapticBackend_WaitForResetCount_ReturnsTrueOnceResetCalled) {
    std::ostringstream log;
    MockHapticBackend backend(log);

    SH_CHECK(backend.WaitForResetCount(1, 20ms) == false);
    backend.Reset();
    SH_CHECK(backend.WaitForResetCount(1, 1s));
}
