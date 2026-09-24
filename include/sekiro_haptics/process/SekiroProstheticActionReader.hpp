#pragma once

// Which prosthetic the player just USED, read from the action the game
// requested -- not from the selection, and not from R2.
//
// WHAT WAS OBSERVED (2026-09-24, this build, one session)
// -------------------------------------------------------
// SprjChrActionRequestModule + 0xC8 holds a u32 action code. While idle it
// flickers between a few small values (790010, 790040, 0). When a prosthetic
// is used it briefly holds a code whose leading digits name the tool:
//
//     shuriken   70400000 70400010 | 70400100 70400110 70400120
//     spear      78400000          | 78400100
//     axe        73xxxxxx            (reported by the player while using the
//                                     axe; its ready code is held noticeably
//                                     longer. Exact codes not yet recorded.)
//     sword      503000x0 ...        (a different family -- never fires here)
//     R2 held    70400900            (held ~3.5 s -- an input, not a use)
//     tool swap  78412000            (seen once while switching)
//
// Found by recording the player's action-related modules through scripted
// phases (idle / shuriken / idle / shuriken / sword / movement) and keeping
// only values that appeared in BOTH shuriken phases and in NO other phase.
// The sword and movement phases are the negative controls; without them a
// value that "changes whenever the player moves" would look like a hit.
//
// READY AND ATTACK ARE DIFFERENT CODES
// ------------------------------------
// One use shows `..400000` then `..400100`. The player's reading -- and the
// evidence -- is that the first is the PREPARATION and the second the attack
// itself: with the axe, whose wind-up is long, the `..4000xx` code is held
// visibly longer before the `..4001xx` code appears. If the first were the
// attack, the axe would have nothing to wait for.
//
// So a use cue fires on the ATTACK code. Firing on the ready code would put
// the axe's impact before its swing.
//
// WHAT IS NOT KNOWN
// -----------------
//  - What the low digits (…00, …10, …20) mean. They look like combo steps;
//    nothing here depends on that reading.
//  - Whether these offsets survive a game update. The executable identity
//    gate refuses any other build.
//  - Families other than 70, 73 and 78. They are ignored rather than guessed
//    at -- a number range is not an observation.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "sekiro_haptics/process/SekiroPlayerGuardReader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sekiro_haptics::process {

struct ProstheticActionLayout {
    PlayerGuardLayout shared;
    std::uint32_t actionRequestVftableRva = 0x2A729D8;   // NS_SPRJ::SprjChrActionRequestModule
    std::size_t actionCodeOffset = 0xC8;
    std::size_t searchWidthBytes = 0x2000;
    int searchDepth = 2;
};

enum class ProstheticActionReadStatus {
    Ok,
    UnsupportedBuild,
    RootUnavailable,
    PlayerInsUnavailable,
    ModuleNotFound,
    ReadFailed,
};
const char* ToString(ProstheticActionReadStatus status);

struct ProstheticActionSample {
    ProstheticActionReadStatus status = ProstheticActionReadStatus::RootUnavailable;
    /// Bumped whenever the module is re-found (load, death, area change), so a
    /// code from before the swap is never compared with one from after it.
    std::uint64_t generation = 0;
    std::uint32_t actionCode = 0;
    bool Ok() const { return status == ProstheticActionReadStatus::Ok; }
};

struct ProstheticActionReaderStats {
    std::uint64_t polls = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    std::uint64_t moduleSearches = 0;
    std::uint64_t moduleChanges = 0;
};

class SekiroProstheticActionReader {
public:
    SekiroProstheticActionReader(IProcessReader& reader, IProcessInspector& inspector,
                                 KnownRootSpec worldChrManSpec,
                                 ExecutableIdentity expectedIdentity,
                                 ExecutableIdentity currentIdentity,
                                 std::uintptr_t moduleBaseAddress,
                                 ProstheticActionLayout layout = {});

    RootResolveResult Prime();
    ProstheticActionSample Poll();
    const ProstheticActionReaderStats& Stats() const { return stats_; }

private:
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address);
    bool HasVftable(std::uintptr_t object, std::uint32_t rva);
    /// Only from a container that ALSO holds SprjPlayerDamageModule -- the
    /// player-specific concrete type. Enemies have action-request modules
    /// too, and "the first one with the right vptr" would be one of theirs.
    std::uintptr_t FindPlayerOwnedModule(std::uintptr_t playerIns);

    IProcessReader& reader_;
    SekiroKnownRootResolver rootResolver_;
    std::uintptr_t moduleBase_;
    ProstheticActionLayout layout_;

    bool primed_ = false;
    std::uintptr_t cachedPlayerIns_ = 0;
    std::uintptr_t cachedModule_ = 0;
    std::uint64_t generation_ = 0;
    ProstheticActionReaderStats stats_;

    std::vector<std::uint8_t> scratch_;
    std::vector<std::uintptr_t> frontier_;
    std::vector<std::uintptr_t> next_;
    std::vector<std::uintptr_t> seen_;
};

// ---------------------------------------------------------------------------
// Turning codes into events. Pure: no process access, fully unit-tested.
// ---------------------------------------------------------------------------

/// The prosthetics whose action family has been OBSERVED. Anything else is
/// ignored -- including families that "look like" tools by number.
enum class ProstheticTool { Shuriken, Axe, Spear };
const char* ToString(ProstheticTool tool);

/// Leading two digits -> tool, or nothing when not observed.
std::optional<ProstheticTool> ToolForActionCode(std::uint32_t code);

enum class ProstheticPhase { None, Ready, Attack };
/// From the last five digits: 000xx is ready, 001xx is the attack, anything
/// else (the R2 hold's 00900, the swap's 12000) is neither.
ProstheticPhase PhaseForActionCode(std::uint32_t code);

struct ProstheticActionEvent {
    ProstheticTool tool = ProstheticTool::Shuriken;
    ProstheticPhase phase = ProstheticPhase::Attack;
    std::uint32_t code = 0;
    std::int64_t timestampUs = 0;
};

struct ProstheticActionObservation {
    std::int64_t timestampUs = 0;
    bool readOk = false;
    std::uint64_t generation = 0;
    std::uint32_t actionCode = 0;
};

class ProstheticActionDetector {
public:
    /// One observation in, zero or more events out.
    ///
    /// RULES (each has a test):
    ///  - The first observation, and the first after a read failure or a
    ///    generation change, is a baseline only. A code that was already
    ///    showing is not something that just happened.
    ///  - A READY event on entering a ready code.
    ///  - An ATTACK event on entering an attack code, when armed. Being
    ///    armed means a ready code has been seen since the last attack, OR
    ///    this attack code differs from the last one fired (the next step of
    ///    a chain). The game re-requests the same attack code more than once
    ///    per swing -- two pulses 10 ms apart were recorded -- and this is
    ///    how that becomes one event WITHOUT merging by time, which would
    ///    also swallow real rapid repeats.
    ///  - Nothing for unknown families, and nothing for codes that are
    ///    neither ready nor attack.
    std::vector<ProstheticActionEvent> Update(const ProstheticActionObservation& observation);

    void Reset();

private:
    bool baselined_ = false;
    std::uint64_t generation_ = 0;
    std::uint32_t lastCode_ = 0;
    bool armed_ = false;
    std::uint32_t lastAttackCode_ = 0;
};

} // namespace sekiro_haptics::process
