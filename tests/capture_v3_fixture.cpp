#include "FakeProcessReader.hpp"
#include "sekiro_haptics/process/SekiroCombatCaptureSession.hpp"
#include <cstdint>
#include <vector>
using namespace sekiro_haptics::process;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    sekiro_haptics::process::FakeProcessReader reader;
    constexpr std::uintptr_t address = 0x200000;
    std::vector<std::uint8_t> bytes(32,0);
    reader.PokeBytes(address,bytes.data(),bytes.size());
    SekiroCombatCaptureSession session(reader);
    CombatCaptureConfig config;
    config.requestedWindowSizeBytes = bytes.size();
    config.samplingInterval = std::chrono::milliseconds(10);
    if (session.Start(config,address,1,argv[1],0) != CombatCaptureStartResult::Started) return 1;
    session.Tick(address,1,10'000); // observed unchanged
    bytes[0] = 0xef; bytes[1] = 0xbe; bytes[2] = 0xad; bytes[3] = 0xde;
    reader.PokeBytes(address,bytes.data(),bytes.size());
    session.Tick(address,1,20'000); // raw high-bit u32
    session.Tick(0,1,30'000); // continuity break
    bytes[0] = 1; reader.PokeBytes(address,bytes.data(),bytes.size());
    session.Tick(address,2,40'000); // rebaseline, no cross-object delta
    session.Mark("synthetic_fixture_not_live_game",41'000,41'000);
    session.Tick(address,2,80'000); // schedule gap, rebaseline
    return session.Stop() ? 0 : 1;
}
