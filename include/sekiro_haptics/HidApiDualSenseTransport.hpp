#pragma once

#include "sekiro_haptics/IDualSenseTransport.hpp"

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

private:
    static std::ostream& DefaultLogStream();

    std::ostream& log_;
    hid_device_* device_ = nullptr;
    std::string openPath_;
};

} // namespace sekiro_haptics
