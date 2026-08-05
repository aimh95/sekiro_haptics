#pragma once

#include "sekiro_haptics/HapticEffect.hpp"

#include <string>

namespace sekiro_haptics {

/// A named, reusable description of what a game event should feel like.
///
/// HapticPreset is the bridge between an abstract GameEvent and a concrete
/// HapticEffect: `presetId` is what EventMapping references, `effect` is
/// what actually gets handed to HapticScheduler::Schedule(). Presets are
/// normally loaded from JSON via PresetRepository (see docs/trace-format.md
/// for the on-disk schema) rather than constructed as enum-typed presets
/// the way presets::PerfectDeflect() is -- new game events get a new
/// presetId in config, not a new HapticEffectType enumerator. See the
/// migration note in docs/ARCHITECTURE.md.
///
/// The current hardware only supports legacy left/right rumble, so `effect`
/// is the only representation today. `effect.type` is always
/// HapticEffectType::Generic for JSON-loaded presets -- identity lives in
/// `presetId`, not the enum. This struct deliberately holds a full
/// HapticEffect rather than e.g. bare left/right floats so that a future
/// preset kind (PCM haptics, adaptive-trigger commands) can be added by
/// extending HapticEffect/this struct without changing GameEvent or
/// EventMapping.
struct HapticPreset {
    std::string presetId;
    std::string displayName;
    HapticEffect effect;
};

} // namespace sekiro_haptics
