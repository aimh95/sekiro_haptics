#include "sekiro_haptics/process/RipRelative.hpp"

namespace sekiro_haptics::process {

AobScanResult ResolveRipRelativeAddress(const RipRelativeSpec& spec, std::int32_t displacement,
                                         std::uintptr_t& outTarget) {
    if (spec.allowedSourceRangeBase == 0 || spec.allowedSourceRangeSize == 0) {
        return AobScanResult::InvalidRange;
    }
    std::uintptr_t sourceEnd = spec.allowedSourceRangeBase + spec.allowedSourceRangeSize;
    if (sourceEnd < spec.allowedSourceRangeBase) {
        return AobScanResult::InvalidRange;
    }
    if (spec.allowedTargetRangeBase == 0 || spec.allowedTargetRangeSize == 0) {
        return AobScanResult::InvalidRange;
    }
    std::uintptr_t targetEnd = spec.allowedTargetRangeBase + spec.allowedTargetRangeSize;
    if (targetEnd < spec.allowedTargetRangeBase) {
        return AobScanResult::InvalidRange;
    }

    // The 4 displacement bytes must lie fully inside the instruction.
    if (spec.displacementOffset < spec.instructionOffset) {
        return AobScanResult::InvalidDisplacementLayout;
    }
    std::size_t displacementEndWithinInstruction = (spec.displacementOffset - spec.instructionOffset) + 4;
    if (displacementEndWithinInstruction > spec.instructionLength) {
        return AobScanResult::InvalidDisplacementLayout;
    }

    // Instruction start address, checked for overflow and range membership.
    std::uintptr_t instructionAddress = spec.matchAddress + spec.instructionOffset;
    if (instructionAddress < spec.matchAddress) {
        return AobScanResult::AddressOverflow;
    }
    if (instructionAddress < spec.allowedSourceRangeBase || instructionAddress > sourceEnd) {
        return AobScanResult::InvalidRange;
    }

    // Displacement bytes' own address, checked for overflow and range membership.
    std::uintptr_t displacementAddress = spec.matchAddress + spec.displacementOffset;
    if (displacementAddress < spec.matchAddress) {
        return AobScanResult::AddressOverflow;
    }
    std::uintptr_t displacementEnd = displacementAddress + 4;
    if (displacementEnd < displacementAddress) {
        return AobScanResult::AddressOverflow;
    }
    if (displacementAddress < spec.allowedSourceRangeBase || displacementEnd > sourceEnd) {
        return AobScanResult::InvalidRange;
    }

    // Next-instruction address (the RIP-relative base), checked for
    // overflow and range membership.
    std::uintptr_t nextInstructionAddress = instructionAddress + spec.instructionLength;
    if (nextInstructionAddress < instructionAddress) {
        return AobScanResult::AddressOverflow;
    }
    if (nextInstructionAddress > sourceEnd) {
        return AobScanResult::InvalidRange;
    }

    // target = nextInstructionAddress + sign_extend(displacement), computed
    // in a wider signed type so a large negative displacement never wraps
    // an unsigned pointer type around instead of being caught as an
    // underflow.
    std::int64_t target64 = static_cast<std::int64_t>(nextInstructionAddress) + static_cast<std::int64_t>(displacement);
    if (target64 < 0) {
        return AobScanResult::AddressOverflow; // negative underflow
    }

    std::uintptr_t target = static_cast<std::uintptr_t>(target64);
    if (target == 0) {
        return AobScanResult::TargetOutOfRange;
    }
    if (target < spec.allowedTargetRangeBase || target >= targetEnd) {
        return AobScanResult::TargetOutOfRange;
    }

    outTarget = target;
    return AobScanResult::Success;
}

AobScanResult ResolveRipRelativeFromBuffer(const std::uint8_t* buffer, std::size_t bufferSize,
                                            const RipRelativeSpec& spec, std::uintptr_t& outTarget) {
    if (spec.displacementOffset + 4 < spec.displacementOffset || spec.displacementOffset + 4 > bufferSize) {
        return AobScanResult::InvalidDisplacementLayout;
    }

    std::uint32_t raw = static_cast<std::uint32_t>(buffer[spec.displacementOffset]) |
                         (static_cast<std::uint32_t>(buffer[spec.displacementOffset + 1]) << 8) |
                         (static_cast<std::uint32_t>(buffer[spec.displacementOffset + 2]) << 16) |
                         (static_cast<std::uint32_t>(buffer[spec.displacementOffset + 3]) << 24);
    return ResolveRipRelativeAddress(spec, static_cast<std::int32_t>(raw), outTarget);
}

AobScanResult ResolveRipRelativeFromProcess(IProcessReader& reader, const RipRelativeSpec& spec,
                                             std::uintptr_t& outTarget) {
    std::uintptr_t displacementAddress = spec.matchAddress + spec.displacementOffset;
    if (displacementAddress < spec.matchAddress) {
        return AobScanResult::AddressOverflow;
    }

    std::uint8_t displacementBytes[4] = {};
    ProcessReaderResult readResult = reader.ReadBytes(displacementAddress, displacementBytes, sizeof(displacementBytes));
    switch (readResult) {
        case ProcessReaderResult::Success:
            break;
        case ProcessReaderResult::NotAttached:
            return AobScanResult::NotAttached;
        case ProcessReaderResult::ProcessExited:
            return AobScanResult::ProcessExited;
        case ProcessReaderResult::PartialRead:
            return AobScanResult::PartialRead;
        default:
            return AobScanResult::ReadFailed;
    }

    std::uint32_t raw = static_cast<std::uint32_t>(displacementBytes[0]) |
                         (static_cast<std::uint32_t>(displacementBytes[1]) << 8) |
                         (static_cast<std::uint32_t>(displacementBytes[2]) << 16) |
                         (static_cast<std::uint32_t>(displacementBytes[3]) << 24);
    return ResolveRipRelativeAddress(spec, static_cast<std::int32_t>(raw), outTarget);
}

} // namespace sekiro_haptics::process
