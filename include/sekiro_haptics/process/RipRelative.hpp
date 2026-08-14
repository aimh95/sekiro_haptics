#pragma once

// x64 RIP-relative (signed rel32 displacement) address calculation. Part
// of SEK-READ-001C -- a general building block, not tied to any specific
// instruction encoding or game. See docs/05-process-access.md for the
// formula and the range-validation contract.

#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"

#include <cstddef>
#include <cstdint>

namespace sekiro_haptics::process {

/// Describes one RIP-relative displacement to resolve, relative to a
/// pattern match's start address -- a signature match's start and the
/// actual instruction it identifies are not always the same address, so
/// both are expressed as independent offsets from `matchAddress`.
///
/// General formula:
///   instructionAddress = matchAddress + instructionOffset
///   target = instructionAddress + instructionLength + sign_extend(disp32)
/// where disp32 is the 4 little-endian bytes at
/// `matchAddress + displacementOffset`.
struct RipRelativeSpec {
    std::uintptr_t matchAddress = 0;
    std::size_t instructionOffset = 0;
    std::size_t displacementOffset = 0;
    std::size_t instructionLength = 0;
    /// The range every address this calculation touches or produces as an
    /// intermediate (instruction start, the 4 displacement bytes,
    /// next-instruction address) must lie within -- normally the same
    /// validated module range a prior ScanProcessRange() call searched.
    std::uintptr_t allowedSourceRangeBase = 0;
    std::size_t allowedSourceRangeSize = 0;
    /// The range the *computed target* must lie within. Never widened or
    /// guessed at -- a target outside this range is TargetOutOfRange, not
    /// a best-effort answer.
    std::uintptr_t allowedTargetRangeBase = 0;
    std::size_t allowedTargetRangeSize = 0;
};

/// The pure arithmetic + range-validation core: given an already-known
/// signed 32-bit displacement, computes and validates the RIP-relative
/// target. No reading of any kind happens here -- see
/// ResolveRipRelativeFromBuffer()/ResolveRipRelativeFromProcess() for the
/// two ways to obtain `displacement` in the first place.
///
/// Fails closed (outTarget left untouched on any non-Success result):
/// - `allowedSourceRangeBase`/`allowedTargetRangeBase` == 0,
///   corresponding size == 0, or `base + size` overflowing: InvalidRange.
/// - The 4 displacement bytes (at `matchAddress + displacementOffset`) not
///   lying fully within `[instructionOffset, instructionOffset +
///   instructionLength)`: InvalidDisplacementLayout.
/// - `matchAddress + instructionOffset`, `matchAddress +
///   displacementOffset + 4`, or `instructionAddress + instructionLength`
///   overflowing the address space: AddressOverflow.
/// - The instruction or its displacement bytes falling outside
///   `allowedSourceRange`: InvalidRange.
/// - `instructionAddress + instructionLength + displacement` underflowing
///   below address 0: AddressOverflow.
/// - The computed target being exactly 0, or falling outside
///   `allowedTargetRange`: TargetOutOfRange.
AobScanResult ResolveRipRelativeAddress(const RipRelativeSpec& spec, std::int32_t displacement,
                                         std::uintptr_t& outTarget);

/// Reads the 4-byte displacement directly out of `buffer` -- which must
/// represent the bytes starting exactly at `spec.matchAddress` -- then
/// delegates to ResolveRipRelativeAddress(). `spec.displacementOffset + 4`
/// exceeding `bufferSize` is InvalidDisplacementLayout (the buffer doesn't
/// even contain the declared displacement location).
AobScanResult ResolveRipRelativeFromBuffer(const std::uint8_t* buffer, std::size_t bufferSize,
                                            const RipRelativeSpec& spec, std::uintptr_t& outTarget);

/// Reads the 4-byte displacement from the attached process at
/// `spec.matchAddress + spec.displacementOffset` via `reader`, then
/// delegates to ResolveRipRelativeAddress(). The underlying
/// ProcessReaderResult (NotAttached / ProcessExited / ReadFailed /
/// PartialRead) is mapped 1:1 to the corresponding AobScanResult.
AobScanResult ResolveRipRelativeFromProcess(IProcessReader& reader, const RipRelativeSpec& spec,
                                             std::uintptr_t& outTarget);

} // namespace sekiro_haptics::process
