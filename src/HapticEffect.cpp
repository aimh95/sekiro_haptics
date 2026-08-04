#include "sekiro_haptics/HapticEffect.hpp"

namespace sekiro_haptics {

const char* ToString(HapticEffectType type) {
    switch (type) {
        case HapticEffectType::Generic:
            return "Generic";
        case HapticEffectType::PerfectDeflect:
            return "PerfectDeflect";
        case HapticEffectType::PostureBreak:
            return "PostureBreak";
        case HapticEffectType::TakeDamage:
            return "TakeDamage";
        case HapticEffectType::Deathblow:
            return "Deathblow";
    }
    return "Unknown";
}

} // namespace sekiro_haptics
