#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"

#include "sekiro_haptics/process/AobScanner.hpp"
#include "sekiro_haptics/process/RipRelative.hpp"

#include <cstring>

namespace sekiro_haptics::process {

namespace {

bool SameBuild(const ExecutableIdentity& a, const ExecutableIdentity& b) {
    return a.fileSizeBytes == b.fileSizeBytes && a.sha256 == b.sha256;
}

RootResolveResult MapAobScanResult(AobScanResult result) {
    switch (result) {
        case AobScanResult::NotAttached:
            return RootResolveResult::NotAttached;
        case AobScanResult::ProcessExited:
            return RootResolveResult::ProcessExited;
        case AobScanResult::NoMatch:
            return RootResolveResult::SignatureNotFound;
        case AobScanResult::MultipleMatches:
            return RootResolveResult::AmbiguousSignature;
        case AobScanResult::ReadFailed:
        case AobScanResult::PartialRead:
            return RootResolveResult::ReadFailed;
        case AobScanResult::InvalidRange:
        case AobScanResult::InvalidPattern:
        case AobScanResult::AddressOverflow:
        case AobScanResult::InvalidDisplacementLayout:
        case AobScanResult::TargetOutOfRange:
            return RootResolveResult::AddressCalculationFailed;
        case AobScanResult::Success:
            break;
    }
    return RootResolveResult::AddressCalculationFailed;
}

RootResolveResult MapReaderResult(ProcessReaderResult result) {
    switch (result) {
        case ProcessReaderResult::NotAttached:
            return RootResolveResult::NotAttached;
        case ProcessReaderResult::ProcessExited:
            return RootResolveResult::ProcessExited;
        default:
            return RootResolveResult::ReadFailed;
    }
}

/// Adds a signed byte offset to an address, reporting overflow/underflow
/// rather than wrapping silently.
bool AddSignedOffset(std::uintptr_t base, std::int64_t offset, std::uintptr_t& outAddress) {
    if (offset >= 0) {
        auto uoffset = static_cast<std::uintptr_t>(offset);
        std::uintptr_t result = base + uoffset;
        if (result < base) {
            return false; // overflow
        }
        outAddress = result;
        return true;
    }
    auto uoffset = static_cast<std::uintptr_t>(-offset);
    if (uoffset > base) {
        return false; // underflow
    }
    outAddress = base - uoffset;
    return true;
}

} // namespace

const char* ToString(RootResolveResult result) {
    switch (result) {
        case RootResolveResult::Resolved:
            return "Resolved";
        case RootResolveResult::UnsupportedBuild:
            return "UnsupportedBuild";
        case RootResolveResult::NotAttached:
            return "NotAttached";
        case RootResolveResult::ProcessExited:
            return "ProcessExited";
        case RootResolveResult::ModuleNotFound:
            return "ModuleNotFound";
        case RootResolveResult::SignatureNotFound:
            return "SignatureNotFound";
        case RootResolveResult::AmbiguousSignature:
            return "AmbiguousSignature";
        case RootResolveResult::AddressCalculationFailed:
            return "AddressCalculationFailed";
        case RootResolveResult::ReadFailed:
            return "ReadFailed";
        case RootResolveResult::NullPointer:
            return "NullPointer";
    }
    return "Unknown";
}

SekiroKnownRootResolver::SekiroKnownRootResolver(IProcessReader& reader, IProcessInspector& inspector,
                                                   KnownRootSpec spec, ExecutableIdentity expectedIdentity,
                                                   ExecutableIdentity currentIdentity)
    : reader_(reader),
      inspector_(inspector),
      spec_(std::move(spec)),
      identitySupported_(SameBuild(expectedIdentity, currentIdentity)) {}

ResolvedRoot SekiroKnownRootResolver::Resolve() {
    auto Fail = [&](RootResolveResult result) -> ResolvedRoot {
        current_ = ResolvedRoot{result, 0, current_.generation};
        return current_;
    };

    if (!identitySupported_) {
        return Fail(RootResolveResult::UnsupportedBuild);
    }

    ModuleInfo module;
    ProcessInspectionResult moduleResult = inspector_.FindModuleExact(spec_.moduleName, module);
    if (moduleResult != ProcessInspectionResult::Success) {
        switch (moduleResult) {
            case ProcessInspectionResult::NotAttached:
                return Fail(RootResolveResult::NotAttached);
            case ProcessInspectionResult::ProcessExited:
                return Fail(RootResolveResult::ProcessExited);
            default:
                return Fail(RootResolveResult::ModuleNotFound);
        }
    }

    std::uintptr_t matchAddress = 0;
    AobScanResult scanResult = ScanProcessRange(reader_, module.baseAddress, module.imageSize, spec_.pattern, matchAddress);
    if (scanResult != AobScanResult::Success) {
        return Fail(MapAobScanResult(scanResult));
    }

    RipRelativeSpec ripSpec;
    ripSpec.matchAddress = matchAddress;
    ripSpec.instructionOffset = spec_.instructionOffset;
    ripSpec.displacementOffset = spec_.displacementOffset;
    ripSpec.instructionLength = spec_.instructionLength;
    ripSpec.allowedSourceRangeBase = module.baseAddress;
    ripSpec.allowedSourceRangeSize = module.imageSize;
    // The RIP-relative target here is a *pointer slot* -- a static global
    // variable declared inside this same binary's own image, so it must
    // itself land inside the module's bounds (the live object it points to,
    // read one step further below, is a separate matter and is not
    // range-checked against the module at all).
    ripSpec.allowedTargetRangeBase = module.baseAddress;
    ripSpec.allowedTargetRangeSize = module.imageSize;

    std::uintptr_t pointerSlotAddress = 0;
    AobScanResult ripResult = ResolveRipRelativeFromProcess(reader_, ripSpec, pointerSlotAddress);
    if (ripResult != AobScanResult::Success) {
        return Fail(MapAobScanResult(ripResult));
    }

    std::uint64_t rawPointerValue = 0;
    ProcessReaderResult readResult = reader_.ReadBytes(pointerSlotAddress, &rawPointerValue, sizeof(rawPointerValue));
    if (readResult != ProcessReaderResult::Success) {
        return Fail(MapReaderResult(readResult));
    }

    auto objectAddress = static_cast<std::uintptr_t>(rawPointerValue);
    if (objectAddress == 0) {
        return Fail(RootResolveResult::NullPointer);
    }

    if (objectAddress != lastKnownAddress_) {
        ++nextGeneration_;
        lastKnownAddress_ = objectAddress;
    }
    // The very first successful resolve reports generation 1, not 2 --
    // nextGeneration_ starts at 1 and is only pre-incremented above when
    // lastKnownAddress_ (initially 0, an address no real resolve ever
    // reports) already differs, which is always true on the first call.
    current_ = ResolvedRoot{RootResolveResult::Resolved, objectAddress, nextGeneration_ - 1};
    return current_;
}

SekiroChildPointerResolver::SekiroChildPointerResolver(IProcessReader& reader, std::int64_t byteOffsetFromParent)
    : reader_(reader), byteOffsetFromParent_(byteOffsetFromParent) {}

ResolvedRoot SekiroChildPointerResolver::Resolve(std::uintptr_t parentAddress) {
    auto Fail = [&](RootResolveResult result) -> ResolvedRoot {
        current_ = ResolvedRoot{result, 0, current_.generation};
        return current_;
    };

    if (parentAddress == 0) {
        return Fail(RootResolveResult::NullPointer);
    }

    std::uintptr_t childSlotAddress = 0;
    if (!AddSignedOffset(parentAddress, byteOffsetFromParent_, childSlotAddress)) {
        return Fail(RootResolveResult::AddressCalculationFailed);
    }

    std::uint64_t rawPointerValue = 0;
    ProcessReaderResult readResult = reader_.ReadBytes(childSlotAddress, &rawPointerValue, sizeof(rawPointerValue));
    if (readResult != ProcessReaderResult::Success) {
        return Fail(MapReaderResult(readResult));
    }

    auto objectAddress = static_cast<std::uintptr_t>(rawPointerValue);
    if (objectAddress == 0) {
        return Fail(RootResolveResult::NullPointer);
    }

    if (objectAddress != lastKnownAddress_) {
        ++nextGeneration_;
        lastKnownAddress_ = objectAddress;
    }
    current_ = ResolvedRoot{RootResolveResult::Resolved, objectAddress, nextGeneration_ - 1};
    return current_;
}

} // namespace sekiro_haptics::process
