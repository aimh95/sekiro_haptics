#include "sekiro_haptics/ActionScript.hpp"
#include "sekiro_haptics/Json.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
namespace sekiro_haptics {
std::vector<TimedAction> LoadActionScript(std::istream& input) {
    std::vector<TimedAction> actions;
    std::string line;
    std::size_t number = 0;
    while (std::getline(input,line)) {
        ++number;
        auto fail = [&](const std::string& why) { throw std::runtime_error("action line "+std::to_string(number)+": "+why); };
        if (line.empty()) continue;
        if (line.size() > 4096 || actions.size() >= 10'000) fail("script size limit exceeded");
        auto parsed = json::ParseJson(line);
        if (!parsed.ok || !parsed.value.IsObject()) fail("expected JSON object");
        const auto& obj = parsed.value;
        const auto* at = obj.Find("atMs");
        if (!at || !at->IsNumber() || !std::isfinite(at->AsNumber()) || at->AsNumber() < 0 ||
            at->AsNumber() > 300'000 || std::floor(at->AsNumber()) != at->AsNumber()) fail("atMs must be integer 0..300000");
        const auto time = static_cast<std::int64_t>(at->AsNumber())*1000;
        if (!actions.empty() && time < actions.back().atUs) fail("times must be nondecreasing");
        auto action = ParseSekiroAction(obj.GetString("action"));
        if (!action) fail("unknown action");
        Prosthetic tool = Prosthetic::None;
        if (*action == SekiroAction::SelectProsthetic) {
            auto value = ParseProsthetic(obj.GetString("tool"));
            if (!value) fail("select requires a known tool");
            tool = *value;
        } else if (obj.Find("tool")) fail("tool is only allowed on select");
        for (const auto& member : obj.AsObject())
            if (member.first != "atMs" && member.first != "action" && member.first != "tool") fail("unknown field "+member.first);
        actions.push_back({time,*action,tool});
    }
    if (input.bad()) throw std::runtime_error("action script read failed");
    if (actions.empty()) throw std::runtime_error("action script is empty");
    return actions;
}
}
