#pragma once

// Which R2 PREPARATION resistance belongs to the currently SELECTED prosthetic.
//
// "Preparation" is the whole point: the resistance has to already be on the
// trigger before the player pulls it, so it is decided by SELECTION STATE, not
// by detecting that something fired. Nothing in this file observes or reports a
// use, a shot, or a hit.
//
// SCOPE, DELIBERATELY NARROW
// --------------------------
// Exactly two EquipParamWeapon ids are supported, matched exactly:
//
//   70000  Loaded Shuriken   -> Weapon mode: a short catch that releases once
//                               the trigger passes it. The "click" feel.
//   78000  Loaded Spear      -> Feedback mode: one constant resistance held
//                               across a wide span. The "push through" feel.
//
// Anything else -- an upgraded Shuriken (70100..70500), an upgraded Spear, a
// different tool, an empty slot, or a selection this process could not read --
// releases whatever resistance this feature applied. Upgrades are NOT folded
// into their family here: the family is public catalogue data, while what an
// upgraded tool should feel like has not been decided, and guessing would put
// a resistance on a trigger for a tool nobody chose it for.
//
// THESE NUMBERS ARE A FIRST PROTOTYPE
// -----------------------------------
// The zone positions and strengths below are a starting point chosen to match
// two described sensations, not measured optima. Zone indices are positions
// along trigger travel (0 untouched .. 9 fully pressed) and strength is the
// firmware's 1..8 scale -- neither is a percentage and neither is a force.
// Every one of them is adjustable, which is why they are settings rather than
// constants.

#include "sekiro_haptics/dualsense/AdaptiveTrigger.hpp"

#include <cstdint>

namespace sekiro_haptics {

/// The two supported ids, matched exactly. Named here so a magic number never
/// appears at a call site.
inline constexpr std::uint32_t kLoadedShurikenEquipId = 70000;
inline constexpr std::uint32_t kLoadedSpearEquipId = 78000;
/// The game's empty-slot sentinel, mirrored so this header does not need the
/// process layer.
inline constexpr std::uint32_t kNoEquipId = 0xFFFFFFFFu;

/// Position, span and strength, separately adjustable for each of the two.
struct ProstheticTriggerSettings {
    bool enabled = true;

    /// Loaded Shuriken -- Weapon mode (0x25).
    ///
    /// Start and end are zone indices bounding the catch. They are kept in the
    /// early-to-middle part of travel on purpose: a wall only met at the very
    /// bottom of the pull reads as "the trigger is stiff", not as a click.
    /// Weapon requires start in 2..7 and end in start+1..8.
    std::uint8_t shurikenStartZone = 3;
    std::uint8_t shurikenEndZone = 4;
    /// 0..8. Low-to-middle to begin with; a hard catch is unpleasant to hold.
    std::uint8_t shurikenStrength = 4;

    /// Loaded Spear -- Feedback mode (0x21).
    ///
    /// Feedback resists from `position` to the end of travel, so a low start
    /// is what makes it a long smooth push rather than a late wall.
    std::uint8_t spearStartZone = 2;
    /// 0..8, low-to-middle so it stays smooth instead of becoming a stop.
    std::uint8_t spearStrength = 3;
};

/// What the trigger should be holding right now.
struct ProstheticPrep {
    /// False means "release the resistance this feature owns". It does NOT
    /// mean "leave the trigger alone" -- a stale prep effect outliving its
    /// selection is exactly the failure this is designed to prevent.
    bool active = false;
    dualsense::TriggerEffectSpec spec;
    /// The id this decision was made for, for logging. kNoEquipId when none.
    std::uint32_t equipId = kNoEquipId;
    /// Short human-readable reason, for logs. Never empty.
    const char* reason = "none";
};

/// Decides the preparation resistance for one selection state.
///
/// `selectionKnown` is false whenever the reader could not produce a trusted
/// answer this tick -- a load, a death, a menu, a failed read. That is treated
/// as "release", never as "keep what we had".
ProstheticPrep PrepForSelection(std::uint32_t selectedEquipId, bool selectionKnown,
                                const ProstheticTriggerSettings& settings);

/// True when this feature has a prep resistance for that id at all.
bool IsSupportedProsthetic(std::uint32_t equipId);

} // namespace sekiro_haptics
