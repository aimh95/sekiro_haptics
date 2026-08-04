#include "sekiro_haptics/IDualSenseTransport.hpp"

namespace sekiro_haptics {

const char* ToString(TransportResult result) {
    switch (result) {
        case TransportResult::Success:
            return "Success";
        case TransportResult::NotFound:
            return "NotFound";
        case TransportResult::OpenFailed:
            return "OpenFailed";
        case TransportResult::WriteFailed:
            return "WriteFailed";
        case TransportResult::NotOpen:
            return "NotOpen";
    }
    return "Unknown";
}

} // namespace sekiro_haptics
