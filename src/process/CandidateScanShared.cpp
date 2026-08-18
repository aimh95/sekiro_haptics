#include "sekiro_haptics/process/CandidateScanShared.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sekiro_haptics::process {

CandidateScanResult ResolveScanScopeRegions(IProcessInspector& inspector, const std::vector<ProcessMemoryRegion>& allRegions,
                                             CandidateScanScope scope,
                                             std::vector<ProcessMemoryRegion>& outTargetRegions) {
    std::vector<ProcessMemoryRegion> targetRegions;

    if (scope == CandidateScanScope::AllReadable) {
        targetRegions = allRegions;
    } else if (scope == CandidateScanScope::PrivateReadable) {
        for (const ProcessMemoryRegion& region : allRegions) {
            if (region.kind == MemoryRegionKind::Private) {
                targetRegions.push_back(region);
            }
        }
    } else { // MainModule
        ModuleInfo mainModule;
        ProcessInspectionResult moduleResult = inspector.GetMainModule(mainModule);
        if (moduleResult != ProcessInspectionResult::Success) {
            return CandidateScanResult::ModuleNotFound;
        }
        std::uintptr_t moduleEnd = mainModule.baseAddress + mainModule.imageSize;
        for (const ProcessMemoryRegion& region : allRegions) {
            if (region.kind != MemoryRegionKind::Image) {
                continue;
            }
            std::uintptr_t regionEnd = region.baseAddress + region.sizeBytes;
            std::uintptr_t clippedBase = std::max(region.baseAddress, mainModule.baseAddress);
            std::uintptr_t clippedEnd = std::min(regionEnd, moduleEnd);
            if (clippedEnd > clippedBase) {
                ProcessMemoryRegion clipped = region;
                clipped.baseAddress = clippedBase;
                clipped.sizeBytes = clippedEnd - clippedBase;
                targetRegions.push_back(clipped);
            }
        }
    }

    outTargetRegions = std::move(targetRegions);
    return CandidateScanResult::Success;
}

std::uint64_t ComputeRegionAlignedValueCount(const ProcessMemoryRegion& region, std::size_t valueSize) {
    std::uint64_t sizeBytes = static_cast<std::uint64_t>(region.sizeBytes);
    std::uint64_t remainder = static_cast<std::uint64_t>(region.baseAddress) % static_cast<std::uint64_t>(valueSize);
    std::uint64_t alignedStart = (remainder == 0) ? 0 : (static_cast<std::uint64_t>(valueSize) - remainder);

    if (alignedStart >= sizeBytes) {
        return 0;
    }
    return (sizeBytes - alignedStart) / static_cast<std::uint64_t>(valueSize);
}

CandidateScanPlanTotals ComputeCandidateScanPlanTotals(const std::vector<ProcessMemoryRegion>& targetRegions,
                                                        CandidateValueType type) {
    CandidateScanPlanTotals totals;
    totals.regionCount = targetRegions.size();

    std::size_t valueSize = CandidateValueTypeSize(type);
    for (const ProcessMemoryRegion& region : targetRegions) {
        totals.totalScopeBytes += static_cast<std::uint64_t>(region.sizeBytes);
        totals.comparableValueCount += ComputeRegionAlignedValueCount(region, valueSize);
    }

    return totals;
}

const char* ToString(InMemoryBudgetCheckResult result) {
    switch (result) {
        case InMemoryBudgetCheckResult::WithinBudget:
            return "WithinBudget";
        case InMemoryBudgetCheckResult::InMemoryBudgetExceeded:
            return "InMemoryBudgetExceeded";
    }
    return "Unknown";
}

InMemoryBudgetCheckResult CheckInMemoryScanBudget(const CandidateScanPlanTotals& totals, std::size_t memoryBudgetBytes) {
    constexpr std::uint64_t candidateSize = sizeof(Candidate);
    constexpr std::uint64_t maxValueCountBeforeOverflow = std::numeric_limits<std::uint64_t>::max() / candidateSize;

    if (totals.comparableValueCount > maxValueCountBeforeOverflow) {
        // The multiplication below would overflow -- unambiguously exceeds
        // any real budget, so report it as exceeded rather than wrapping.
        return InMemoryBudgetCheckResult::InMemoryBudgetExceeded;
    }

    std::uint64_t requiredBytes = totals.comparableValueCount * candidateSize;
    if (requiredBytes > static_cast<std::uint64_t>(memoryBudgetBytes)) {
        return InMemoryBudgetCheckResult::InMemoryBudgetExceeded;
    }
    return InMemoryBudgetCheckResult::WithinBudget;
}

namespace {

bool ApproxEqualFloat(float a, float b) {
    if (std::isnan(a) || std::isnan(b)) {
        return false;
    }
    // Exact equality first -- handles +Infinity == +Infinity and
    // -Infinity == -Infinity, where a - b would otherwise be NaN (inf -
    // inf) and wrongly fail the epsilon check below.
    if (a == b) {
        return true;
    }
    return std::fabs(a - b) <= kCandidateFloatEpsilon;
}

template <typename T>
bool EvaluateIntegerFilter(CandidateFilterKind kind, T previous, T current, const T* exactTarget) {
    switch (kind) {
        case CandidateFilterKind::Changed:
            return current != previous;
        case CandidateFilterKind::Unchanged:
            return current == previous;
        case CandidateFilterKind::Increased:
            return current > previous;
        case CandidateFilterKind::Decreased:
            return current < previous;
        case CandidateFilterKind::ExactValue:
            return exactTarget != nullptr && current == *exactTarget;
    }
    return false;
}

bool EvaluateFloatFilter(CandidateFilterKind kind, float previous, float current, const float* exactTarget) {
    switch (kind) {
        case CandidateFilterKind::Changed:
            return !ApproxEqualFloat(previous, current);
        case CandidateFilterKind::Unchanged:
            return ApproxEqualFloat(previous, current);
        case CandidateFilterKind::Increased:
            return !std::isnan(previous) && !std::isnan(current) && current > previous + kCandidateFloatEpsilon;
        case CandidateFilterKind::Decreased:
            return !std::isnan(previous) && !std::isnan(current) && current < previous - kCandidateFloatEpsilon;
        case CandidateFilterKind::ExactValue:
            return exactTarget != nullptr && ApproxEqualFloat(current, *exactTarget);
    }
    return false;
}

} // namespace

bool EvaluateCandidateFilter(CandidateFilterKind kind, const CandidateValue& previous, const CandidateValue& current,
                              const CandidateValue* exactTarget) {
    if (std::holds_alternative<std::uint8_t>(previous)) {
        const std::uint8_t* target = exactTarget != nullptr ? std::get_if<std::uint8_t>(exactTarget) : nullptr;
        return EvaluateIntegerFilter(kind, std::get<std::uint8_t>(previous), std::get<std::uint8_t>(current), target);
    }
    if (std::holds_alternative<std::uint16_t>(previous)) {
        const std::uint16_t* target = exactTarget != nullptr ? std::get_if<std::uint16_t>(exactTarget) : nullptr;
        return EvaluateIntegerFilter(kind, std::get<std::uint16_t>(previous), std::get<std::uint16_t>(current), target);
    }
    if (std::holds_alternative<std::uint32_t>(previous)) {
        const std::uint32_t* target = exactTarget != nullptr ? std::get_if<std::uint32_t>(exactTarget) : nullptr;
        return EvaluateIntegerFilter(kind, std::get<std::uint32_t>(previous), std::get<std::uint32_t>(current), target);
    }
    if (std::holds_alternative<std::int32_t>(previous)) {
        const std::int32_t* target = exactTarget != nullptr ? std::get_if<std::int32_t>(exactTarget) : nullptr;
        return EvaluateIntegerFilter(kind, std::get<std::int32_t>(previous), std::get<std::int32_t>(current), target);
    }
    const float* target = exactTarget != nullptr ? std::get_if<float>(exactTarget) : nullptr;
    return EvaluateFloatFilter(kind, std::get<float>(previous), std::get<float>(current), target);
}

} // namespace sekiro_haptics::process
