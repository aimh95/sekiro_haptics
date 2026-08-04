#pragma once

#include "sekiro_haptics/IHapticBackend.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <ostream>
#include <vector>

namespace sekiro_haptics {

/// Test/demo backend that never touches real hardware.
///
/// Every effect passed to SendEffect() is logged to an injectable stream
/// (std::cout by default) and appended to an in-memory history so tests and
/// the console application can make assertions about, or print, what was
/// received. Thread-safe: HapticScheduler dispatches from a worker thread.
class MockHapticBackend final : public IHapticBackend {
public:
    /// `log` must outlive this object. Defaults to std::cout.
    explicit MockHapticBackend(std::ostream& log = DefaultLogStream());

    HapticBackendResult SendEffect(const HapticEffect& effect) override;
    HapticBackendResult Reset() override;
    bool IsConnected() const override;

    /// All effects received via SendEffect, in call order. Reset() does not
    /// clear this log; it is a record of everything ever sent, independent
    /// of device state.
    std::vector<HapticEffect> History() const;

    /// Number of times Reset() has been called.
    int ResetCount() const;

    /// Blocks until at least `count` effects have been received, or
    /// `timeout` elapses. Returns true if the count was reached.
    ///
    /// Intended for deterministic tests against HapticScheduler, which
    /// dispatches from a background thread and would otherwise make
    /// assertions racy.
    bool WaitForEffectCount(std::size_t count, std::chrono::milliseconds timeout) const;

private:
    static std::ostream& DefaultLogStream();

    std::ostream& log_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<HapticEffect> history_;
    int resetCount_ = 0;
};

} // namespace sekiro_haptics
