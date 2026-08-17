#pragma once

// Test-only IProcessMemoryMap double for CandidateScanner tests. Lives
// under tests/, never included by production code.

#include "sekiro_haptics/process/IProcessMemoryMap.hpp"

#include <optional>
#include <vector>

namespace sekiro_haptics::process {

class FakeProcessMemoryMap final : public IProcessMemoryMap {
public:
    void SetRegions(std::vector<ProcessMemoryRegion> regions) { regions_ = std::move(regions); }
    void ForceResult(MemoryMapResult result) { forcedResult_ = result; }

    MemoryMapResult EnumerateReadableRegions(std::vector<ProcessMemoryRegion>& outRegions) const override {
        if (forcedResult_.has_value()) {
            return *forcedResult_;
        }
        outRegions = regions_;
        return MemoryMapResult::Success;
    }

private:
    std::vector<ProcessMemoryRegion> regions_;
    std::optional<MemoryMapResult> forcedResult_;
};

} // namespace sekiro_haptics::process
