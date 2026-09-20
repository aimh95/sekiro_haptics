#pragma once

// The same guard-resolution signal, read from ENEMY characters instead of the
// player: "an enemy just blocked / deflected".
//
// WHY THIS IS THE SAME SIGNAL
// ---------------------------
// SprjChrActionFlagModule is a single class -- the static vftable map has one
// entry for it (RVA 0x2A72158), not a player variant and an enemy variant. So
// the two fields the player reader uses are fields of that one class:
//
//   SprjChrActionFlagModule + 0x3C   one-frame pulse, once per resolution
//   SprjChrActionFlagModule + 0xE10  0 = normal block, 1 = deflect
//
// Confirmed live on this build (DEFLECT_STATUS.md 8.2): 19 events in 60 s
// while attacking one enemy, with BLOCK/DEFLECT alternating exactly as the
// fight looked, and 0 events from the other 143 tracked objects.
//
// OWNERSHIP: ONE STRUCTURAL PATH, MEASURED
// ----------------------------------------
// The first version walked the graph for "an ActionFlagModule reachable from
// this character within two hops", the way the player reader does. On the
// player that is safe -- there is one player. On enemies it is NOT: a second
// hop leaves the character and lands on a NEIGHBOUR, so 104 characters
// resolved to only 75 modules with 24 of them shared between characters.
//
// A shared module is worse than no module. The character you are hitting ends
// up reading someone else's object, so your own hits are missed, while that
// other character's fight is reported under two identities. That is exactly
// the "nearby enemies register but my attacks do not" symptom.
//
// So the path is a single offset PAIR -- ChrIns + a -> container + b -- and it
// is DERIVED FROM THE PLAYER, the one character whose module can be resolved
// by a method already proven correct (a container holding both an
// ActionFlagModule and a SprjPlayerDamageModule).
//
// Deriving it from a sample of enemies instead was tried and is not safe: the
// winning pair depends on which characters land in the sample, and two runs
// picked different pairs (0x1B58/0xD0 and 0x1FF8/0x0). The pair is also NOT a
// fixed build constant -- it moved between runs -- so it cannot be hardcoded
// either. Re-deriving it from the player on every discovery pass handles both.
//
// Several pairs reach the player's module; the one that resolves the most
// enemies to DISTINCT modules wins. Any module two characters still claim is
// dropped from both: there is no way to tell which one is right.
//
// WHAT THIS CANNOT TELL YOU
// -------------------------
// +0xE10 says THIS CHARACTER guarded. It does not say whose attack was
// guarded. An enemy blocking another enemy, or blocking a shuriken rather
// than the sword, is indistinguishable here. A 60 s negative control with no
// player attacks produced 0 events, which is encouraging but is not proof --
// the enemies in view may simply never have guarded. Callers must not
// describe these events as "my attack was parried".
//
// OS-independent: IProcessReader / IProcessInspector only, like the rest of
// process/.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "sekiro_haptics/process/SekiroPlayerGuardReader.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace sekiro_haptics::process {

/// Address-shaped constants for the enemy side. The shared ones (pulse and
/// outcome offsets, the ActionFlagModule vftable) are reused from
/// PlayerGuardLayout rather than retyped, because they are the same class.
struct EnemyGuardLayout {
    PlayerGuardLayout shared;
    std::uint32_t enemyInsVftableRva = 0x2A27F28;
    std::uint32_t enemyDamageVftableRva = 0x2A7D628;
    /// Bound on the WorldChrMan walk. The enemy list's offset has never been
    /// verified on this build, so the characters are found by type instead of
    /// by a guessed field -- slower, but it cannot be quietly wrong about what
    /// a field means.
    std::size_t discoveryBudget = 40'000;
    /// The player-specific damage module, used to prove which module is the
    /// PLAYER's before that answer is used to derive the path.
    std::uint32_t playerDamageVftableRva = 0x2A7DA48;
    /// Bytes of the module container scanned while calibrating.
    std::size_t containerScanBytes = 0x800;
};

/// One tracked enemy's current reading.
struct EnemyGuardSample {
    std::uintptr_t character = 0;      ///< the EnemyIns
    std::uintptr_t moduleAddress = 0;  ///< its SprjChrActionFlagModule
    /// Bumped when this character's module is replaced, so values either side
    /// of a swap are never compared.
    std::uint64_t generation = 0;
    std::uint8_t pulse = 0;
    std::uint8_t outcome = 0;
};

/// Diagnostics only. Poll() and the background Discover() both touch these
/// counters without synchronisation; on x86-64 an aligned 64-bit store is not
/// torn, and nothing here feeds a decision, so they are left unlocked rather
/// than putting a mutex in the 5 ms path for numbers that are only printed.
struct EnemyGuardReaderStats {
    std::uint64_t polls = 0;
    std::uint64_t discoveries = 0;
    std::uint64_t charactersTracked = 0;
    std::uint64_t readFailures = 0;
    /// Raw 0 -> non-zero edges on +0x3C, counted BEFORE any detector sees
    /// them. This is what separates "the reader never saw a pulse" from "the
    /// reader saw pulses but the detector emitted nothing" -- two very
    /// different bugs that both show up as zero events.
    std::uint64_t rawPulseEdges = 0;
    std::uint64_t nonZeroPulseSamples = 0;
    /// How many times the derived offset path actually changed. A path that
    /// keeps changing means the derivation is unstable, not that the game is.
    std::uint64_t pathChanges = 0;
    /// Characters dropped because a second character claimed the same module.
    std::uint64_t ambiguousModules = 0;
    /// The calibrated path and how many sampled characters supported it.
    bool pathKnown = false;
    std::size_t pathCharacterOffset = 0;
    std::size_t pathContainerOffset = 0;
    std::size_t pathSupport = 0;
    /// Characters dropped because their module stopped being an
    /// ActionFlagModule -- unloaded or recycled.
    std::uint64_t charactersDropped = 0;
    std::int64_t lastDiscoveryUs = 0;
};

class SekiroEnemyGuardReader {
public:
    SekiroEnemyGuardReader(IProcessReader& reader, IProcessInspector& inspector,
                           KnownRootSpec worldChrManSpec, ExecutableIdentity expectedIdentity,
                           ExecutableIdentity currentIdentity, std::uintptr_t moduleBaseAddress,
                           EnemyGuardLayout layout = {});

    /// One AOB scan for the WorldChrMan slot.
    RootResolveResult Prime();

    /// Rebuilds the tracked set. EXPENSIVE -- measured at ~5.7 s, because it
    /// walks the character graph reading 8 KB per node. Call it on attach, or
    /// let StartBackgroundDiscovery() do it; calling it from the poll loop
    /// pushed the measured poll interval from 5 ms to 4.2 SECONDS.
    std::size_t Discover();

    /// Runs Discover() forever on its own thread, handing each finished set
    /// over under a lock. This is how the tracked set stays current without
    /// the detection loop ever waiting for a walk: enemies that load in later
    /// get picked up on the next pass instead of being missed for the rest of
    /// the session.
    void StartBackgroundDiscovery(float intervalSeconds);
    void StopBackgroundDiscovery();

    /// True when the world moved under us (area change, death, reload) and
    /// Discover() should run again. Cheap: two pointer reads.
    bool NeedsDiscovery();

    /// Cheap per-poll read of every tracked character. Two byte reads each,
    /// and the outcome byte is only read when the pulse is actually set --
    /// reading both for every tracked character pushed the poll interval past
    /// one frame, which is longer than the pulse being looked for.
    const std::vector<EnemyGuardSample>& Poll();

    const EnemyGuardReaderStats& Stats() const { return stats_; }
    void Invalidate();

private:
    struct Tracked {
        std::uintptr_t character = 0;
        std::uintptr_t module = 0;
        std::uint64_t generation = 0;
        std::uint8_t lastOutcome = 0;
        std::uint8_t lastPulse = 0;
    };

    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address);
    bool HasVftable(std::uintptr_t object, std::uint32_t rva);
    /// Resolves the PLAYER's module the proven way, then picks the offset pair
    /// that both reaches it and resolves the most enemies uniquely.
    bool Calibrate(std::uintptr_t playerIns, const std::vector<std::uintptr_t>& characters);
    std::optional<std::uintptr_t> PlayerModule(std::uintptr_t playerIns);
    /// Two reads plus a vptr check. Never searches.
    std::optional<std::uintptr_t> ModuleAtPath(std::uintptr_t character);

    IProcessReader& reader_;
    SekiroKnownRootResolver rootResolver_;
    std::uintptr_t moduleBase_;
    EnemyGuardLayout layout_;

    bool primed_ = false;
    std::uintptr_t lastWorld_ = 0;
    std::uintptr_t lastPlayerIns_ = 0;
    std::uint64_t generation_ = 0;
    /// Guards `tracked_` only, and is held just long enough to swap or to
    /// copy out -- never across a walk.
    mutable std::mutex trackedMutex_;
    std::vector<Tracked> tracked_;
    std::vector<Tracked> pollCopy_;
    std::vector<EnemyGuardSample> samples_;
    std::thread discoveryThread_;
    std::atomic<bool> discoveryRunning_{false};
    float discoveryIntervalSeconds_ = 4.0f;
    EnemyGuardReaderStats stats_;

    // Reused so a 5 ms loop does not allocate.
    std::vector<std::uint8_t> scratch_;
    std::vector<std::uintptr_t> frontier_;
    std::vector<std::uintptr_t> next_;
    /// std::find over a vector was the whole cost of discovery: the walk
    /// visits tens of thousands of nodes and each one was compared against
    /// every node already seen.
    std::unordered_set<std::uintptr_t> seenSet_;
    /// ChrIns + characterOffset_ -> container, container + containerOffset_ ->
    /// the module. Measured by Calibrate(), never assumed.
    bool pathKnown_ = false;
    std::size_t characterOffset_ = 0;
    std::size_t containerOffset_ = 0;
};

} // namespace sekiro_haptics::process
