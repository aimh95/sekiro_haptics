#pragma once

// Test-only IProcessInspector double for AddressResolver tests. Lives
// under tests/, never included by production code. Only FindModuleExact()
// is meaningfully exercised by AddressResolver -- GetImagePath()/
// GetMainModule() are stubbed since AddressResolver never calls them.

#include "sekiro_haptics/process/IProcessInspector.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sekiro_haptics::process {

class FakeProcessInspector final : public IProcessInspector {
public:
    void SetModule(const std::string& moduleName, ModuleInfo info) { modules_.emplace_back(moduleName, info); }

    /// Forces every FindModuleExact() call to return this result instead
    /// of doing the normal name lookup -- for simulating enumeration-level
    /// failures unrelated to any specific module name.
    void ForceFindModuleResult(ProcessInspectionResult result) { forcedResult_ = result; }

    int FindModuleCalls() const { return findModuleCalls_; }

    ProcessInspectionResult GetImagePath(std::filesystem::path&) override { return ProcessInspectionResult::Success; }
    ProcessInspectionResult GetMainModule(ModuleInfo&) override { return ProcessInspectionResult::Success; }

    ProcessInspectionResult FindModuleExact(const std::string& moduleName, ModuleInfo& outModule) override {
        ++findModuleCalls_;
        if (forcedResult_.has_value()) {
            return *forcedResult_;
        }

        const ModuleInfo* matched = nullptr;
        std::size_t matchCount = 0;
        for (const auto& entry : modules_) {
            if (EqualsIgnoreCaseAscii(entry.first, moduleName)) {
                matched = &entry.second;
                ++matchCount;
            }
        }

        if (matchCount == 0) {
            return ProcessInspectionResult::ModuleNotFound;
        }
        if (matchCount > 1) {
            return ProcessInspectionResult::MultipleModules;
        }
        outModule = *matched;
        return ProcessInspectionResult::Success;
    }

private:
    static bool EqualsIgnoreCaseAscii(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::pair<std::string, ModuleInfo>> modules_;
    std::optional<ProcessInspectionResult> forcedResult_;
    int findModuleCalls_ = 0;
};

} // namespace sekiro_haptics::process
