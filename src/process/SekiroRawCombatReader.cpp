#include "sekiro_haptics/process/SekiroRawCombatReader.hpp"

#include <chrono>
#include <cstring>
#include <utility>

namespace sekiro_haptics::process {

namespace {

// Field-layout hypothesis (see the header's doc comment): HP/MaxHP/Posture/
// MaxPosture as consecutive i32 fields on PlayerGameData. kCombatFieldBlockSizeBytes
// (0x24) covers the whole span in one contiguous read.
constexpr std::size_t kHpOffset = 0x18;
constexpr std::size_t kMaxHpOffset = 0x1C;
constexpr std::size_t kPostureOffset = 0x34;
constexpr std::size_t kMaxPostureOffset = 0x38;
constexpr std::size_t kFieldBlockBase = kHpOffset;
static_assert((kMaxPostureOffset + sizeof(std::int32_t)) - kFieldBlockBase == kCombatFieldBlockSizeBytes,
              "kCombatFieldBlockSizeBytes must match the actual HP..MaxPosture span");

// Generous sanity bound -- real HP/Posture values are at most a few
// thousand; anything above this is treated as garbage/misaligned data
// rather than accepted as a real game value.
constexpr std::int32_t kUnrealisticUpperBound = 1'000'000;

std::int64_t NowMonotonicUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

CombatSnapshotStatus MapRootResolveFailure(RootResolveResult result) {
    switch (result) {
        case RootResolveResult::UnsupportedBuild:
            return CombatSnapshotStatus::UnsupportedBuild;
        case RootResolveResult::SignatureNotFound:
            return CombatSnapshotStatus::SignatureNotFound;
        case RootResolveResult::AmbiguousSignature:
            return CombatSnapshotStatus::AmbiguousSignature;
        default:
            // NotAttached / ProcessExited / ModuleNotFound /
            // AddressCalculationFailed / ReadFailed / NullPointer -- all
            // collapse to ReadFailed at this layer. The precise underlying
            // reason is still available to a caller willing to drive the
            // lower-level resolvers directly; this reader only promises the
            // 8 states documented on CombatSnapshotStatus.
            return CombatSnapshotStatus::ReadFailed;
    }
}

} // namespace

const char* ToString(CombatSnapshotStatus status) {
    switch (status) {
        case CombatSnapshotStatus::UnsupportedBuild:
            return "UnsupportedBuild";
        case CombatSnapshotStatus::SignatureNotFound:
            return "SignatureNotFound";
        case CombatSnapshotStatus::AmbiguousSignature:
            return "AmbiguousSignature";
        case CombatSnapshotStatus::ResolvedUnvalidated:
            return "ResolvedUnvalidated";
        case CombatSnapshotStatus::Valid:
            return "Valid";
        case CombatSnapshotStatus::TemporarilyUnavailable:
            return "TemporarilyUnavailable";
        case CombatSnapshotStatus::ReadFailed:
            return "ReadFailed";
        case CombatSnapshotStatus::InvariantViolation:
            return "InvariantViolation";
    }
    return "Unknown";
}

SekiroRawCombatReader::SekiroRawCombatReader(IProcessReader& reader, IProcessInspector& inspector,
                                               KnownRootSpec gameDataManSpec,
                                               std::int64_t playerGameDataOffsetFromGameDataMan,
                                               ExecutableIdentity expectedIdentity, ExecutableIdentity currentIdentity)
    : reader_(reader),
      gameDataManResolver_(reader, inspector, std::move(gameDataManSpec), std::move(expectedIdentity),
                            std::move(currentIdentity)),
      playerGameDataResolver_(reader, playerGameDataOffsetFromGameDataMan) {}

CombatResolveResult SekiroRawCombatReader::Resolve() {
    resolved_ = false;
    playerGameDataAddress_ = 0;

    CombatResolveResult result;

    ResolvedRoot gameDataMan = gameDataManResolver_.Resolve();
    if (gameDataMan.result != RootResolveResult::Resolved) {
        result.status = MapRootResolveFailure(gameDataMan.result);
        return result;
    }
    result.gameDataManAddress = gameDataMan.objectAddress;

    ResolvedRoot playerGameData = playerGameDataResolver_.Resolve(gameDataMan.objectAddress);
    if (playerGameData.result != RootResolveResult::Resolved) {
        result.status = MapRootResolveFailure(playerGameData.result);
        return result;
    }
    result.playerGameDataAddress = playerGameData.objectAddress;
    result.generation = playerGameData.generation;
    result.status = CombatSnapshotStatus::ResolvedUnvalidated;

    resolved_ = true;
    playerGameDataAddress_ = playerGameData.objectAddress;
    generation_ = playerGameData.generation;
    return result;
}

CombatSnapshot SekiroRawCombatReader::ReadSnapshot() {
    CombatSnapshot snapshot;
    snapshot.monotonicTimestampUs = NowMonotonicUs();

    if (!resolved_) {
        snapshot.status = CombatSnapshotStatus::TemporarilyUnavailable;
        return snapshot;
    }
    snapshot.generation = generation_;

    std::uint8_t buffer[kCombatFieldBlockSizeBytes];
    ProcessReaderResult readResult =
        reader_.ReadBytes(playerGameDataAddress_ + kFieldBlockBase, buffer, sizeof(buffer));
    if (readResult != ProcessReaderResult::Success) {
        snapshot.status = CombatSnapshotStatus::ReadFailed;
        return snapshot;
    }

    std::int32_t hp = 0, maxHp = 0, posture = 0, maxPosture = 0;
    std::memcpy(&hp, buffer + (kHpOffset - kFieldBlockBase), sizeof(hp));
    std::memcpy(&maxHp, buffer + (kMaxHpOffset - kFieldBlockBase), sizeof(maxHp));
    std::memcpy(&posture, buffer + (kPostureOffset - kFieldBlockBase), sizeof(posture));
    std::memcpy(&maxPosture, buffer + (kMaxPostureOffset - kFieldBlockBase), sizeof(maxPosture));

    bool valid = maxHp > 0 && maxHp <= kUnrealisticUpperBound && hp >= 0 && hp <= maxHp && maxPosture > 0 &&
                 maxPosture <= kUnrealisticUpperBound && posture >= 0 && posture <= maxPosture;

    snapshot.hp = hp;
    snapshot.maxHp = maxHp;
    snapshot.posture = posture;
    snapshot.maxPosture = maxPosture;
    snapshot.status = valid ? CombatSnapshotStatus::Valid : CombatSnapshotStatus::InvariantViolation;
    return snapshot;
}

} // namespace sekiro_haptics::process
