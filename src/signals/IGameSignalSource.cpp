#include "sekiro_haptics/signals/IGameSignalSource.hpp"

namespace sekiro_haptics {

const char* ToString(SignalSourceResult result) {
    switch (result) {
        case SignalSourceResult::Success:
            return "Success";
        case SignalSourceResult::EndOfSource:
            return "EndOfSource";
        case SignalSourceResult::MalformedInput:
            return "MalformedInput";
    }
    return "Unknown";
}

} // namespace sekiro_haptics
