#include "sekiro_haptics/ProstheticTriggerPolicy.hpp"

namespace sekiro_haptics {

bool IsSupportedProsthetic(std::uint32_t equipId) {
    return equipId == kLoadedShurikenEquipId || equipId == kLoadedSpearEquipId;
}

ProstheticPrep PrepForSelection(std::uint32_t selectedEquipId, bool selectionKnown,
                                const ProstheticTriggerSettings& settings) {
    ProstheticPrep prep;
    if (!settings.enabled) {
        prep.reason = "disabled";
        return prep;
    }
    if (!selectionKnown) {
        // Load, death, menu, object replacement, failed read. The resistance
        // goes away rather than outliving the state that justified it.
        prep.reason = "selection unknown";
        return prep;
    }
    if (selectedEquipId == kNoEquipId) {
        prep.reason = "no prosthetic selected";
        return prep;
    }

    switch (selectedEquipId) {
        case kLoadedShurikenEquipId:
            prep.active = true;
            prep.equipId = selectedEquipId;
            prep.reason = "Loaded Shuriken -- weapon catch";
            prep.spec = dualsense::TriggerEffectSpec::MakeWeapon(
                settings.shurikenStartZone, settings.shurikenEndZone, settings.shurikenStrength);
            break;
        case kLoadedSpearEquipId:
            prep.active = true;
            prep.equipId = selectedEquipId;
            prep.reason = "Loaded Spear -- constant resistance";
            prep.spec = dualsense::TriggerEffectSpec::MakeFeedback(
                settings.spearStartZone, settings.spearStrength);
            break;
        default:
            // An upgrade, a different tool, or an id nothing here has a
            // decided feel for. Releasing is the honest answer; reusing a
            // neighbour's effect because the id is numerically close would be
            // putting a resistance on the trigger for a tool it was never
            // chosen for.
            prep.equipId = selectedEquipId;
            prep.reason = "unsupported prosthetic";
            break;
    }

    if (prep.active) {
        // The settings are user-adjustable, so a bad combination is reachable
        // (Weapon rejects end <= start, and every zone/strength has a range).
        // A rejected spec releases instead of sending something the firmware
        // would refuse or read as another field.
        if (!dualsense::ValidateTriggerEffect(prep.spec).ok) {
            prep.active = false;
            prep.reason = "settings out of range";
        }
    }
    return prep;
}

} // namespace sekiro_haptics
