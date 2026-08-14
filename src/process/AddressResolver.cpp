#include "sekiro_haptics/process/AddressResolver.hpp"

#include "sekiro_haptics/process/AobScanner.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/RipRelative.hpp"

#include <limits>

namespace sekiro_haptics::process {

namespace {

ResolvedAddress MakeDisabled(const std::string& addressId, AddressResolutionReason reason,
                              std::string underlyingResult = {}) {
    ResolvedAddress result;
    result.addressId = addressId;
    result.state = AddressResolutionState::Disabled;
    result.reason = reason;
    result.underlyingResult = std::move(underlyingResult);
    return result;
}

AddressResolutionReason ReasonForScanFailure(AobScanResult result) {
    switch (result) {
        case AobScanResult::NoMatch:
            return AddressResolutionReason::PatternNotFound;
        case AobScanResult::MultipleMatches:
            return AddressResolutionReason::PatternAmbiguous;
        case AobScanResult::NotAttached:
        case AobScanResult::ProcessExited:
        case AobScanResult::ReadFailed:
        case AobScanResult::PartialRead:
            return AddressResolutionReason::ProcessReadFailed;
        default:
            // InvalidRange/InvalidPattern should never occur here given
            // the range/pattern were already validated before scanning --
            // fail closed rather than treat as success regardless.
            return AddressResolutionReason::InvalidScanRange;
    }
}

AddressResolutionReason ReasonForRipFailure(AobScanResult result) {
    switch (result) {
        case AobScanResult::TargetOutOfRange:
            return AddressResolutionReason::TargetOutOfRange;
        case AobScanResult::NotAttached:
        case AobScanResult::ProcessExited:
        case AobScanResult::ReadFailed:
        case AobScanResult::PartialRead:
            return AddressResolutionReason::ProcessReadFailed;
        default:
            // InvalidRange / InvalidDisplacementLayout / AddressOverflow.
            return AddressResolutionReason::AddressCalculationFailed;
    }
}

ResolvedAddress ResolveOneAddress(const AddressSpec& spec, IProcessInspector& inspector, IProcessReader& reader) {
    // 1-2: exact module lookup. FindModuleExact() only ever returns
    // Success alongside an already-validated (non-zero, non-overflowing)
    // ModuleInfo -- see IProcessInspector.hpp -- so base/size are trusted
    // as-is from here on.
    ModuleInfo module;
    ProcessInspectionResult moduleResult = inspector.FindModuleExact(spec.moduleName, module);
    if (moduleResult != ProcessInspectionResult::Success) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::ModuleNotFound, ToString(moduleResult));
    }
    std::uintptr_t moduleEnd = module.baseAddress + module.imageSize;

    // 3-4: module-relative scan range -> runtime range, fully contained
    // within the module.
    std::uintptr_t scanBase = module.baseAddress + spec.scanOffset;
    if (scanBase < module.baseAddress) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::InvalidScanRange);
    }
    std::uintptr_t scanEnd = scanBase + spec.scanSize;
    if (scanEnd < scanBase || scanEnd > moduleEnd) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::InvalidScanRange);
    }

    // 5: unique AOB scan -- reuses ScanProcessRange() unchanged.
    std::uintptr_t matchAddress = 0;
    AobScanResult scanResult = ScanProcessRange(reader, scanBase, spec.scanSize, spec.pattern, matchAddress);
    if (scanResult != AobScanResult::Success) {
        return MakeDisabled(spec.addressId, ReasonForScanFailure(scanResult), ToString(scanResult));
    }

    // 6: resolution-kind-specific address calculation.
    std::uintptr_t resolvedAddress = 0;

    if (spec.kind == AddressResolutionKind::RipRelative32) {
        RipRelativeSpec ripSpec;
        ripSpec.matchAddress = matchAddress;
        ripSpec.instructionOffset = spec.instructionOffset;
        ripSpec.displacementOffset = spec.displacementOffset;
        ripSpec.instructionLength = spec.instructionLength;
        ripSpec.allowedSourceRangeBase = module.baseAddress;
        ripSpec.allowedSourceRangeSize = module.imageSize;

        std::uintptr_t targetRangeBase = module.baseAddress + spec.targetRangeOffset;
        if (targetRangeBase < module.baseAddress) {
            return MakeDisabled(spec.addressId, AddressResolutionReason::InvalidScanRange);
        }
        ripSpec.allowedTargetRangeBase = targetRangeBase;
        ripSpec.allowedTargetRangeSize = spec.targetRangeSize;

        // ResolveRipRelativeFromProcess() -- reused unchanged -- already
        // performs its own full target-range validation (see
        // RipRelative.hpp), so no separate external target-range check is
        // needed for this resolution kind.
        AobScanResult ripResult = ResolveRipRelativeFromProcess(reader, ripSpec, resolvedAddress);
        if (ripResult != AobScanResult::Success) {
            return MakeDisabled(spec.addressId, ReasonForRipFailure(ripResult), ToString(ripResult));
        }

        ResolvedAddress result;
        result.addressId = spec.addressId;
        result.state = AddressResolutionState::Resolved;
        result.address = resolvedAddress;
        return result;
    }

    // AddressResolutionKind::MatchAddress: the unique match address plus a
    // signed module-relative offset.
    constexpr std::uintptr_t kInt64MaxAsUptr = static_cast<std::uintptr_t>(std::numeric_limits<std::int64_t>::max());
    if (matchAddress > kInt64MaxAsUptr) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::AddressCalculationFailed);
    }
    std::int64_t candidate = static_cast<std::int64_t>(matchAddress) + spec.matchOffset;
    if (candidate < 0) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::AddressCalculationFailed);
    }
    resolvedAddress = static_cast<std::uintptr_t>(candidate);

    // 7: explicit target-range check (RipRelative32 already did its own above).
    std::uintptr_t targetRangeBase = module.baseAddress + spec.targetRangeOffset;
    if (targetRangeBase < module.baseAddress) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::InvalidScanRange);
    }
    std::uintptr_t targetRangeEnd = targetRangeBase + spec.targetRangeSize;
    if (targetRangeEnd < targetRangeBase) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::InvalidScanRange);
    }
    if (resolvedAddress == 0 || resolvedAddress < targetRangeBase || resolvedAddress >= targetRangeEnd) {
        return MakeDisabled(spec.addressId, AddressResolutionReason::TargetOutOfRange);
    }

    // 8: record resolved.
    ResolvedAddress result;
    result.addressId = spec.addressId;
    result.state = AddressResolutionState::Resolved;
    result.address = resolvedAddress;
    return result;
}

} // namespace

ProfileResolutionOutcome ResolveAddressesForIdentity(const SignatureProfileRepository& repository,
                                                       const ExecutableIdentity& identity, IProcessInspector& inspector,
                                                       IProcessReader& reader) {
    ProfileResolutionOutcome outcome;

    const SignatureProfile* profile = nullptr;
    ProfileSelectionResult selectionResult = repository.SelectFor(identity, profile);

    if (selectionResult == ProfileSelectionResult::UnsupportedBuild) {
        outcome.buildSupported = false;
        outcome.buildFailureReason = AddressResolutionReason::UnsupportedBuild;
        return outcome;
    }
    if (selectionResult == ProfileSelectionResult::AmbiguousProfile) {
        outcome.buildSupported = false;
        outcome.buildFailureReason = AddressResolutionReason::ProfileInvalid;
        return outcome;
    }

    outcome.buildSupported = true;
    outcome.addresses.reserve(profile->addresses.size());
    for (const AddressSpec& spec : profile->addresses) {
        outcome.addresses.push_back(ResolveOneAddress(spec, inspector, reader));
    }
    return outcome;
}

} // namespace sekiro_haptics::process
