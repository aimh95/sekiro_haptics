#include "sekiro_haptics/replay/ExpectedEventRepository.hpp"
#include "testing.hpp"

#include <string>

using namespace sekiro_haptics::replay;

namespace {
std::string Fixture(const std::string& name) {
    return std::string(SH_FIXTURES_DIR) + "/" + name;
}
} // namespace

SH_TEST(ExpectedEventRepository_LoadFromFile_LoadsValidEntries) {
    ExpectedEventRepository repo;
    ExpectedEventLoadOutcome outcome = repo.LoadFromFile(Fixture("expected_perfect_deflect.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 1);
    SH_CHECK(outcome.errors.empty());
    SH_CHECK(repo.Events().size() == 1);

    const ExpectedEvent& event = repo.Events()[0];
    SH_CHECK(event.gameId == "sekiro");
    SH_CHECK(event.eventId == "combat.perfect_deflect");
    SH_CHECK(event.timestamp.count() == 1040);
}

SH_TEST(ExpectedEventRepository_LoadFromFile_EmptyArray_LoadsZeroEvents) {
    ExpectedEventRepository repo;
    ExpectedEventLoadOutcome outcome = repo.LoadFromFile(Fixture("expected_empty.json"));

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 0);
    SH_CHECK(repo.Events().empty());
}
