// Unit tests for DualSenseLegacyBackend: intensity->motor-byte conversion,
// raw report bytes (reusing BuildRumbleReport, never reimplementing the HID
// spec), and lifecycle (connect/disconnect, write failure, repeated
// stop/reset, shutdown). All against FakeDualSenseTransport -- no real
// hardware, no HIDAPI dependency.

#include "sekiro_haptics/DualSenseLegacyBackend.hpp"
#include "sekiro_haptics/DualSenseUsbReport.hpp"
#include "testing.hpp"

#include "FakeDualSenseTransport.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace sekiro_haptics;

namespace {

HapticEffect Effect(float left, float right, HapticEffectType type = HapticEffectType::Generic) {
    HapticEffect effect;
    effect.type = type;
    effect.intensity.left = left;
    effect.intensity.right = right;
    return effect;
}

} // namespace

// --- ToMotorByte conversion rule ---

SH_TEST(DualSenseLegacyBackend_ToMotorByte_ZeroIntensity_ReturnsZero) {
    SH_CHECK(DualSenseLegacyBackend::ToMotorByte(0.0f) == 0);
}

SH_TEST(DualSenseLegacyBackend_ToMotorByte_FullIntensity_Returns255) {
    SH_CHECK(DualSenseLegacyBackend::ToMotorByte(1.0f) == 255);
}

SH_TEST(DualSenseLegacyBackend_ToMotorByte_HalfIntensity_Returns128) {
    // 0.5 * 255 = 127.5, rounded half-away-from-zero -> 128.
    SH_CHECK(DualSenseLegacyBackend::ToMotorByte(0.5f) == 128);
}

SH_TEST(DualSenseLegacyBackend_ToMotorByte_NegativeInput_ClampsToZero) {
    SH_CHECK(DualSenseLegacyBackend::ToMotorByte(-0.5f) == 0);
}

SH_TEST(DualSenseLegacyBackend_ToMotorByte_AboveOneInput_ClampsTo255) {
    SH_CHECK(DualSenseLegacyBackend::ToMotorByte(1.5f) == 255);
}

// --- SendEffect: raw report bytes ---

SH_TEST(DualSenseLegacyBackend_SendEffect_LeftOnly_WritesCorrectRawBytes) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    HapticBackendResult result = backend.SendEffect(Effect(1.0f, 0.0f));

    SH_CHECK(result == HapticBackendResult::Success);
    auto reports = transport.WrittenReports();
    SH_CHECK(reports.size() == 1);
    SH_CHECK(reports[0][4] == 255); // left motor offset
    SH_CHECK(reports[0][3] == 0);   // right motor offset
}

SH_TEST(DualSenseLegacyBackend_SendEffect_RightOnly_WritesCorrectRawBytes) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    backend.SendEffect(Effect(0.0f, 1.0f));

    auto reports = transport.WrittenReports();
    SH_CHECK(reports[0][3] == 255);
    SH_CHECK(reports[0][4] == 0);
}

SH_TEST(DualSenseLegacyBackend_SendEffect_BothMotors_WritesCorrectRawBytes) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    backend.SendEffect(Effect(0.5f, 1.0f));

    auto reports = transport.WrittenReports();
    SH_CHECK(reports[0][4] == 128);
    SH_CHECK(reports[0][3] == 255);
}

SH_TEST(DualSenseLegacyBackend_SendEffect_ZeroIntensity_WritesZeroMotorReport) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    HapticBackendResult result = backend.SendEffect(Effect(0.0f, 0.0f));

    SH_CHECK(result == HapticBackendResult::Success);
    auto reports = transport.WrittenReports();
    SH_CHECK(reports.size() == 1);
    SH_CHECK(reports[0][3] == 0);
    SH_CHECK(reports[0][4] == 0);
}

SH_TEST(DualSenseLegacyBackend_SendEffect_ReportMatchesBuildRumbleReport) {
    // Proves reuse, not reimplementation, of the HID report spec.
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    backend.SendEffect(Effect(0.5f, 0.25f));

    auto expected = dualsense_protocol::BuildRumbleReport(DualSenseLegacyBackend::ToMotorByte(0.5f),
                                                            DualSenseLegacyBackend::ToMotorByte(0.25f));
    auto actual = transport.WrittenReports()[0];
    SH_CHECK(actual.size() == expected.size());
    SH_CHECK(std::equal(expected.begin(), expected.end(), actual.begin()));
}

SH_TEST(DualSenseLegacyBackend_SendEffect_AllHapticEffectTypes_OnlyIntensityAffectsReport) {
    // DualSenseLegacyBackend never branches on HapticEffect::type -- every
    // type produces byte-identical output for the same intensity. This is
    // the closest testable proxy for "PCM/adaptive-trigger/speaker are
    // never pretended to be supported": there is no per-type code path
    // that could silently diverge into something other than legacy
    // rumble, because none of HapticEffectType's values are even consulted.
    const HapticEffectType types[] = {HapticEffectType::Generic, HapticEffectType::PerfectDeflect,
                                       HapticEffectType::PostureBreak, HapticEffectType::TakeDamage,
                                       HapticEffectType::Deathblow};

    std::vector<std::uint8_t> firstReport;
    for (HapticEffectType type : types) {
        FakeDualSenseTransport transport;
        transport.SetOpen(true);
        DualSenseLegacyBackend backend(transport);
        backend.SendEffect(Effect(0.75f, 0.3f, type));

        auto report = transport.WrittenReports()[0];
        if (firstReport.empty()) {
            firstReport = report;
        } else {
            SH_CHECK(report == firstReport);
        }
    }
}

// --- Connection / failure handling ---

SH_TEST(DualSenseLegacyBackend_SendEffect_WhenNotConnected_ReturnsNotConnectedAndWritesNothing) {
    FakeDualSenseTransport transport;
    transport.SetOpen(false);
    DualSenseLegacyBackend backend(transport);

    HapticBackendResult result = backend.SendEffect(Effect(1.0f, 1.0f));

    SH_CHECK(result == HapticBackendResult::NotConnected);
    SH_CHECK(transport.WriteCount() == 0);
}

SH_TEST(DualSenseLegacyBackend_SendEffect_WriteFailure_ReturnsDeviceErrorAndDoesNotMarkVibrating) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    transport.FailNextWrite();

    {
        DualSenseLegacyBackend backend(transport);
        HapticBackendResult result = backend.SendEffect(Effect(1.0f, 1.0f));

        SH_CHECK(result == HapticBackendResult::DeviceError);
        SH_CHECK(transport.WriteCount() == 1);   // the failed attempt itself
        SH_CHECK(transport.WrittenReports().empty()); // but nothing "landed"
    }
    // Destructor must NOT have attempted a stop -- the failed send was
    // never recorded as having left the controller vibrating.
    SH_CHECK(transport.WriteCount() == 1);
}

SH_TEST(DualSenseLegacyBackend_Reset_WhenNotConnected_ReturnsNotConnected) {
    FakeDualSenseTransport transport;
    transport.SetOpen(false);
    DualSenseLegacyBackend backend(transport);

    SH_CHECK(backend.Reset() == HapticBackendResult::NotConnected);
    SH_CHECK(transport.WriteCount() == 0);
}

SH_TEST(DualSenseLegacyBackend_Reset_WriteFailure_ReturnsDeviceError) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    transport.SetAlwaysFailWrites(true);
    DualSenseLegacyBackend backend(transport);

    SH_CHECK(backend.Reset() == HapticBackendResult::DeviceError);
}

SH_TEST(DualSenseLegacyBackend_Reset_CalledRepeatedly_EachCallWritesZeroReportSafely) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    DualSenseLegacyBackend backend(transport);

    SH_CHECK(backend.Reset() == HapticBackendResult::Success);
    SH_CHECK(backend.Reset() == HapticBackendResult::Success);
    SH_CHECK(backend.Reset() == HapticBackendResult::Success);

    SH_CHECK(transport.WriteCount() == 3);
    for (const auto& report : transport.WrittenReports()) {
        SH_CHECK(report[3] == 0);
        SH_CHECK(report[4] == 0);
    }
}

SH_TEST(DualSenseLegacyBackend_IsConnected_ReflectsTransportOpenState) {
    FakeDualSenseTransport transport;
    DualSenseLegacyBackend backend(transport);

    transport.SetOpen(false);
    SH_CHECK(backend.IsConnected() == false);

    transport.SetOpen(true);
    SH_CHECK(backend.IsConnected());
}

// --- Shutdown / destructor best-effort stop ---

SH_TEST(DualSenseLegacyBackend_Shutdown_NoActiveEffect_SendsNoStopReport) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    {
        DualSenseLegacyBackend backend(transport);
        // Never sent an effect.
    }
    SH_CHECK(transport.WriteCount() == 0);
}

SH_TEST(DualSenseLegacyBackend_Shutdown_AfterZeroIntensityEffect_SendsNoStopReport) {
    // A zero-intensity SendEffect is still a successful write, but it never
    // left the controller "vibrating" -- no stop needed on shutdown.
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    {
        DualSenseLegacyBackend backend(transport);
        backend.SendEffect(Effect(0.0f, 0.0f));
    }
    SH_CHECK(transport.WriteCount() == 1);
}

SH_TEST(DualSenseLegacyBackend_Shutdown_ActiveEffect_SendsBestEffortStopReport) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    {
        DualSenseLegacyBackend backend(transport);
        backend.SendEffect(Effect(1.0f, 1.0f));
        SH_CHECK(transport.WriteCount() == 1);
    }
    // Destructor sent exactly one more report: the stop.
    SH_CHECK(transport.WriteCount() == 2);
    auto stopReport = transport.WrittenReports()[1];
    SH_CHECK(stopReport[3] == 0);
    SH_CHECK(stopReport[4] == 0);
}

SH_TEST(DualSenseLegacyBackend_Shutdown_AfterExplicitReset_SendsNoAdditionalStopReport) {
    FakeDualSenseTransport transport;
    transport.SetOpen(true);
    {
        DualSenseLegacyBackend backend(transport);
        backend.SendEffect(Effect(1.0f, 1.0f));
        backend.Reset();
        SH_CHECK(transport.WriteCount() == 2);
    }
    // Already stopped explicitly -- destructor has nothing to do.
    SH_CHECK(transport.WriteCount() == 2);
}
