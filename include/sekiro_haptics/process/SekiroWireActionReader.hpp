#pragma once

// Is the player in a WIRE ACTION right now?
//
// WHAT WAS OBSERVED (2026-09-20/21, build sha256 637aca52...b856)
// ----------------------------------------------------------------
//   CSWireActionModule + 0x1C0   u64. Idle it reads 0xFFFFFFFFFFFFFFFF.
//                                During a wire action it holds a handle
//                                (0x20B000xx_xxxxxxxx shaped), returning to
//                                the idle value when the action ends.
//
// Three separate things had to be told apart, and all three were measured:
//
//   the wire BUTTON pressed with no target   -> 0 changes (repeated presses)
//   a usable TARGET present, button untouched -> 0 changes (~30 s aiming at a
//                                                grapple point)
//   an actual wire ACTION                     -> exactly one pulse each, six
//                                                for six in one session and
//                                                one for one in another
//
// Ordinary jumps and falls also produced nothing, while SprjPlayerFallModule
// +0x25 toggled during the falls -- so "in the air" and "on a wire" are
// distinct here rather than the same observation read two ways.
//
// WHAT THIS DOES NOT TELL YOU
// ---------------------------
// Only START and END. The shoot / attach / pull / arrive phases are NOT
// separable from this field, and nothing here invents them from a timer --
// a phase this cannot see must not be emitted as if the game reported it.
// The handle's meaning is also unread: it is carried as an opaque id so two
// consecutive actions on different anchors can be told apart, and nothing
// interprets it as a position, an object or a distance.
//
// A candidate field at +0x1BC (u8 0/1) was REJECTED: it fired on only three
// of the six wire actions in the same capture. It is left documented so it is
// not rediscovered and mistaken for the answer.
//
// OS-independent: IProcessReader / IProcessInspector only.

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

struct WireActionLayout {
    /// WorldChrMan / PlayerIns anchors, reused from the proven guard reader.
    PlayerGuardLayout shared;
    std::uint32_t wireActionVftableRva = 0x2A90870;   // NS_SPRJ::CSWireActionModule
    std::uint32_t wireHitChangeVftableRva = 0x2A90210; // NS_SPRJ::CSWireActionHitChangeModule
    /// The engaged-target handle. Idle value below.
    std::size_t handleOffset = 0x1C0;
    std::uint64_t idleHandle = 0xFFFFFFFFFFFFFFFFull;
    /// u8 in CSWireActionHitChangeModule: a wire target is available right
    /// now -- the state the on-screen grapple indicator shows.
    ///
    /// Measured by looking at a grapple point and away from it six times: the
    /// byte followed the indicator every time (9 on/off spans), went to 0 the
    /// instant a wire action fired, and was 0 the whole time no target was in
    /// view. It is the "can grapple" state, NOT the action.
    std::size_t targetAvailableOffset = 0x690;
    /// The AIRBORNE counterpart of the byte above -- PROBABLY. See the warning.
    ///
    /// Jumping in place while looking at a grapple point made the two toggle
    /// at exactly the same timestamps, always in opposite directions: on the
    /// ground +0x690 is 1 and +0x6B0 is 0, in the air they swap.
    ///
    /// WHAT THAT SCENE DID NOT SHOW
    /// ----------------------------
    /// A grapple point was in view for the WHOLE of it. So it shows this byte
    /// is 1 when airborne WITH a target; it says nothing about airborne
    /// WITHOUT one. If the byte simply means "airborne", the recording looks
    /// exactly the same.
    ///
    /// The player then reported L2 resisting on every ordinary jump, which is
    /// what "airborne" would do and what "airborne AND target" would not. So
    /// this offset is treated as UNCONFIRMED: WirePrepPolicy no longer arms
    /// from it alone. Settling it needs one scene this one lacked -- jump
    /// where nothing is grapplable -- see tools/wire_air_flag_probe.py.
    std::size_t airTargetAvailableOffset = 0x6B0;
    /// u8 in the same module: 1 only between launch and arrival. Recorded
    /// because it was seen, exposed because it is cheap -- nothing currently
    /// depends on it, and it is NOT the phase breakdown (shoot/attach/pull/
    /// arrive), which remains unavailable.
    std::size_t inFlightOffset = 0x6F0;
    std::size_t searchWidthBytes = 0x2000;
    int searchDepth = 2;
};

enum class WireReadStatus {
    Ok,
    UnsupportedBuild,
    RootUnavailable,
    PlayerInsUnavailable,
    ModuleNotFound,
    ReadFailed,
};
const char* ToString(WireReadStatus status);

struct WireActionSample {
    WireReadStatus status = WireReadStatus::RootUnavailable;
    std::uintptr_t moduleAddress = 0;
    /// Bumped when the resolved module instance changes, so values either side
    /// of a swap are never compared.
    std::uint64_t generation = 0;
    /// True when the handle is not the idle value.
    bool inWireAction = false;
    /// A wire target is available -- what the on-screen indicator shows,
    /// on the ground OR in the air. False whenever the read failed, so it is
    /// never stale.
    bool targetAvailable = false;
    /// The two halves, kept separate for logging: they are mutually exclusive
    /// and which one is set says whether the player is grounded.
    bool groundTarget = false;
    bool airTarget = false;
    /// Between launch and arrival. Observed, not depended on.
    bool inFlight = false;
    /// Opaque. Only ever compared for equality.
    std::uint64_t handle = 0;
    bool Ok() const { return status == WireReadStatus::Ok; }
};

struct WireActionReaderStats {
    std::uint64_t polls = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    std::uint64_t moduleSearches = 0;
    std::uint64_t moduleChanges = 0;
};

class SekiroWireActionReader {
public:
    SekiroWireActionReader(IProcessReader& reader, IProcessInspector& inspector,
                           KnownRootSpec worldChrManSpec, ExecutableIdentity expectedIdentity,
                           ExecutableIdentity currentIdentity, std::uintptr_t moduleBaseAddress,
                           WireActionLayout layout = {});

    RootResolveResult Prime();
    WireActionSample Poll();

    const WireActionReaderStats& Stats() const { return stats_; }
    void Invalidate();

private:
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address);
    bool HasVftable(std::uintptr_t object, std::uint32_t rva);
    /// Both wire modules are only accepted from a container that ALSO holds a
    /// SprjPlayerDamageModule -- the player-specific concrete type. Same
    /// ownership proof the guard reader uses; "the first object with the right
    /// vptr" would be somebody else's.
    bool FindPlayerOwnedModules(std::uintptr_t playerIns, std::uintptr_t& wireOut,
                                std::uintptr_t& hitChangeOut);

    IProcessReader& reader_;
    SekiroKnownRootResolver rootResolver_;
    std::uintptr_t moduleBase_;
    WireActionLayout layout_;

    bool primed_ = false;
    std::uintptr_t cachedPlayerIns_ = 0;
    std::uintptr_t cachedModule_ = 0;
    std::uintptr_t cachedHitChange_ = 0;
    std::uint64_t generation_ = 0;
    WireActionReaderStats stats_;

    std::vector<std::uint8_t> scratch_;
    std::vector<std::uintptr_t> frontier_;
    std::vector<std::uintptr_t> next_;
    std::vector<std::uintptr_t> seen_;
};

// ---------------------------------------------------------------------------
// Turning samples into start/end events.
// ---------------------------------------------------------------------------

enum class WireEventKind {
    Started,
    /// The action ended while we were still watching it.
    Ended,
    /// We stopped being able to watch. NOT a completion: a read failure, a
    /// module swap or a capture gap says nothing about whether the wire action
    /// finished, and reporting one would be inventing an outcome.
    EndedUnobserved,
};
const char* ToString(WireEventKind kind);

struct WireEvent {
    WireEventKind kind = WireEventKind::Started;
    std::int64_t timestampUs = 0;
    std::uint64_t handle = 0;
    /// Only meaningful for Ended; 0 otherwise.
    std::int64_t durationUs = 0;
};

struct WireObservation {
    std::int64_t timestampUs = 0;
    bool readOk = false;
    std::uint64_t generation = 0;
    bool inWireAction = false;
    std::uint64_t handle = 0;
    /// Set when the caller knows it did not observe the window since the last
    /// observation (a long poll gap).
    bool continuityBreak = false;
};

/// Start/end detector.
///
/// Rules, each of which exists because the opposite would fabricate something:
///   - the FIRST observation only establishes a baseline; an action already in
///     progress when we attached is not reported as having started
///   - a generation change or a continuity break ends any open action as
///     EndedUnobserved, never as Ended
///   - a handle that changes while still in an action is two actions, not one
///   - a read failure does not end an action silently; it ends it as
///     EndedUnobserved
class WireActionEventDetector {
public:
    std::vector<WireEvent> Update(const WireObservation& observation);
    /// Close out at shutdown. An action still open becomes EndedUnobserved.
    std::vector<WireEvent> Finish(std::int64_t timestampUs);

    bool InAction() const { return inAction_; }

private:
    std::vector<WireEvent> EndOpen(std::int64_t timestampUs, WireEventKind kind);

    bool haveBaseline_ = false;
    bool inAction_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t handle_ = 0;
    std::int64_t startedUs_ = 0;
};

} // namespace sekiro_haptics::process
