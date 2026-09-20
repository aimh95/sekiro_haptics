#pragma once

// Reads the player's guard-resolution signal live, so the C++ runtime can
// drive output from it. This is the realtime counterpart of
// tools/deflect_occurrence_record.py and carries the SAME two corrections
// that tool needed -- both were found by a recording that silently produced
// zero events for three minutes (docs/astra/results/DEFLECT_STATUS.md 7.8):
//
//   BUG 1  A vptr check alone does not prove an object is LIVE. After an area
//          transition the freed allocation still holds the old vftable
//          pointer, so a cached module keeps passing its type check while the
//          game has moved on. Fix: PlayerIns is re-read on EVERY poll and the
//          module cache is dropped the moment it changes.
//
//   BUG 2  "the first object with the wanted vptr" is arbitrary -- it depends
//          on traversal order. Fix: the module is only accepted from a
//          container that ALSO holds a SprjPlayerDamageModule, the
//          player-specific concrete type. That is the same type-level
//          ownership proof used in DEFLECT_STATUS.md 6.5.
//
// The signal itself (DEFLECT_STATUS.md 7.6/7.9):
//   SprjChrActionFlagModule + 0x3C   one-frame (~15 ms) pulse, once per guard
//                                    resolution, including repeats
//   SprjChrActionFlagModule + 0xE10  0 = normal block, 1 = deflect
//
// Because the pulse lasts about one frame, the caller must poll at a few
// milliseconds. This class does not schedule itself; it reports what it read
// and the caller records the interval actually achieved.
//
// OS-independent: only IProcessReader / IProcessInspector, like the rest of
// process/. No Win32 here.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sekiro_haptics::process {

/// Every address-shaped constant, so nothing is buried in the code and a new
/// game build can be handled by changing data. Defaults are the values
/// verified on this project's build (sha256 637aca52...b856).
struct PlayerGuardLayout {
    std::int64_t playerInsOffset = 0x88;
    std::uint32_t worldChrManVftableRva = 0x2A30058;
    std::uint32_t playerInsVftableRva = 0x2A2B338;
    std::uint32_t actionFlagVftableRva = 0x2A72158;
    std::uint32_t playerDamageVftableRva = 0x2A7DA48;
    std::size_t pulseOffset = 0x3C;
    std::size_t outcomeOffset = 0xE10;
    /// Bytes of each candidate object walked while searching.
    std::size_t searchWidthBytes = 0x2000;
    /// Hops below PlayerIns. 2 was enough on every observed layout.
    int searchDepth = 2;
};

enum class GuardReadStatus {
    Ok,
    /// The build's identity gate refused -- no scan was attempted.
    UnsupportedBuild,
    /// WorldChrMan slot null or its vptr did not match (title screen, loading).
    RootUnavailable,
    /// PlayerIns null or wrong type.
    PlayerInsUnavailable,
    /// No player-owned container holding the module was reachable.
    ModuleNotFound,
    /// The module resolved but its bytes could not be read.
    ReadFailed,
};
const char* ToString(GuardReadStatus status);

struct PlayerGuardSample {
    GuardReadStatus status = GuardReadStatus::RootUnavailable;
    std::uintptr_t moduleAddress = 0;
    /// Increments whenever the resolved module instance changes. Values from
    /// two different generations must never be compared.
    std::uint64_t generation = 0;
    std::uint8_t pulse = 0;
    std::uint8_t outcome = 0;
    bool Ok() const { return status == GuardReadStatus::Ok; }
};

struct PlayerGuardReaderStats {
    std::uint64_t polls = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    /// How many times the module had to be searched for again. A healthy
    /// session searches once per player-object replacement, not per poll.
    std::uint64_t moduleSearches = 0;
    std::uint64_t playerInsChanges = 0;
    std::uint64_t moduleChanges = 0;
    /// How many player-owned containers agreed on the chosen module, out of how
    /// many distinct modules were offered. A lone candidate winning 1-of-2 means
    /// the layout changed and the choice deserves a second look.
    int lastVoteCount = 0;
    int lastVoteCandidates = 0;
};

class SekiroPlayerGuardReader {
public:
    /// `moduleBaseAddress` is the attached process's main-module base, used to
    /// turn the vftable RVAs above into runtime addresses.
    SekiroPlayerGuardReader(IProcessReader& reader, IProcessInspector& inspector,
                            KnownRootSpec worldChrManSpec, ExecutableIdentity expectedIdentity,
                            ExecutableIdentity currentIdentity, std::uintptr_t moduleBaseAddress,
                            PlayerGuardLayout layout = {});

    /// One AOB scan for the WorldChrMan pointer slot. Call once after attach;
    /// Poll() afterwards only re-reads the slot.
    RootResolveResult Prime();

    /// One complete read. Never returns a value from an object it did not
    /// re-validate on this call.
    PlayerGuardSample Poll();

    const PlayerGuardReaderStats& Stats() const { return stats_; }

    /// Forget everything cached (device loss, deliberate re-attach).
    void Invalidate();

private:
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address);
    bool HasVftable(std::uintptr_t object, std::uint32_t rva);
    std::optional<std::uintptr_t> FindPlayerOwnedModule(std::uintptr_t playerIns);

    IProcessReader& reader_;
    SekiroKnownRootResolver rootResolver_;
    std::uintptr_t moduleBase_;
    PlayerGuardLayout layout_;

    bool primed_ = false;
    std::uintptr_t cachedPlayerIns_ = 0;
    std::uintptr_t cachedModule_ = 0;
    std::uint64_t generation_ = 0;
    PlayerGuardReaderStats stats_;
    int lastVoteCount_ = 0;
    int lastVoteCandidates_ = 0;

    // Reused across polls so a 5 ms loop does not allocate.
    std::vector<std::uint8_t> scratch_;
    std::vector<std::uintptr_t> frontier_;
    std::vector<std::uintptr_t> next_;
    std::vector<std::uintptr_t> seen_;
};

} // namespace sekiro_haptics::process
