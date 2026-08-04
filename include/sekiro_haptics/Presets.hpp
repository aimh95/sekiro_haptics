#pragma once

#include "sekiro_haptics/HapticEffect.hpp"

namespace sekiro_haptics::presets {

/// A sharp, high-intensity, short pulse representing a successful Sekiro
/// "Perfect Deflect" parry.
///
/// This is the only preset wired up to the console test application in the
/// current scope. Additional presets (PostureBreak, TakeDamage, Deathblow)
/// can follow the same pattern once real gameplay-event sources exist.
HapticEffect PerfectDeflect();

} // namespace sekiro_haptics::presets
