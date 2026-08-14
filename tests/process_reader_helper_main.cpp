// Test-only helper process for Win32ProcessReader's real Win32 integration
// test (tests/test_win32_process_reader_integration.cpp) and
// AobScanner/RipRelative's real Win32 integration test
// (tests/test_aob_scanner_integration.cpp). Not part of the production
// project -- this exists purely to be a real, separate, harmless process
// with known byte patterns at known addresses for those integration tests
// to attach to and read.
//
// Protocol (line-based over stdout/stdin, no sleep on either side):
//   1. On startup, prints exactly one line to stdout:
//        READY pid=<pid> addr=<decimal address> len=<byte count>
//          aobAddr=<decimal address> aobLen=<byte count>
//          matchOffset=<byte offset of the AOB pattern within aobAddr>
//          target=<decimal address of the synthetic rel32's target>
//      and flushes -- the parent blocking on reading this line is the
//      synchronization for "the helper is ready."
//   2. Blocks reading a line from stdin. Any line (or EOF) is treated as
//      "exit now."

#include "sekiro_haptics/process/AobScanner.hpp"

#include <windows.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

// A fixed, recognizable byte pattern living in this process's own memory,
// for the integration test to read back via Win32ProcessReader::ReadBytes()
// and compare exactly.
alignas(8) unsigned char g_knownPattern[32] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x23, 0x45, 0x67,
    0x89, 0xAB, 0xCD, 0xEF, 0x10, 0x20, 0x30, 0x40,
};

// AOB scan target: large enough (well over kAobScanChunkBytes) that a
// real ScanProcessRange() call against this real process spans multiple
// internal read chunks. The unique 10-byte pattern below is deliberately
// placed straddling the first chunk boundary, so the real integration
// test exercises AobScanner's chunk-overlap handling against genuine
// ReadProcessMemory calls, not just a Fake.
//
// Pattern layout (10 bytes, matching the AOB text
// "AB CD 11 22 ?? ?? ?? ?? EF 99" used by the integration test):
//   [0]    0xAB  sentinel (exact)
//   [1]    0xCD  sentinel (exact)
//   [2]    0x11  synthetic "opcode" (exact)  <- instructionOffset = 2
//   [3]    0x22  synthetic "opcode" (exact)
//   [4..7] disp32, little-endian, signed      <- displacementOffset = 4
//          (wildcarded in the AOB pattern -- unknown until computed below)
//   [8]    0xEF  sentinel (exact)              instructionLength = 6
//   [9]    0x99  sentinel (exact)              (covers bytes [2..8))
//
// The displacement is a made-up value computed at startup so that
// instructionAddress + instructionLength + disp32 == &g_ripRelativeTarget
// exactly -- not any real instruction encoding, not any real game data.
constexpr std::size_t kAobBufferSize = 200 * 1024;
alignas(8) unsigned char g_aobBuffer[kAobBufferSize];
std::size_t g_patternOffset = 0;

alignas(8) unsigned char g_ripRelativeTarget[8] = {'T', 'A', 'R', 'G', 'E', 'T', '!', '!'};

void InitAobBuffer() {
    // Deterministic filler that can never equal any of the pattern's exact
    // bytes (0xAB, 0xCD, 0x11, 0x22, 0xEF, 0x99) -- guarantees the pattern
    // below is the buffer's only match by construction, not by chance.
    for (std::size_t i = 0; i < kAobBufferSize; ++i) {
        g_aobBuffer[i] = static_cast<unsigned char>(0x90 + (i % 7)); // 0x90..0x96
    }

    g_patternOffset = sekiro_haptics::process::kAobScanChunkBytes - 5; // straddles the first chunk boundary

    auto instructionAddress =
        reinterpret_cast<std::intptr_t>(&g_aobBuffer[g_patternOffset]) + 2; // + instructionOffset
    auto nextInstructionAddress = instructionAddress + 6;                   // + instructionLength
    auto targetAddress = reinterpret_cast<std::intptr_t>(g_ripRelativeTarget);
    auto disp32 = static_cast<std::int32_t>(targetAddress - nextInstructionAddress);

    g_aobBuffer[g_patternOffset + 0] = 0xAB;
    g_aobBuffer[g_patternOffset + 1] = 0xCD;
    g_aobBuffer[g_patternOffset + 2] = 0x11;
    g_aobBuffer[g_patternOffset + 3] = 0x22;
    g_aobBuffer[g_patternOffset + 4] = static_cast<unsigned char>(disp32 & 0xFF);
    g_aobBuffer[g_patternOffset + 5] = static_cast<unsigned char>((disp32 >> 8) & 0xFF);
    g_aobBuffer[g_patternOffset + 6] = static_cast<unsigned char>((disp32 >> 16) & 0xFF);
    g_aobBuffer[g_patternOffset + 7] = static_cast<unsigned char>((disp32 >> 24) & 0xFF);
    g_aobBuffer[g_patternOffset + 8] = 0xEF;
    g_aobBuffer[g_patternOffset + 9] = 0x99;
}

} // namespace

int main() {
    InitAobBuffer();

    std::cout << "READY pid=" << GetCurrentProcessId() << " addr="
              << reinterpret_cast<std::uintptr_t>(static_cast<void*>(g_knownPattern)) << " len=" << sizeof(g_knownPattern)
              << " aobAddr=" << reinterpret_cast<std::uintptr_t>(static_cast<void*>(g_aobBuffer))
              << " aobLen=" << kAobBufferSize << " matchOffset=" << g_patternOffset
              << " target=" << reinterpret_cast<std::uintptr_t>(static_cast<void*>(g_ripRelativeTarget)) << std::endl;
    std::cout.flush();

    std::string line;
    std::getline(std::cin, line); // blocks until the parent sends anything (or closes the pipe)
    return 0;
}
