#include "sekiro_haptics/HapticEffect.hpp"
#include "sekiro_haptics/Presets.hpp"
#include "testing.hpp"

#include <cstring>

using namespace sekiro_haptics;

SH_TEST(HapticEffect_DefaultsAreNeutral) {
    HapticEffect effect;
    SH_CHECK(effect.type == HapticEffectType::Generic);
    SH_CHECK(effect.intensity.left == 0.0f);
    SH_CHECK(effect.intensity.right == 0.0f);
    SH_CHECK(effect.debugLabel.empty());
}

SH_TEST(ToString_CoversKnownTypes) {
    SH_CHECK(std::strcmp(ToString(HapticEffectType::Generic), "Generic") == 0);
    SH_CHECK(std::strcmp(ToString(HapticEffectType::PerfectDeflect), "PerfectDeflect") == 0);
    SH_CHECK(std::strcmp(ToString(HapticEffectType::PostureBreak), "PostureBreak") == 0);
    SH_CHECK(std::strcmp(ToString(HapticEffectType::TakeDamage), "TakeDamage") == 0);
    SH_CHECK(std::strcmp(ToString(HapticEffectType::Deathblow), "Deathblow") == 0);
}

SH_TEST(Presets_PerfectDeflect_IsHighIntensityAndShort) {
    HapticEffect effect = presets::PerfectDeflect();
    SH_CHECK(effect.type == HapticEffectType::PerfectDeflect);
    SH_CHECK(effect.intensity.left > 0.0f);
    SH_CHECK(effect.intensity.right > 0.0f);
    SH_CHECK(effect.duration.count() > 0);
    SH_CHECK(effect.debugLabel == "PerfectDeflect");
}
