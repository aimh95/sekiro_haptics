// Test-only Linux helper process for SEK-PROBE-001C-LINUX-E2E: a real,
// separate, harmless child process with a known mmap'd anonymous private
// arena containing a handful of known u32 values at known offsets, for the
// disk-backed candidate scanner's real Linux end-to-end test
// (test_disk_candidate_scanner_linux_e2e.cpp) and the manual E2E runner
// (linux_probe_e2e_main.cpp) to attach to and read via process_vm_readv.
// Not part of the production project, and not a stand-in for Sekiro or any
// other real game -- this never becomes a Linux/Proton product backend.
//
// Usage: linux_signal_probe_helper <arenaSizeBytes>
//
// Protocol (line-based over stdout/stdin, no sleep on either side):
//   1. On startup, prints exactly one line to stdout:
//        READY pid=<pid> arenaBase=<decimal address> arenaSize=<byte count>
//          hpAddr=<decimal address> decoyAddr=<decimal address>
//          stableAddr=<decimal address> noiseAddr=<decimal address>
//      and flushes -- the parent blocking on reading this line is the
//      synchronization for "the helper is ready."
//   2. Loops reading commands from stdin, one per line, each applied
//      synchronously before printing exactly one "ACK <command>"
//      acknowledgement line (the parent blocking on reading that line is
//      the synchronization for "the command has been applied" -- no sleep
//      on either side):
//        DAMAGE      -- playerHp -= 100, decoyDecreasingValue -= 100
//        NOISE_ONLY  -- decoyDecreasingValue += 1, unrelatedNoise += 1
//                       (playerHp untouched)
//        HEAL        -- playerHp += 50 (decoyDecreasingValue untouched)
//        STATUS      -- no value change; ACK line carries current values
//        EXIT        -- exits immediately (no ACK printed)

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr std::size_t kDefaultArenaSizeBytes = 64ULL * 1024 * 1024;

volatile std::uint32_t* g_arena = nullptr;

// Fixed offsets into the arena -- all naturally 4-byte aligned since
// mmap() itself always returns a page-aligned (and therefore 4-byte
// aligned) base address.
constexpr std::size_t kHpOffset = 0;
constexpr std::size_t kDecoyOffset = 4;
constexpr std::size_t kStableOffset = 8;
constexpr std::size_t kNoiseOffset = 12;

volatile std::uint32_t& HpValue() {
    return *reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<volatile std::uint8_t*>(g_arena) + kHpOffset);
}
volatile std::uint32_t& DecoyValue() {
    return *reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<volatile std::uint8_t*>(g_arena) +
                                                        kDecoyOffset);
}
volatile std::uint32_t& StableValue() {
    return *reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<volatile std::uint8_t*>(g_arena) +
                                                        kStableOffset);
}
volatile std::uint32_t& NoiseValue() {
    return *reinterpret_cast<volatile std::uint32_t*>(reinterpret_cast<volatile std::uint8_t*>(g_arena) +
                                                        kNoiseOffset);
}

void PrintAck(const std::string& command) {
    std::cout << "ACK " << command << " hp=" << HpValue() << " decoy=" << DecoyValue() << " stable=" << StableValue()
              << " noise=" << NoiseValue() << std::endl;
    std::cout.flush();
}

} // namespace

int main(int argc, char** argv) {
    std::size_t arenaSizeBytes = kDefaultArenaSizeBytes;
    if (argc > 1) {
        try {
            arenaSizeBytes = static_cast<std::size_t>(std::stoull(argv[1]));
        } catch (...) {
            std::cerr << "invalid arena size argument\n";
            return 1;
        }
    }
    if (arenaSizeBytes < 4096) {
        arenaSizeBytes = 4096; // at least one page -- must hold 4 u32 values plus real scan content
    }

    void* mapped = mmap(nullptr, arenaSizeBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        return 1;
    }
    g_arena = static_cast<volatile std::uint32_t*>(mapped);

    // Anonymous mmap pages are guaranteed zero-filled by the kernel -- the
    // rest of the arena stays exactly zero except these four values, so a
    // "decreased"/"increased"/"unchanged" filter pass against the whole
    // arena has an unambiguous, deterministic set of survivors.
    HpValue() = 1000;
    DecoyValue() = 500;
    StableValue() = 77;
    NoiseValue() = 10;

    std::cout << "READY pid=" << getpid() << " arenaBase=" << reinterpret_cast<std::uintptr_t>(mapped)
              << " arenaSize=" << arenaSizeBytes
              << " hpAddr=" << reinterpret_cast<std::uintptr_t>(const_cast<std::uint32_t*>(&HpValue()))
              << " decoyAddr=" << reinterpret_cast<std::uintptr_t>(const_cast<std::uint32_t*>(&DecoyValue()))
              << " stableAddr=" << reinterpret_cast<std::uintptr_t>(const_cast<std::uint32_t*>(&StableValue()))
              << " noiseAddr=" << reinterpret_cast<std::uintptr_t>(const_cast<std::uint32_t*>(&NoiseValue()))
              << std::endl;
    std::cout.flush();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "EXIT") {
            break;
        }
        if (line == "DAMAGE") {
            HpValue() = HpValue() - 100;
            DecoyValue() = DecoyValue() - 100;
        } else if (line == "NOISE_ONLY") {
            DecoyValue() = DecoyValue() + 1;
            NoiseValue() = NoiseValue() + 1;
        } else if (line == "HEAL") {
            HpValue() = HpValue() + 50;
        } else if (line == "STATUS") {
            // No value change -- ACK carries current values, see PrintAck().
        } else {
            // Unknown command: still ACK (with the unrecognized text) so a
            // misbehaving caller doesn't hang forever waiting for a reply.
            PrintAck(line);
            continue;
        }
        PrintAck(line);
    }
    return 0;
}
