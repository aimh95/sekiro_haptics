#include "sekiro_haptics/HidApiDualSenseTransport.hpp"

#include <hidapi.h>

#include <iostream>
#include <mutex>

namespace sekiro_haptics {

namespace {

// Sony's USB-IF vendor id and the DualSense's USB product id. These are
// public device identifiers (the same values used to match the DualSense
// in, e.g., the Linux kernel's hid-playstation driver), not part of any
// DualSense wire protocol -- no HID report format is encoded here.
constexpr std::uint16_t kSonyVendorId = 0x054C;
constexpr std::uint16_t kDualSenseUsbProductId = 0x0CE6;

// hid_init()/hid_exit() are process-global. Reference-count them across all
// HidApiDualSenseTransport instances so one instance's destructor can't tear
// down HIDAPI out from under another still-live instance.
std::mutex& HidLifecycleMutex() {
    static std::mutex mutex;
    return mutex;
}

int& HidLifecycleRefCount() {
    static int count = 0;
    return count;
}

void AcquireHidApi() {
    std::lock_guard<std::mutex> lock(HidLifecycleMutex());
    if (HidLifecycleRefCount()++ == 0) {
        hid_init();
    }
}

void ReleaseHidApi() {
    std::lock_guard<std::mutex> lock(HidLifecycleMutex());
    if (--HidLifecycleRefCount() == 0) {
        hid_exit();
    }
}

std::string NarrowOrEmpty(const char* s) {
    return s != nullptr ? std::string(s) : std::string();
}

std::wstring WideOrEmpty(const wchar_t* s) {
    return s != nullptr ? std::wstring(s) : std::wstring();
}

} // namespace

std::ostream& HidApiDualSenseTransport::DefaultLogStream() {
    return std::cout;
}

HidApiDualSenseTransport::HidApiDualSenseTransport(std::ostream& log) : log_(log) {
    AcquireHidApi();
}

HidApiDualSenseTransport::~HidApiDualSenseTransport() {
    Close();
    ReleaseHidApi();
}

std::vector<HidDeviceInfo> HidApiDualSenseTransport::EnumerateCandidates() {
    std::vector<HidDeviceInfo> candidates;

    hid_device_info* list = hid_enumerate(kSonyVendorId, kDualSenseUsbProductId);
    for (hid_device_info* cur = list; cur != nullptr; cur = cur->next) {
        HidDeviceInfo info;
        info.vendorId = cur->vendor_id;
        info.productId = cur->product_id;
        info.path = NarrowOrEmpty(cur->path);
        info.product = WideOrEmpty(cur->product_string);
        info.serialNumber = WideOrEmpty(cur->serial_number);

        log_ << "[HidApiDualSenseTransport] Candidate found vendorId=0x" << std::hex << info.vendorId
             << " productId=0x" << info.productId << std::dec << " path=" << info.path << '\n';

        candidates.push_back(std::move(info));
    }
    hid_free_enumeration(list);

    return candidates;
}

TransportResult HidApiDualSenseTransport::Open(const std::string& path) {
    Close();

    hid_device* handle = hid_open_path(path.c_str());
    if (handle == nullptr) {
        log_ << "[HidApiDualSenseTransport] Open path=" << path
             << " result=" << ToString(TransportResult::OpenFailed) << '\n';
        return TransportResult::OpenFailed;
    }

    device_ = handle;
    openPath_ = path;
    log_ << "[HidApiDualSenseTransport] Open path=" << path
         << " result=" << ToString(TransportResult::Success) << '\n';
    return TransportResult::Success;
}

void HidApiDualSenseTransport::Close() {
    if (device_ == nullptr) {
        return;
    }

    hid_close(device_);
    log_ << "[HidApiDualSenseTransport] Close path=" << openPath_ << '\n';
    device_ = nullptr;
    openPath_.clear();
}

bool HidApiDualSenseTransport::IsOpen() const {
    return device_ != nullptr;
}

TransportResult HidApiDualSenseTransport::WriteOutputReport(const std::uint8_t* report, std::size_t length) {
    if (device_ == nullptr) {
        log_ << "[HidApiDualSenseTransport] WriteOutputReport result=" << ToString(TransportResult::NotOpen) << '\n';
        return TransportResult::NotOpen;
    }

    int written = hid_write(device_, report, length);
    TransportResult result = written >= 0 ? TransportResult::Success : TransportResult::WriteFailed;

    log_ << "[HidApiDualSenseTransport] WriteOutputReport path=" << openPath_ << " bytes=" << length
         << " result=" << ToString(result) << '\n';

    return result;
}

} // namespace sekiro_haptics
