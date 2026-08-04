#include "sekiro_haptics/Presets.hpp"

namespace sekiro_haptics::presets {

HapticEffect PerfectDeflect() {
    HapticEffect effect;
    effect.type = HapticEffectType::PerfectDeflect;
    effect.intensity = MotorIntensity{1.0f, 1.0f};
    effect.duration = std::chrono::milliseconds{80};
    effect.debugLabel = "PerfectDeflect";
    return effect;
}

} // namespace sekiro_haptics::presets
