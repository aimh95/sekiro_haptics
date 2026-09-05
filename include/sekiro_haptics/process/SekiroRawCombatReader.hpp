#pragma once

// SEK-PROBE-001D Stage B: reads player HP/MaxHP/Posture/MaxPosture from an
// already-resolved PlayerGameData object, built entirely on top of
// SekiroKnownRootResolver.hpp (Stage A, unchanged) -- this module adds only
// the parts Stage A doesn't cover: the GameDataMan -> PlayerGameData
// pointer-chain composition, one contiguous field read, and the invariant
// validation that turns raw bytes into a trustworthy CombatSnapshot. See
// docs/07-combat-signal-reader.md.
//
// The field offsets (+0x18/+0x1C/+0x34/+0x38) are a *hypothesis* until
// cross-checked against a real attached process -- this module never
// invents a value it can't validate; see CombatSnapshotStatus::
// InvariantViolation and ToString(CombatSnapshotStatus).

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"

#include <cstdint>

namespace sekiro_haptics::process {

/// Byte span of the one contiguous read ReadSnapshot() performs -- HP
/// (+0x18) through MaxPosture (+0x38, 4 bytes) inclusive, in a single
/// ReadBytes() call rather than four separate ones. Exposed so callers
/// (e.g. a `combat-plan` report) can state expected per-sample bytes
/// without duplicating the field-layout hypothesis here.
inline constexpr std::size_t kCombatFieldBlockSizeBytes = 0x24;

/// Coarse status of one resolve attempt or one snapshot read -- the
/// "minimum states" this module reports, per its own contract:
/// - UnsupportedBuild/SignatureNotFound/AmbiguousSignature: propagated
///   unchanged from the underlying root/child resolvers during Resolve().
/// - ResolvedUnvalidated: Resolve() located a non-null, live
///   GameDataMan and PlayerGameData object -- the pointer chain itself is
///   trusted, but no field values have been read or validated yet. This is
///   Resolve()'s own success terminal state.
/// - TemporarilyUnavailable: ReadSnapshot() was called before a successful
///   Resolve() (or after one that has since failed) -- the caller should
///   call Resolve() (again), not treat this as a hard error.
/// - ReadFailed: either the pointer chain itself failed to resolve for a
///   reason other than the three explicit cases above (module gone,
///   address-calculation failure, a hard process-read failure, or a null
///   intermediate pointer), or ReadSnapshot()'s own contiguous field read
///   failed/returned a partial read.
/// - InvariantViolation: the field read succeeded, but the decoded values
///   fail basic sanity checks (MaxHP/MaxPosture <= 0, HP/Posture out of
///   [0, Max], or either exceeding a generous unrealistic upper bound) --
///   never silently accepted as real game data.
/// - Valid: every invariant holds.
enum class CombatSnapshotStatus {
    UnsupportedBuild,
    SignatureNotFound,
    AmbiguousSignature,
    ResolvedUnvalidated,
    Valid,
    TemporarilyUnavailable,
    ReadFailed,
    InvariantViolation,
};
const char* ToString(CombatSnapshotStatus status);

/// Result of one Resolve() call. `gameDataManAddress`/`playerGameDataAddress`
/// are only meaningful when `status == ResolvedUnvalidated`.
struct CombatResolveResult {
    CombatSnapshotStatus status = CombatSnapshotStatus::SignatureNotFound;
    std::uintptr_t gameDataManAddress = 0;
    std::uintptr_t playerGameDataAddress = 0;
    /// PlayerGameData's own resolve generation -- see CombatSnapshot::generation.
    std::uint64_t generation = 0;
};

/// One HP/Posture reading. Fields other than `status`/`generation`/
/// `monotonicTimestampUs` are only meaningful when `status == Valid` -- an
/// invalid sample is never silently replaced by the last known-good values,
/// and is reported as its own distinct sample rather than skipped.
///
/// `generation` identifies which PlayerGameData *instance* these fields
/// came from (see SekiroKnownRootResolver.hpp's generation contract): two
/// snapshots with different `generation` values must never have their
/// fields diffed against each other -- they may not even be the same
/// in-game character.
struct CombatSnapshot {
    CombatSnapshotStatus status = CombatSnapshotStatus::TemporarilyUnavailable;
    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    std::int32_t posture = 0;
    std::int32_t maxPosture = 0;
    std::uint64_t generation = 0;
    std::int64_t monotonicTimestampUs = 0;
};

/// Composes a GameDataMan root (Stage A's SekiroKnownRootResolver) with one
/// further pointer dereference to PlayerGameData (Stage A's
/// SekiroChildPointerResolver), then reads/validates the four combat
/// fields. Resolve() is never called automatically by ReadSnapshot() --
/// a caller driving a live sampling loop is expected to call Resolve() only
/// at session start and after a detected failure, then call ReadSnapshot()
/// many times against the same resolved address (no per-sample AOB scan).
class SekiroRawCombatReader {
public:
    /// `playerGameDataOffsetFromGameDataMan` is the signed byte offset
    /// applied to GameDataMan's resolved address before one more pointer
    /// dereference (the ticket's "GameDataMan + 0x8 -> PlayerGameData*"
    /// hypothesis -- pass 0x8 for that). `expectedIdentity`/`currentIdentity`
    /// are forwarded to the underlying SekiroKnownRootResolver unchanged.
    SekiroRawCombatReader(IProcessReader& reader, IProcessInspector& inspector, KnownRootSpec gameDataManSpec,
                           std::int64_t playerGameDataOffsetFromGameDataMan, ExecutableIdentity expectedIdentity,
                           ExecutableIdentity currentIdentity);

    CombatResolveResult Resolve();

    /// Takes one snapshot using the currently resolved PlayerGameData
    /// address, without re-resolving anything or re-scanning for the AOB.
    CombatSnapshot ReadSnapshot();

private:
    IProcessReader& reader_;
    SekiroKnownRootResolver gameDataManResolver_;
    SekiroChildPointerResolver playerGameDataResolver_;

    bool resolved_ = false;
    std::uintptr_t playerGameDataAddress_ = 0;
    std::uint64_t generation_ = 0;
};

} // namespace sekiro_haptics::process
