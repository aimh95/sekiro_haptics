#pragma once

// Test-only IDualSenseTransport double. Lives under tests/, not
// include/sekiro_haptics/ -- production code (DualSenseLegacyBackend, any
// caller) depends only on IDualSenseTransport and never links or includes
// this type.

#include "sekiro_haptics/IDualSenseTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sekiro_haptics {

class FakeDualSenseTransport final : public IDualSenseTransport {
public:
    std::vector<HidDeviceInfo> EnumerateCandidates() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++enumerateCount_;
        return candidates_;
    }

    TransportResult Open(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++openAttempts_;
        lastOpenPath_ = path;
        if (!openShouldSucceed_) {
            return TransportResult::OpenFailed;
        }
        isOpen_ = true;
        return TransportResult::Success;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++closeCount_;
        isOpen_ = false;
    }

    bool IsOpen() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return isOpen_;
    }

    TransportResult WriteOutputReport(const std::uint8_t* report, std::size_t length) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++writeCount_;
        if (!isOpen_) {
            return TransportResult::NotOpen;
        }
        if (alwaysFailWrites_ || failNextWrite_) {
            failNextWrite_ = false;
            return TransportResult::WriteFailed;
        }
        writtenReports_.emplace_back(report, report + length);
        cv_.notify_all();
        return TransportResult::Success;
    }

    // --- test-only controls ---

    /// Sets IsOpen()'s return value directly, without going through
    /// Open()/EnumerateCandidates() -- most tests just need "the device is
    /// connected" without a fake enumeration/path round-trip.
    void SetOpen(bool open) {
        std::lock_guard<std::mutex> lock(mutex_);
        isOpen_ = open;
    }

    void SetOpenShouldSucceed(bool succeed) {
        std::lock_guard<std::mutex> lock(mutex_);
        openShouldSucceed_ = succeed;
    }

    /// The next WriteOutputReport() call fails; subsequent calls succeed
    /// again (unless SetAlwaysFailWrites() is also set).
    void FailNextWrite() {
        std::lock_guard<std::mutex> lock(mutex_);
        failNextWrite_ = true;
    }

    void SetAlwaysFailWrites(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        alwaysFailWrites_ = fail;
    }

    int WriteCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writeCount_;
    }

    int OpenAttempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return openAttempts_;
    }

    int CloseCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closeCount_;
    }

    /// Every report that actually reached WriteOutputReport and returned
    /// Success, in call order. Does NOT include failed write attempts --
    /// WriteCount() covers those.
    std::vector<std::vector<std::uint8_t>> WrittenReports() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return writtenReports_;
    }

    /// Blocks until at least `count` reports have been successfully
    /// written, or `timeout` elapses. For deterministic tests against
    /// HapticScheduler's background dispatch thread, mirroring
    /// MockHapticBackend::WaitForEffectCount -- no fixed sleep needed.
    bool WaitForWriteCount(std::size_t count, std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return writtenReports_.size() >= count; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;

    bool isOpen_ = false;
    bool openShouldSucceed_ = true;
    bool failNextWrite_ = false;
    bool alwaysFailWrites_ = false;
    int writeCount_ = 0;
    int openAttempts_ = 0;
    int closeCount_ = 0;
    int enumerateCount_ = 0;
    std::string lastOpenPath_;
    std::vector<HidDeviceInfo> candidates_;
    std::vector<std::vector<std::uint8_t>> writtenReports_;
};

} // namespace sekiro_haptics
