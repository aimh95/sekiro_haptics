// OutputPreset loading and validation: the v1 file still loads, the v2 file
// adds PCM/speaker/trigger layers, and anything out of range is rejected
// BEFORE it could reach an output.

#include "sekiro_haptics/presets/OutputPresetRepository.hpp"
#include "sekiro_haptics/presets/PresetRepository.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;
using namespace sekiro_haptics::dualsense;

namespace {

const char* kLegacyFile = R"({
  "presets": [
    { "presetId": "sharp_metal_v1", "displayName": "Sharp Metal Impact",
      "legacyRumble": { "left": 0.45, "right": 0.85, "durationMs": 28 } }
  ]
})";

const char* kCurrentFile = R"({
  "schemaVersion": 2,
  "presets": [
    {
      "presetId": "guard.deflect",
      "displayName": "Deflect",
      "speaker":   [ { "cueId": "deflect.speaker", "gain": 1.0 } ],
      "pcmHaptic": [ { "cueId": "deflect.haptic", "gain": 1.0, "balance": 0.0 } ],
      "trigger":   [ { "side": "r2", "mode": "weapon", "startPosition": 3,
                       "endPosition": 7, "strength": 8, "durationMs": 120,
                       "startOffsetMs": 15 } ]
    }
  ]
})";

} // namespace

SH_TEST(OutputPreset_LoadsTheOriginalVersionOneFileUnchanged) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(kLegacyFile);

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.fatalError.empty());
    SH_CHECK(outcome.schemaVersion == kOutputPresetSchemaVersionLegacy);
    SH_CHECK(outcome.loadedCount == 1);

    const auto* preset = repository.Find("sharp_metal_v1");
    SH_CHECK(preset != nullptr);
    SH_CHECK(preset->legacyRumble.has_value());
    SH_CHECK(preset->legacyRumble->effect.intensity.right > 0.84f);
    SH_CHECK(preset->legacyRumble->effect.duration.count() == 28);
    SH_CHECK(preset->speaker.empty() && preset->pcmHaptic.empty() && preset->trigger.empty());
    SH_CHECK(preset->LayerCount() == 1);
}

SH_TEST(OutputPreset_TheShippedConfigFileStillLoads) {
    // The real file, not a copy: this is the regression guard for "the new
    // loader broke the config that is actually on disk".
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromFile(std::string(SH_REPO_CONFIG_DIR) + "/presets.json");
    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.errors.empty());
    SH_CHECK(outcome.loadedCount == 2);
    SH_CHECK(repository.Find("sharp_metal_v1") != nullptr);
    SH_CHECK(repository.Find("dull_thud_v1") != nullptr);

    // And the original repository still reads it too -- nothing was migrated
    // out from under the existing code path.
    PresetRepository legacy;
    const auto legacyOutcome = legacy.LoadFromFile(std::string(SH_REPO_CONFIG_DIR) + "/presets.json");
    SH_CHECK(legacyOutcome.ok);
    SH_CHECK(legacyOutcome.loadedCount == 2);
}

SH_TEST(OutputPreset_LoadsEveryLayerKindWithItsOwnTiming) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(kCurrentFile);
    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.errors.empty());

    const auto* preset = repository.Find("guard.deflect");
    SH_CHECK(preset != nullptr);
    SH_CHECK(preset->LayerCount() == 3);
    SH_CHECK(preset->speaker[0].cueId == "deflect.speaker");
    SH_CHECK(preset->pcmHaptic[0].cueId == "deflect.haptic");
    SH_CHECK(preset->trigger[0].side == TriggerSide::Right);
    SH_CHECK(preset->trigger[0].spec.mode == TriggerEffectMode::Weapon);
    SH_CHECK(preset->trigger[0].spec.weapon.startPosition == 3);
    SH_CHECK(preset->trigger[0].spec.weapon.endPosition == 7);
    // Independent timing: the speaker starts at the event, the trigger 15 ms
    // later, and the trigger's 120 ms lifetime belongs to the trigger only.
    SH_CHECK(preset->speaker[0].startOffsetMs == 0.0f);
    SH_CHECK(preset->trigger[0].startOffsetMs == 15.0f);
    SH_CHECK(preset->trigger[0].durationMs == 120.0f);
}

SH_TEST(OutputPreset_ATriggerParameterOutsideItsRangeIsRejectedNotLoaded) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 2,
      "presets": [ { "presetId": "bad", "displayName": "Bad",
        "trigger": [ { "side": "l2", "mode": "feedback",
                       "position": 3, "strength": 99 } ] } ]
    })");

    SH_CHECK(outcome.ok);            // the FILE is fine
    SH_CHECK(outcome.loadedCount == 0);
    SH_CHECK(!outcome.errors.empty());
    SH_CHECK(repository.Find("bad") == nullptr);   // never reachable by output
}

SH_TEST(OutputPreset_AParameterFromAnotherModeIsAnErrorNotAnIgnoredKey) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 2,
      "presets": [ { "presetId": "bad", "displayName": "Bad",
        "trigger": [ { "side": "l2", "mode": "feedback", "position": 3,
                       "strength": 4, "frequencyHz": 40 } ] } ]
    })");
    SH_CHECK(outcome.loadedCount == 0);
    SH_CHECK(!outcome.errors.empty());
    SH_CHECK(outcome.errors[0].message.find("frequencyHz") != std::string::npos);
}

SH_TEST(OutputPreset_ANegativeStartOffsetIsRejected) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 2,
      "presets": [ { "presetId": "bad", "displayName": "Bad",
        "speaker": [ { "cueId": "x", "startOffsetMs": -5 } ] } ]
    })");
    SH_CHECK(outcome.loadedCount == 0);
    SH_CHECK(!outcome.errors.empty());
}

SH_TEST(OutputPreset_APresetWithNoLayersIsRejected) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 2,
      "presets": [ { "presetId": "empty", "displayName": "Nothing" } ]
    })");
    SH_CHECK(outcome.loadedCount == 0);
    SH_CHECK(!outcome.errors.empty());
}

SH_TEST(OutputPreset_AFileFromANewerSchemaIsRefusedRatherThanPartlyRead) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 99,
      "presets": [ { "presetId": "x", "displayName": "X",
                     "speaker": [ { "cueId": "c" } ] } ]
    })");
    SH_CHECK(!outcome.ok);
    SH_CHECK(!outcome.fatalError.empty());
    SH_CHECK(repository.Size() == 0);
}

SH_TEST(OutputPreset_OneBadEntryDoesNotDiscardTheGoodOnesAroundIt) {
    OutputPresetRepository repository;
    const auto outcome = repository.LoadFromString(R"({
      "schemaVersion": 2,
      "presets": [
        { "presetId": "good1", "displayName": "A", "speaker": [ { "cueId": "c" } ] },
        { "presetId": "bad", "displayName": "B",
          "trigger": [ { "side": "l2", "mode": "weapon", "startPosition": 1,
                         "endPosition": 5, "strength": 4 } ] },
        { "presetId": "good2", "displayName": "C", "pcmHaptic": [ { "cueId": "h" } ] }
      ]
    })");
    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 2);
    SH_CHECK(repository.Find("good1") != nullptr);
    SH_CHECK(repository.Find("good2") != nullptr);
    SH_CHECK(repository.Find("bad") == nullptr);
}

SH_TEST(OutputPreset_ConvertingALegacyPresetKeepsItsRumbleAndNothingElse) {
    HapticPreset legacy;
    legacy.presetId = "sharp_metal_v1";
    legacy.displayName = "Sharp Metal Impact";
    legacy.effect.intensity = {0.45f, 0.85f};
    legacy.effect.duration = std::chrono::milliseconds(28);

    const auto converted = FromLegacyPreset(legacy);
    SH_CHECK(converted.presetId == legacy.presetId);
    SH_CHECK(converted.schemaVersion == kOutputPresetSchemaVersionLegacy);
    SH_CHECK(converted.LayerCount() == 1);
    SH_CHECK(converted.legacyRumble.has_value());
    SH_CHECK(ValidateOutputPreset(converted).ok);
}
