#include "sekiro_haptics/IHapticBackend.hpp"

namespace sekiro_haptics {

const char* ToString(HapticBackendResult result) {
    switch (result) {
        case HapticBackendResult::Success:
            return "Success";
        case HapticBackendResult::NotConnected:
            return "NotConnected";
        case HapticBackendResult::DeviceError:
            return "DeviceError";
    }
    return "Unknown";
}

} // namespace sekiro_haptics
