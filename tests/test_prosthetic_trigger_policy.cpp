// Which R2 preparation resistance belongs to a selected prosthetic.
//
// Synthetic only: this is the decision, not the reading. Nothing here says the
// game was observed doing anything.

#include "sekiro_haptics/ProstheticTriggerPolicy.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

SH_TEST(ProstheticPrep_LoadedShurikenIsAWeaponCatchInEarlyTravel) {
    ProstheticTriggerSettings settings;
    const auto prep = PrepForSelection(kLoadedShurikenEquipId, true, settings);

    SH_CHECK(prep.active);
    SH_CHECK(prep.equipId == 70000);
    SH_CHECK(prep.spec.mode == TriggerEffectMode::Weapon);
    // The catch must be a SHORT span, and not parked at the bottom of travel:
    // a wall only met when fully pressed reads as a stiff trigger, not a click.
    SH_CHECK(prep.spec.weapon.startPosition <= 5);
    SH_CHECK(prep.spec.weapon.endPosition > prep.spec.weapon.startPosition);
    SH_CHECK(prep.spec.weapon.endPosition - prep.spec.weapon.startPosition <= 3);
    SH_CHECK(ValidateTriggerEffect(prep.spec).ok);
}

SH_TEST(ProstheticPrep_LoadedSpearIsOneConstantResistance) {
    ProstheticTriggerSettings settings;
    const auto prep = PrepForSelection(kLoadedSpearEquipId, true, settings);

    SH_CHECK(prep.active);
    SH_CHECK(prep.equipId == 78000);
    // Feedback resists from its position to the END of travel -- that is what
    // makes it a push rather than a catch.
    SH_CHECK(prep.spec.mode == TriggerEffectMode::Feedback);
    SH_CHECK(prep.spec.feedback.position <= 4);
    SH_CHECK(ValidateTriggerEffect(prep.spec).ok);
}

SH_TEST(ProstheticPrep_TheTwoToolsDoNotFeelTheSame) {
    // The whole request is that these are distinguishable, so a change that
    // made them the same mode should fail here rather than on the hardware.
    ProstheticTriggerSettings settings;
    const auto shuriken = PrepForSelection(kLoadedShurikenEquipId, true, settings);
    const auto spear = PrepForSelection(kLoadedSpearEquipId, true, settings);

    SH_CHECK(shuriken.active && spear.active);
    SH_CHECK(shuriken.spec.mode != spear.spec.mode);

    TriggerBlock a{}, b{};
    SH_CHECK(EncodeTriggerEffect(shuriken.spec, a).ok);
    SH_CHECK(EncodeTriggerEffect(spear.spec, b).ok);
    SH_CHECK(a != b);
}

SH_TEST(ProstheticPrep_StrengthsStartLowToMiddle) {
    // "낮음~중간 강도로 시작해라": a first prototype should not open at the
    // top of the scale.
    ProstheticTriggerSettings settings;
    SH_CHECK(settings.shurikenStrength >= 1 && settings.shurikenStrength <= 5);
    SH_CHECK(settings.spearStrength >= 1 && settings.spearStrength <= 5);
}

SH_TEST(ProstheticPrep_AnUpgradedShurikenIsNotFoldedIntoTheBaseOne) {
    // 70100..70500 are the same family in the public catalogue, but nothing
    // has decided what they should feel like. Reusing the base effect would be
    // putting a resistance on the trigger for a tool it was not chosen for.
    ProstheticTriggerSettings settings;
    for (std::uint32_t id : {70100u, 70200u, 70500u, 78100u, 75000u}) {
        const auto prep = PrepForSelection(id, true, settings);
        SH_CHECK(!prep.active);
        SH_CHECK(prep.equipId == id);
        SH_CHECK(std::string(prep.reason) == "unsupported prosthetic");
    }
}

SH_TEST(ProstheticPrep_AnUnknownSelectionReleasesRatherThanHolding) {
    ProstheticTriggerSettings settings;
    const auto prep = PrepForSelection(kLoadedShurikenEquipId, /*selectionKnown=*/false, settings);
    SH_CHECK(!prep.active);
    SH_CHECK(std::string(prep.reason) == "selection unknown");
}

SH_TEST(ProstheticPrep_AnEmptySlotReleases) {
    ProstheticTriggerSettings settings;
    const auto prep = PrepForSelection(kNoEquipId, true, settings);
    SH_CHECK(!prep.active);
}

SH_TEST(ProstheticPrep_DisablingTheFeatureReleases) {
    ProstheticTriggerSettings settings;
    settings.enabled = false;
    SH_CHECK(!PrepForSelection(kLoadedShurikenEquipId, true, settings).active);
    SH_CHECK(!PrepForSelection(kLoadedSpearEquipId, true, settings).active);
}

SH_TEST(ProstheticPrep_OutOfRangeSettingsReleaseInsteadOfSendingSomethingInvalid) {
    ProstheticTriggerSettings settings;
    settings.shurikenEndZone = settings.shurikenStartZone;   // Weapon rejects this
    const auto prep = PrepForSelection(kLoadedShurikenEquipId, true, settings);
    SH_CHECK(!prep.active);
    SH_CHECK(std::string(prep.reason) == "settings out of range");
}

SH_TEST(ProstheticPrep_PositionSpanAndStrengthAreEachAdjustable) {
    ProstheticTriggerSettings settings;
    settings.shurikenStartZone = 5;
    settings.shurikenEndZone = 7;
    settings.shurikenStrength = 6;
    settings.spearStartZone = 1;
    settings.spearStrength = 5;

    const auto shuriken = PrepForSelection(kLoadedShurikenEquipId, true, settings);
    SH_CHECK(shuriken.spec.weapon.startPosition == 5);
    SH_CHECK(shuriken.spec.weapon.endPosition == 7);
    SH_CHECK(shuriken.spec.weapon.strength == 6);

    const auto spear = PrepForSelection(kLoadedSpearEquipId, true, settings);
    SH_CHECK(spear.spec.feedback.position == 1);
    SH_CHECK(spear.spec.feedback.strength == 5);
}

SH_TEST(ProstheticPrep_OnlyTheTwoBaseToolsAreSupported) {
    SH_CHECK(IsSupportedProsthetic(70000));
    SH_CHECK(IsSupportedProsthetic(78000));
    SH_CHECK(!IsSupportedProsthetic(70100));
    SH_CHECK(!IsSupportedProsthetic(75000));
    SH_CHECK(!IsSupportedProsthetic(kNoEquipId));
}
