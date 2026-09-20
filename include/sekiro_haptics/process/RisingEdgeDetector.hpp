#pragma once

// Pure rising-edge (false->true transition) detector, used by
// apps/sekiro_signal_probe/main.cpp's RunControllerGuardWatchThread() to
// turn raw, per-poll XInput button state into a single "guard_input" event
// per physical press -- never one per poll while the button is held down.
// Extracted from main.cpp (rather than left as an inline bool) so the exact
// press/hold/release/repress sequence is unit-testable without XInput or a
// real controller.

namespace sekiro_haptics::process {

class RisingEdgeDetector {
public:
    /// Returns true exactly on a false->true transition; false for every
    /// other call (held true, held false, or the very first call when
    /// `pressed` is false).
    bool Update(bool pressed) {
        bool rose = pressed && !wasPressed_;
        wasPressed_ = pressed;
        return rose;
    }

private:
    bool wasPressed_ = false;
};

} // namespace sekiro_haptics::process
