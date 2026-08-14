// Unit tests for RIP-relative (signed rel32) address resolution:
// ResolveRipRelativeAddress() (pure arithmetic/validation, given an
// already-known displacement), ResolveRipRelativeFromBuffer() (extracts
// the displacement from an in-memory buffer), and
// ResolveRipRelativeFromProcess() (extracts it via IProcessReader, driven
// through FakeProcessReader here -- no real process).

#include "sekiro_haptics/process/RipRelative.hpp"
#include "testing.hpp"

#include "FakeProcessReader.hpp"

#include <cstdint>
#include <limits>

using namespace sekiro_haptics::process;

// ===========================================================================
// ResolveRipRelativeAddress: pure arithmetic + range validation
// ===========================================================================

SH_TEST(ResolveRipRelativeAddress_PositiveDisplacement_ComputesCorrectTarget) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1010;
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x5000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0x40E9, target); // nextInstr=0x1017, target=0x5100

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == 0x5100u);
}

SH_TEST(ResolveRipRelativeAddress_NegativeDisplacement_ComputesCorrectTarget) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x9010;
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x9000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x2000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    // nextInstr = 0x9017, target should land at 0x2500 -> displacement = 0x2500 - 0x9017
    std::int32_t displacement = static_cast<std::int32_t>(0x2500) - static_cast<std::int32_t>(0x9017);
    AobScanResult result = ResolveRipRelativeAddress(spec, displacement, target);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == 0x2500u);
    SH_CHECK(displacement < 0);
}

SH_TEST(ResolveRipRelativeAddress_ZeroDisplacement_TargetIsNextInstructionAddress) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1010;
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == 0x1017u); // matchAddress + instructionOffset(0) + instructionLength(7)
}

SH_TEST(ResolveRipRelativeAddress_InstructionOffsetDistinctFromMatchAddress_AppliedCorrectly) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0x10; // instruction does not start at the match's own address
    spec.displacementOffset = 0x13;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::Success);
    // instructionAddress = 0x1010, + instructionLength(7) = 0x1017
    SH_CHECK(target == 0x1017u);
}

SH_TEST(ResolveRipRelativeAddress_DisplacementOffsetAppliedCorrectly_ViaBuffer) {
    // Two specs differing only in displacementOffset must read different
    // bytes -- proven by giving each offset a distinct, recognizable value.
    std::uint8_t buffer[16] = {};
    // displacementOffset 2: bytes [2..6) = 0x00000010 (16)
    buffer[2] = 0x10;
    buffer[3] = 0x00;
    buffer[4] = 0x00;
    buffer[5] = 0x00;
    // displacementOffset 6: bytes [6..10) = 0x00000020 (32)
    buffer[6] = 0x20;
    buffer[7] = 0x00;
    buffer[8] = 0x00;
    buffer[9] = 0x00;

    RipRelativeSpec specA;
    specA.matchAddress = 0x1000;
    specA.instructionOffset = 0;
    specA.displacementOffset = 2;
    specA.instructionLength = 6;
    specA.allowedSourceRangeBase = 0x1000;
    specA.allowedSourceRangeSize = 0x1000;
    specA.allowedTargetRangeBase = 0x1000;
    specA.allowedTargetRangeSize = 0x1000;

    RipRelativeSpec specB = specA;
    specB.displacementOffset = 6;
    specB.instructionLength = 10;

    std::uintptr_t targetA = 0;
    std::uintptr_t targetB = 0;
    SH_CHECK(ResolveRipRelativeFromBuffer(buffer, sizeof(buffer), specA, targetA) == AobScanResult::Success);
    SH_CHECK(ResolveRipRelativeFromBuffer(buffer, sizeof(buffer), specB, targetB) == AobScanResult::Success);

    SH_CHECK(targetA == 0x1000u + 6 + 16);  // instructionAddress(0x1000) + instructionLength(6) + 16
    SH_CHECK(targetB == 0x1000u + 10 + 32); // instructionAddress(0x1000) + instructionLength(10) + 32
}

SH_TEST(ResolveRipRelativeFromBuffer_LittleEndianPositive_InterpretedCorrectly) {
    std::uint8_t buffer[8] = {0, 0, 0, 0, 0x00, 0x01, 0x00, 0x00}; // disp32 at [4..8) = 0x00000100 = 256
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0;
    spec.displacementOffset = 4;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    SH_CHECK(ResolveRipRelativeFromBuffer(buffer, sizeof(buffer), spec, target) == AobScanResult::Success);
    SH_CHECK(target == 0x1000u + 8 + 256);
}

SH_TEST(ResolveRipRelativeFromBuffer_LittleEndianNegative_InterpretedCorrectly) {
    std::uint8_t buffer[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}; // disp32 = -1
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0;
    spec.displacementOffset = 4;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    SH_CHECK(ResolveRipRelativeFromBuffer(buffer, sizeof(buffer), spec, target) == AobScanResult::Success);
    SH_CHECK(target == 0x1000u + 8 - 1);
}

SH_TEST(ResolveRipRelativeAddress_DisplacementBytesEndExactlyAtSourceRangeEnd_Succeeds) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1FF9; // instruction [0x1FF9, 0x2000): displacement [0x1FFC, 0x2000)
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000; // source ends exactly at 0x2000
    spec.allowedTargetRangeBase = 0x2000; // target (nextInstr + 0 = 0x2000) must itself be in-range
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == 0x2000u);
}

SH_TEST(ResolveRipRelativeAddress_DisplacementOutsideInstruction_ReturnsInvalidDisplacementLayout) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0;
    spec.displacementOffset = 10; // instruction only covers [0,6)
    spec.instructionLength = 6;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::InvalidDisplacementLayout);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_DisplacementBeforeInstructionStart_ReturnsInvalidDisplacementLayout) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 5;
    spec.displacementOffset = 2; // before instructionOffset
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::InvalidDisplacementLayout);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_SourceAddressOverflow_ReturnsAddressOverflow) {
    RipRelativeSpec spec;
    spec.matchAddress = std::numeric_limits<std::uintptr_t>::max() - 2;
    spec.instructionOffset = 10; // matchAddress + instructionOffset overflows
    spec.displacementOffset = 12;
    spec.instructionLength = 16;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::AddressOverflow);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_NextInstructionAddressOverflow_ReturnsAddressOverflow) {
    RipRelativeSpec spec;
    // The source range itself borders the top of the address space (but
    // doesn't overflow), and matchAddress/instructionAddress legitimately
    // fall inside it -- only instructionAddress + instructionLength
    // overflows past the top of the 64-bit address space.
    std::uintptr_t maxAddr = std::numeric_limits<std::uintptr_t>::max();
    spec.allowedSourceRangeBase = maxAddr - 0x2000;
    spec.allowedSourceRangeSize = 0x1000; // source = [max-0x2000, max-0x1000)
    spec.matchAddress = maxAddr - 0x1500; // within the source range
    spec.instructionOffset = 0;
    spec.displacementOffset = 0;
    spec.instructionLength = 0x2000; // instructionAddress + this overflows past `maxAddr`
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    AobScanResult result = ResolveRipRelativeAddress(spec, 0, target);

    SH_CHECK(result == AobScanResult::AddressOverflow);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_NegativeUnderflow_ReturnsAddressOverflow) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x100;
    spec.instructionOffset = 0;
    spec.displacementOffset = 0;
    spec.instructionLength = 4;
    spec.allowedSourceRangeBase = 0x100;
    spec.allowedSourceRangeSize = 0x100;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    // nextInstructionAddress = 0x104; a large negative displacement drives
    // the target below address 0.
    AobScanResult result = ResolveRipRelativeAddress(spec, -0x1000, target);

    SH_CHECK(result == AobScanResult::AddressOverflow);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_TargetOutsideAllowedRange_ReturnsTargetOutOfRange) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1010;
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x5000;
    spec.allowedTargetRangeSize = 0x1000; // target range [0x5000, 0x6000)

    std::uintptr_t target = 0xDEAD;
    // nextInstr = 0x1017, displacement chosen so target lands well outside [0x5000,0x6000)
    AobScanResult result = ResolveRipRelativeAddress(spec, 0x100, target); // target = 0x1117

    SH_CHECK(result == AobScanResult::TargetOutOfRange);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeAddress_TargetIsExactlyZero_ReturnsTargetOutOfRange) {
    RipRelativeSpec spec;
    spec.matchAddress = 0x1010;
    spec.instructionOffset = 0;
    spec.displacementOffset = 3;
    spec.instructionLength = 7;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x2000; // deliberately wide enough that a naive check might let 0 through

    std::uintptr_t target = 0xDEAD;
    // nextInstr = 0x1017; displacement = -0x1017 drives the target to exactly 0.
    AobScanResult result = ResolveRipRelativeAddress(spec, -static_cast<std::int32_t>(0x1017), target);

    SH_CHECK(result == AobScanResult::TargetOutOfRange);
    SH_CHECK(target == 0xDEADu);
}

// ===========================================================================
// ResolveRipRelativeFromBuffer: layout/size guard
// ===========================================================================

SH_TEST(ResolveRipRelativeFromBuffer_DisplacementBeyondBufferEnd_ReturnsInvalidDisplacementLayout) {
    std::uint8_t buffer[6] = {}; // too short to contain a 4-byte displacement at offset 4
    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0;
    spec.displacementOffset = 4;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    AobScanResult result = ResolveRipRelativeFromBuffer(buffer, sizeof(buffer), spec, target);

    SH_CHECK(result == AobScanResult::InvalidDisplacementLayout);
    SH_CHECK(target == 0xDEADu);
}

// ===========================================================================
// ResolveRipRelativeFromProcess: driven through FakeProcessReader
// ===========================================================================

SH_TEST(ResolveRipRelativeFromProcess_Success_ReadsDisplacementAndComputesTarget) {
    FakeProcessReader reader;
    std::uint8_t dispBytes[4] = {0x00, 0x01, 0x00, 0x00}; // 256
    reader.PokeBytes(0x1004, dispBytes, sizeof(dispBytes));

    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionOffset = 0;
    spec.displacementOffset = 4;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0;
    AobScanResult result = ResolveRipRelativeFromProcess(reader, spec, target);

    SH_CHECK(result == AobScanResult::Success);
    SH_CHECK(target == 0x1000u + 8 + 256);
}

SH_TEST(ResolveRipRelativeFromProcess_NotAttached_ReturnsNotAttached) {
    FakeProcessReader reader;
    reader.SetAttached(false);

    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    SH_CHECK(ResolveRipRelativeFromProcess(reader, spec, target) == AobScanResult::NotAttached);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeFromProcess_ProcessExited_ReturnsProcessExited) {
    FakeProcessReader reader;
    reader.SetAlive(false);

    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    SH_CHECK(ResolveRipRelativeFromProcess(reader, spec, target) == AobScanResult::ProcessExited);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeFromProcess_ReadFailure_ReturnsReadFailed) {
    FakeProcessReader reader;
    reader.FailReadAtCall(0);

    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    SH_CHECK(ResolveRipRelativeFromProcess(reader, spec, target) == AobScanResult::ReadFailed);
    SH_CHECK(target == 0xDEADu);
}

SH_TEST(ResolveRipRelativeFromProcess_PartialRead_ReturnsPartialRead) {
    FakeProcessReader reader;
    reader.ForcePartialReadAtCall(0, 2);

    RipRelativeSpec spec;
    spec.matchAddress = 0x1000;
    spec.instructionLength = 8;
    spec.allowedSourceRangeBase = 0x1000;
    spec.allowedSourceRangeSize = 0x1000;
    spec.allowedTargetRangeBase = 0x1000;
    spec.allowedTargetRangeSize = 0x1000;

    std::uintptr_t target = 0xDEAD;
    SH_CHECK(ResolveRipRelativeFromProcess(reader, spec, target) == AobScanResult::PartialRead);
    SH_CHECK(target == 0xDEADu);
}
