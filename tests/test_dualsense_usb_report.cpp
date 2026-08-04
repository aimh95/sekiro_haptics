// Pure data tests for DualSense USB output report construction. No HID/USB
// access happens here -- this only checks the bytes BuildRumbleReport()
// produces, independent of any real hardware.

#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "testing.hpp"

using namespace sekiro_haptics::dualsense_protocol;

SH_TEST(BuildRumbleReport_HasCorrectLengthAndReportId) {
    auto report = BuildRumbleReport(0, 0);

    SH_CHECK(report.size() == kUsbOutputReportLength);
    SH_CHECK(report[0] == kUsbOutputReportId);
}

SH_TEST(BuildRumbleReport_SetsMotorBytesAtDocumentedOffsets) {
    auto report = BuildRumbleReport(/*leftMotor=*/12, /*rightMotor=*/200);

    SH_CHECK(report[3] == 200); // right motor
    SH_CHECK(report[4] == 12);  // left motor
}

SH_TEST(BuildRumbleReport_SetsMainMotorEnableFlags) {
    auto report = BuildRumbleReport(100, 100);

    SH_CHECK((report[1] & 0x01) != 0);
    SH_CHECK((report[1] & 0x02) != 0);
}

SH_TEST(BuildRumbleReport_LeavesUnrelatedBytesZeroed) {
    // Only the report id, flag byte, and the two motor bytes should be
    // non-zero; everything else (audio, trigger, LED fields) must stay
    // untouched by a rumble-only report.
    auto report = BuildRumbleReport(255, 255);

    for (std::size_t i = 0; i < report.size(); ++i) {
        if (i == 0 || i == 1 || i == 3 || i == 4) {
            continue;
        }
        SH_CHECK(report[i] == 0);
    }
}

SH_TEST(BuildRumbleReport_ZeroMotors_ProducesAllZeroMotorBytes) {
    auto report = BuildRumbleReport(0, 0);

    SH_CHECK(report[3] == 0);
    SH_CHECK(report[4] == 0);
}
