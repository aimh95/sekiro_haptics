#pragma once

#include <chrono>
#include <string>

namespace sekiro_haptics {

/// Identifies the semantic category of a haptic effect.
///
/// This is a small, Sekiro-flavored vocabulary rather than a generic
/// "vibrate for N ms" primitive, so that backends and higher-level game
/// logic can reason about *why* an effect is playing rather than just its
/// raw motor values. New game events should add a new enumerator here
/// rather than being encoded into debugLabel.
enum class HapticEffectType {
    Generic,
    PerfectDeflect,
    PostureBreak,
    TakeDamage,
    Deathblow,
};

/// Returns a human-readable name for a HapticEffectType, e.g. for logging.
const char* ToString(HapticEffectType type);

/// Motor intensities normalized to the range [0.0, 1.0].
///
/// The left/right split mirrors typical dual-motor rumble hardware. This
/// project does not model per-trigger (adaptive trigger) effects yet.
struct MotorIntensity {
    float left = 0.0f;
    float right = 0.0f;
};

/// Immutable-by-convention description of a single haptic effect to be
/// played.
///
/// HapticEffect is pure data: it carries no behavior and no knowledge of how
/// it will be delivered to hardware. IHapticBackend implementations decide
/// how (or whether) to interpret each field.
struct HapticEffect {
    HapticEffectType type = HapticEffectType::Generic;
    MotorIntensity intensity;

    /// How long the effect should be played for.
    std::chrono::milliseconds duration{100};

    /// Free-form label for logging/debugging. Not interpreted by backends.
    std::string debugLabel;
};

} // namespace sekiro_haptics
