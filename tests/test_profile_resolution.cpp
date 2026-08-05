#include "sekiro_haptics/presets/MappingRepository.hpp"
#include "sekiro_haptics/presets/PresetRepository.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

SH_TEST(PresetRepository_LoadFromFile_LoadsValidPresets) {
    PresetRepository repo;
    PresetLoadOutcome outcome = repo.LoadFromFile(Fixture("presets.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 1);
    SH_CHECK(outcome.errors.empty());
    SH_CHECK(repo.Size() == 1);

    const HapticPreset* preset = repo.Find("sharp_metal_v1");
    SH_CHECK(preset != nullptr);
    SH_CHECK(preset->displayName == "Sharp Metal Impact");
    SH_CHECK(preset->effect.intensity.left > 0.44f && preset->effect.intensity.left < 0.46f);
    SH_CHECK(preset->effect.intensity.right > 0.84f && preset->effect.intensity.right < 0.86f);
    SH_CHECK(preset->effect.duration.count() == 28);
}

SH_TEST(PresetRepository_Find_ReturnsNullptrForUnknownId) {
    PresetRepository repo;
    repo.LoadFromFile(Fixture("presets.json"));

    SH_CHECK(repo.Find("does_not_exist") == nullptr);
}

SH_TEST(PresetRepository_LoadFromFile_ClampsOutOfRangeIntensity) {
    PresetRepository repo;
    PresetLoadOutcome outcome = repo.LoadFromFile(Fixture("presets_invalid.json"));

    SH_CHECK(outcome.ok);

    const HapticPreset* preset = repo.Find("clamped_v1");
    SH_CHECK(preset != nullptr);
    SH_CHECK(preset->effect.intensity.left == 1.0f);
    SH_CHECK(preset->effect.intensity.right == 0.0f);

    bool foundClampWarning = false;
    for (const auto& error : outcome.errors) {
        if (error.message.find("clamped") != std::string::npos) {
            foundClampWarning = true;
        }
    }
    SH_CHECK(foundClampWarning);
}

SH_TEST(PresetRepository_LoadFromFile_RejectsNonPositiveDuration) {
    PresetRepository repo;
    PresetLoadOutcome outcome = repo.LoadFromFile(Fixture("presets_invalid.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(repo.Find("bad_duration_v1") == nullptr);

    bool foundDurationError = false;
    for (const auto& error : outcome.errors) {
        if (error.message.find("durationMs") != std::string::npos) {
            foundDurationError = true;
        }
    }
    SH_CHECK(foundDurationError);
}

SH_TEST(MappingRepository_LoadFromFile_LoadsValidMappings) {
    MappingRepository repo;
    MappingLoadOutcome outcome = repo.LoadFromFile(Fixture("mappings.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 1);
    SH_CHECK(repo.Size() == 1);

    const EventMapping* mapping = repo.Find("sekiro", "combat.perfect_deflect");
    SH_CHECK(mapping != nullptr);
    SH_CHECK(mapping->presetId == "sharp_metal_v1");
}

SH_TEST(MappingRepository_Find_ReturnsNullptrForUnknownMapping) {
    MappingRepository repo;
    repo.LoadFromFile(Fixture("mappings.json"));

    SH_CHECK(repo.Find("sekiro", "combat.does_not_exist") == nullptr);
}

SH_TEST(MappingRepository_LoadFromFile_ReferencingMissingPreset_StillLoads) {
    MappingRepository repo;
    MappingLoadOutcome outcome = repo.LoadFromFile(Fixture("mappings_missing_preset.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 1);

    const EventMapping* mapping = repo.Find("sekiro", "combat.take_damage");
    SH_CHECK(mapping != nullptr);
    SH_CHECK(mapping->presetId == "does_not_exist_v1");
}
