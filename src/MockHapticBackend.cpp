#include "sekiro_haptics/MockHapticBackend.hpp"

#include <iostream>

namespace sekiro_haptics {

std::ostream& MockHapticBackend::DefaultLogStream() {
    return std::cout;
}

MockHapticBackend::MockHapticBackend(std::ostream& log) : log_(log) {}

void MockHapticBackend::SendEffect(const HapticEffect& effect) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.push_back(effect);

    log_ << "[MockHapticBackend] SendEffect type=" << ToString(effect.type)
         << " left=" << effect.intensity.left << " right=" << effect.intensity.right
         << " duration=" << effect.duration.count() << "ms";
    if (!effect.debugLabel.empty()) {
        log_ << " label=\"" << effect.debugLabel << "\"";
    }
    log_ << '\n';

    cv_.notify_all();
}

void MockHapticBackend::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++resetCount_;
    log_ << "[MockHapticBackend] Reset\n";
}

bool MockHapticBackend::IsConnected() const {
    // The mock backend has no real connection state; it is always "ready".
    return true;
}

std::vector<HapticEffect> MockHapticBackend::History() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_;
}

int MockHapticBackend::ResetCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resetCount_;
}

bool MockHapticBackend::WaitForEffectCount(std::size_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return history_.size() >= count; });
}

} // namespace sekiro_haptics
