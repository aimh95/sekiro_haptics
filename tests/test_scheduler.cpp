#include "sekiro_haptics/HapticScheduler.hpp"
#include "sekiro_haptics/MockHapticBackend.hpp"
#include "sekiro_haptics/Presets.hpp"
#include "testing.hpp"

#include <chrono>
#include <sstream>

using namespace sekiro_haptics;
using namespace std::chrono_literals;

SH_TEST(HapticScheduler_Schedule_DispatchesEffectToBackend) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    HapticScheduler scheduler(backend);

    scheduler.Schedule(presets::PerfectDeflect());

    SH_CHECK(backend.WaitForEffectCount(1, 1s));
    SH_CHECK(backend.History().size() == 1);
}

SH_TEST(HapticScheduler_Schedule_DispatchesTwoEffectsInDelayOrder) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    HapticScheduler scheduler(backend);

    HapticEffect first = presets::PerfectDeflect();
    first.debugLabel = "first";
    HapticEffect second = presets::PerfectDeflect();
    second.debugLabel = "second";

    scheduler.Schedule(second, 100ms);
    scheduler.Schedule(first, 10ms);

    SH_CHECK(backend.WaitForEffectCount(2, 1s));
    auto history = backend.History();
    SH_CHECK(history.size() == 2);
    SH_CHECK(history[0].debugLabel == "first");
    SH_CHECK(history[1].debugLabel == "second");
}

SH_TEST(HapticScheduler_Cancel_PreventsDispatch) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    HapticScheduler scheduler(backend);

    HapticEffectId id = scheduler.Schedule(presets::PerfectDeflect(), 150ms);
    bool cancelled = scheduler.Cancel(id);

    SH_CHECK(cancelled);
    SH_CHECK(backend.WaitForEffectCount(1, 300ms) == false);
    SH_CHECK(backend.History().empty());
}

SH_TEST(HapticScheduler_Cancel_ReturnsFalseForUnknownId) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    HapticScheduler scheduler(backend);

    SH_CHECK(scheduler.Cancel(999999) == false);
}

SH_TEST(HapticScheduler_Reset_ClearsPendingEffectsAndResetsBackend) {
    std::ostringstream log;
    MockHapticBackend backend(log);
    HapticScheduler scheduler(backend);

    scheduler.Schedule(presets::PerfectDeflect(), 200ms);
    scheduler.Reset();

    SH_CHECK(backend.ResetCount() == 1);
    SH_CHECK(backend.WaitForEffectCount(1, 350ms) == false);
    SH_CHECK(backend.History().empty());
}
