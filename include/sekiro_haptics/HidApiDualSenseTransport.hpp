#pragma once

#include "sekiro_haptics/IDualSenseTransport.hpp"

#include <cstddef>
#include <ostream>
#include <string>

// Forward declaration of HIDAPI's opaque device handle so this header does
// not need to include <hidapi.h> (and pull HIDAPI's macros/typedefs into
// every translation unit that includes this file).
struct hid_device_;

namespace sekiro_haptics {

/// USB HID transport for a real DualSense controller, built on HIDAPI
/// (https://github.com/libusb/hidapi).
///
/// This is a transport spike: it owns HID device discovery and device
/// lifetime (enumerate, open, close) and can write a raw output report.
/// It knows nothing about what a DualSense output report should contain --
/// callers pass already-constructed report bytes to WriteOutputReport().
/// USB only; Bluetooth is not implemented.
///
/// Every operation logs vendor id, product id, path, and the operation's
/// result to an injectable std::ostream (defaults to std::cout), so a real
/// connection attempt is observable without a debugger.
///
/// HIDAPI's global hid_init()/hid_exit() are reference-counted across all
/// live instances of this class, so construction/destruction is safe to
/// interleave across multiple transports, and the underlying HID handle is
/// always closed before the library is torn down.
class HidApiDualSenseTransport final : public IDualSenseTransport {
public:
    explicit HidApiDualSenseTransport(std::ostream& log = DefaultLogStream());
    ~HidApiDualSenseTransport() override;

    HidApiDualSenseTransport(const HidApiDualSenseTransport&) = delete;
    HidApiDualSenseTransport& operator=(const HidApiDualSenseTransport&) = delete;

    std::vector<HidDeviceInfo> EnumerateCandidates() override;
    TransportResult Open(const std::string& path) override;
    void Close() override;
    bool IsOpen() const override;
    TransportResult WriteOutputReport(const std::uint8_t* report, std::size_t length) override;

    /// What the open device's HID descriptor declares an output report must be,
    /// or 0 when nothing is open / it could not be read.
    ///
    /// This is queried rather than assumed. The controller on the development
    /// machine declares 48, while this project's report builders produce a
    /// 64-byte buffer -- writing 64 bytes to it never succeeded, which is why
    /// no audio-routing report ever reached the device. WriteOutputReport()
    /// now sends exactly this many bytes.
    std::size_t DeclaredOutputReportLength() const { return declaredOutputLength_; }

private:
    static std::ostream& DefaultLogStream();
    static std::size_t QueryOutputReportLength(const std::string& path);

    std::ostream& log_;
    hid_device_* device_ = nullptr;
    std::string openPath_;
    std::size_t declaredOutputLength_ = 0;
};

} // namespace sekiro_haptics
