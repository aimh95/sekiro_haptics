// Tests for HidApiDualSenseTransport that must pass with or without a real
// DualSense plugged in. They exercise the transport's error paths and
// lifetime safety, not real hardware I/O -- there is no assertion here that
// requires a controller to be connected.

#include "sekiro_haptics/HidApiDualSenseTransport.hpp"
#include "testing.hpp"

#include <sstream>

using namespace sekiro_haptics;

SH_TEST(HidApiDualSenseTransport_Construct_IsNotOpenInitially) {
    std::ostringstream log;
    HidApiDualSenseTransport transport(log);

    SH_CHECK(transport.IsOpen() == false);
}

SH_TEST(HidApiDualSenseTransport_Open_InvalidPath_ReturnsOpenFailedAndLogs) {
    std::ostringstream log;
    HidApiDualSenseTransport transport(log);

    TransportResult result = transport.Open("this-is-not-a-real-hid-device-path");

    SH_CHECK(result == TransportResult::OpenFailed);
    SH_CHECK(transport.IsOpen() == false);
    SH_CHECK(log.str().find("OpenFailed") != std::string::npos);
}

SH_TEST(HidApiDualSenseTransport_Close_WhenNothingOpen_IsSafeAndIdempotent) {
    std::ostringstream log;
    HidApiDualSenseTransport transport(log);

    transport.Close();
    transport.Close();

    SH_CHECK(transport.IsOpen() == false);
}

SH_TEST(HidApiDualSenseTransport_WriteOutputReport_WhenNotOpen_ReturnsNotOpen) {
    std::ostringstream log;
    HidApiDualSenseTransport transport(log);

    std::uint8_t report[8] = {0};
    TransportResult result = transport.WriteOutputReport(report, sizeof(report));

    SH_CHECK(result == TransportResult::NotOpen);
}

SH_TEST(HidApiDualSenseTransport_EnumerateCandidates_DoesNotCrashRegardlessOfHardware) {
    std::ostringstream log;
    HidApiDualSenseTransport transport(log);

    // On a machine with no DualSense attached this is legitimately empty;
    // the point of this test is that enumeration completes without
    // crashing. If a real DualSense happens to be attached, every result
    // must actually be one (the Sony vendor id), proving the vendor/product
    // filter passed to hid_enumerate() is doing its job.
    std::vector<HidDeviceInfo> candidates = transport.EnumerateCandidates();
    for (const HidDeviceInfo& candidate : candidates) {
        SH_CHECK(candidate.vendorId == 0x054C);
    }
}

SH_TEST(HidApiDualSenseTransport_MultipleInstances_ConstructAndDestructSafely) {
    // Exercises the reference-counted hid_init()/hid_exit() lifecycle:
    // overlapping and sequential instances must not double-free or crash
    // HIDAPI's global state, including at the point the last one is
    // destroyed.
    std::ostringstream log;
    {
        HidApiDualSenseTransport first(log);
        HidApiDualSenseTransport second(log);
        SH_CHECK(first.IsOpen() == false);
        SH_CHECK(second.IsOpen() == false);
    }

    HidApiDualSenseTransport third(log);
    SH_CHECK(third.IsOpen() == false);
}

SH_TEST(HidApiDualSenseTransport_DestructorAfterFailedOpen_DoesNotCrash) {
    std::ostringstream log;
    {
        HidApiDualSenseTransport transport(log);
        transport.Open("this-is-not-a-real-hid-device-path");
    }
    // Reaching this line without crashing is the assertion.
    SH_CHECK(true);
}
