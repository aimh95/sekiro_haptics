#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics {

/// Outcome of a transport-level operation against a HID device.
enum class TransportResult {
    Success,
    NotFound,
    OpenFailed,
    WriteFailed,
    NotOpen,
};

/// Returns a human-readable name for a TransportResult, e.g. for logging.
const char* ToString(TransportResult result);

/// Identifying information for one HID device discovered during
/// enumeration. This is transport-level metadata (what the OS/HID stack
/// reports about the device) -- it says nothing about DualSense's wire
/// protocol.
struct HidDeviceInfo {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;

    /// OS-specific device path, as reported by the HID backend. Pass this
    /// to IDualSenseTransport::Open() to open this specific device.
    std::string path;

    std::wstring product;
    std::wstring serialNumber;
};

/// Abstraction over raw USB HID access to a DualSense controller.
///
/// IDualSenseTransport owns HID device discovery and device lifetime:
/// enumerating candidate devices, opening/closing a specific device by
/// path, and writing raw output-report bytes. It has no knowledge of what
/// those bytes mean -- DualSense output-report/packet construction is a
/// higher layer's responsibility (a future DualSenseBackend), not the
/// transport's. USB only; Bluetooth is out of scope.
class IDualSenseTransport {
public:
    virtual ~IDualSenseTransport() = default;

    /// Enumerate connected HID devices and return the subset that look
    /// like a Sony DualSense (matched by vendor/product id). Safe to call
    /// repeatedly; does not affect any currently-open device.
    virtual std::vector<HidDeviceInfo> EnumerateCandidates() = 0;

    /// Open a specific HID device by its OS path (as returned by
    /// EnumerateCandidates). Closes any currently-open device first.
    virtual TransportResult Open(const std::string& path) = 0;

    /// Close the currently-open device, if any. Safe to call when nothing
    /// is open, and safe to call more than once.
    virtual void Close() = 0;

    /// Whether a device is currently open.
    virtual bool IsOpen() const = 0;

    /// Write a raw HID output report to the open device. `report` is
    /// caller-constructed; the transport does not interpret, validate, or
    /// construct its contents -- that is DualSense packet-format logic and
    /// belongs outside the transport.
    virtual TransportResult WriteOutputReport(const std::uint8_t* report, std::size_t length) = 0;
};

} // namespace sekiro_haptics
