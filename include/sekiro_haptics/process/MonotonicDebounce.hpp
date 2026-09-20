#pragma once

// SEK-PROBE-001E Section 7: a hotkey/controller input thread can re-trigger
// faster than the human intended (double-tap under combat adrenaline, or a
// controller's own switch bounce) even with MOD_NOREPEAT already suppressing
// OS-level key-repeat. This is a monotonic-clock debounce shared by both
// input threads in apps/sekiro_signal_probe/main.cpp: a trigger within
// `debounceUs` of the previous accepted trigger for the *same* key/button is
// ignored outright. Pure logic, no OS dependency, so it can be unit tested
// directly rather than only exercised indirectly through the Win32 app.

#include <cstdint>

namespace sekiro_haptics::process {

class MonotonicDebounce {
public:
    explicit MonotonicDebounce(std::int64_t debounceUs) : debounceUs_(debounceUs) {}

    /// Returns true if `nowUs` is far enough past the last accepted
    /// trigger to fire again, and records `nowUs` as the new last-trigger
    /// time if so. The very first call always fires.
    bool ShouldFire(std::int64_t nowUs) {
        if (hasLast_ && (nowUs - lastUs_) < debounceUs_) {
            return false;
        }
        hasLast_ = true;
        lastUs_ = nowUs;
        return true;
    }

private:
    std::int64_t debounceUs_;
    std::int64_t lastUs_ = 0;
    bool hasLast_ = false;
};

} // namespace sekiro_haptics::process
