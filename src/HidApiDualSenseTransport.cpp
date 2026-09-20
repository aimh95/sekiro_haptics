#include "sekiro_haptics/HidApiDualSenseTransport.hpp"

#include <hidapi.h>

#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#endif

#include <cstring>
#include <vector>
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
        if (cur->bus_type != HID_API_BUS_USB || cur->usage_page != 0x01 || cur->usage != 0x05) continue;
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

    const auto* info = hid_get_device_info(handle);
    if (!info || info->vendor_id != kSonyVendorId || info->product_id != kDualSenseUsbProductId ||
        info->bus_type != HID_API_BUS_USB || info->usage_page != 0x01 || info->usage != 0x05) {
        hid_close(handle);
        log_ << "[HidApiDualSenseTransport] Expected USB DualSense gamepad interface\n";
        return TransportResult::OpenFailed;
    }

    device_ = handle;
    openPath_ = path;
    declaredOutputLength_ = QueryOutputReportLength(path);
    log_ << "[HidApiDualSenseTransport] Open path=" << path
         << " outputReportLength=" << declaredOutputLength_
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
    declaredOutputLength_ = 0;
}

/// Asks the HID stack how long an output report must be for this device. A
/// write of any other length is rejected, and hid_write() reports that the
/// same way it reports "someone else owns the device" -- so this has to be
/// measured, not assumed.
std::size_t HidApiDualSenseTransport::QueryOutputReportLength(const std::string& path) {
#ifdef _WIN32
    const HANDLE file = CreateFileA(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    std::size_t length = 0;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (HidD_GetPreparsedData(file, &preparsed)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
            length = caps.OutputReportByteLength;
        HidD_FreePreparsedData(preparsed);
    }
    CloseHandle(file);
    return length;
#else
    (void)path;
    return 0;
#endif
}

bool HidApiDualSenseTransport::IsOpen() const {
    return device_ != nullptr;
}

TransportResult HidApiDualSenseTransport::WriteOutputReport(const std::uint8_t* report, std::size_t length) {
    if (device_ == nullptr) {
        log_ << "[HidApiDualSenseTransport] WriteOutputReport result=" << ToString(TransportResult::NotOpen) << '\n';
        return TransportResult::NotOpen;
    }

    // Send exactly what the descriptor declares. The builders in
    // DualSenseUsbReport.hpp produce a fixed 64-byte buffer, but this
    // controller declares 48 -- writing the buffer's own size silently failed
    // every time. Padding is zero, which is "change nothing" for every field.
    std::vector<std::uint8_t> padded;
    const std::uint8_t* data = report;
    std::size_t toWrite = length;
    if (declaredOutputLength_ != 0 && declaredOutputLength_ != length) {
        padded.assign(declaredOutputLength_, 0);
        std::memcpy(padded.data(), report, std::min(length, declaredOutputLength_));
        data = padded.data();
        toWrite = declaredOutputLength_;
    }

    int written = hid_write(device_, data, toWrite);
    TransportResult result = written >= 0 && static_cast<std::size_t>(written) == toWrite
        ? TransportResult::Success : TransportResult::WriteFailed;

    log_ << "[HidApiDualSenseTransport] WriteOutputReport path=" << openPath_
         << " requested=" << length << " sent=" << toWrite
         << " written=" << written << " result=" << ToString(result);
    if (result != TransportResult::Success) {
        // Without the backend's own message a failed write is unactionable: a
        // wrong report length, another process holding the device, and a
        // permissions problem all look identical from the return value alone.
        if (const wchar_t* why = hid_error(device_)) {
            log_ << " error=\"";
            for (const wchar_t* c = why; *c; ++c) log_ << static_cast<char>(*c < 128 ? *c : '?');
            log_ << "\"";
        }
    }
    log_ << '\n';

    return result;
}

} // namespace sekiro_haptics
