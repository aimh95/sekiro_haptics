#include "sekiro_haptics/process/SekiroPlayerGuardReader.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
#include <utility>

namespace sekiro_haptics::process {

namespace {

/// A user-space pointer we are willing to dereference. Deliberately loose --
/// the real filter is the vftable check, not this.
bool Plausible(std::uintptr_t p) {
    return p > 0x10000 && p < 0x7FFF'FFFF'FFFFull;
}

} // namespace

const char* ToString(GuardReadStatus status) {
    switch (status) {
        case GuardReadStatus::Ok: return "Ok";
        case GuardReadStatus::UnsupportedBuild: return "UnsupportedBuild";
        case GuardReadStatus::RootUnavailable: return "RootUnavailable";
        case GuardReadStatus::PlayerInsUnavailable: return "PlayerInsUnavailable";
        case GuardReadStatus::ModuleNotFound: return "ModuleNotFound";
        case GuardReadStatus::ReadFailed: return "ReadFailed";
    }
    return "Unknown";
}

SekiroPlayerGuardReader::SekiroPlayerGuardReader(IProcessReader& reader, IProcessInspector& inspector,
                                                 KnownRootSpec worldChrManSpec,
                                                 ExecutableIdentity expectedIdentity,
                                                 ExecutableIdentity currentIdentity,
                                                 std::uintptr_t moduleBaseAddress,
                                                 PlayerGuardLayout layout)
    : reader_(reader),
      rootResolver_(reader, inspector, std::move(worldChrManSpec), std::move(expectedIdentity),
                    std::move(currentIdentity)),
      moduleBase_(moduleBaseAddress),
      layout_(layout) {
    scratch_.resize(std::max<std::size_t>(layout_.searchWidthBytes,
                                          layout_.outcomeOffset + 1));
}

void SekiroPlayerGuardReader::Invalidate() {
    cachedPlayerIns_ = 0;
    cachedModule_ = 0;
}

RootResolveResult SekiroPlayerGuardReader::Prime() {
    const auto resolved = rootResolver_.Resolve();
    primed_ = resolved.result == RootResolveResult::Resolved;
    return resolved.result;
}

std::optional<std::uintptr_t> SekiroPlayerGuardReader::ReadPointer(std::uintptr_t address) {
    if (!Plausible(address) || address > std::numeric_limits<std::uintptr_t>::max() - 8) return std::nullopt;
    std::uint64_t value = 0;
    if (reader_.ReadBytes(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) !=
        ProcessReaderResult::Success) {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(value);
}

bool SekiroPlayerGuardReader::HasVftable(std::uintptr_t object, std::uint32_t rva) {
    const auto vptr = ReadPointer(object);
    return vptr.has_value() && *vptr == moduleBase_ + rva;
}

std::optional<std::uintptr_t> SekiroPlayerGuardReader::FindPlayerOwnedModule(std::uintptr_t playerIns) {
    // "A container that also holds a SprjPlayerDamageModule" is necessary but
    // NOT sufficient: several such containers are reachable and at least one of
    // them points at a stale ActionFlagModule left behind by an earlier player
    // object. Taking the first one found picked exactly that dead object -- its
    // outcome byte sat at 0 while the live module read 1, so no guard was ever
    // detected.
    //
    // So every player-owned container is collected and the module the MOST of
    // them agree on wins. On the observed layout that is 8 votes to 1.
    ++stats_.moduleSearches;
    seen_.clear();
    frontier_.clear();
    frontier_.push_back(playerIns);
    seen_.push_back(playerIns);

    std::vector<std::pair<std::uintptr_t, int>> votes;
    const std::size_t width = layout_.searchWidthBytes;

    for (int depth = 0; depth < layout_.searchDepth; ++depth) {
        next_.clear();
        for (std::uintptr_t node : frontier_) {
            if (node > std::numeric_limits<std::uintptr_t>::max() - width) continue;
            if (reader_.ReadBytes(node, scratch_.data(), width) != ProcessReaderResult::Success) continue;

            std::uintptr_t actionFlag = 0;
            bool hasPlayerDamage = false;
            for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= width; offset += sizeof(std::uint64_t)) {
                std::uint64_t raw = 0;
                std::memcpy(&raw, scratch_.data() + offset, sizeof(raw));
                const auto candidate = static_cast<std::uintptr_t>(raw);
                if (!Plausible(candidate)) continue;
                const auto vptr = ReadPointer(candidate);
                if (!vptr.has_value()) continue;
                if (*vptr == moduleBase_ + layout_.actionFlagVftableRva && actionFlag == 0) {
                    actionFlag = candidate;
                } else if (*vptr == moduleBase_ + layout_.playerDamageVftableRva) {
                    hasPlayerDamage = true;
                }
                if (std::find(seen_.begin(), seen_.end(), candidate) == seen_.end()) {
                    seen_.push_back(candidate);
                    if (depth + 1 < layout_.searchDepth) next_.push_back(candidate);
                }
            }
            if (actionFlag != 0 && hasPlayerDamage) {
                auto it = std::find_if(votes.begin(), votes.end(),
                                        [&](const auto& v) { return v.first == actionFlag; });
                if (it == votes.end()) votes.emplace_back(actionFlag, 1);
                else ++it->second;
            }
            if (seen_.size() > 20'000) break;   // never let a graph eat the poll budget
        }
        frontier_.swap(next_);
    }

    if (votes.empty()) return std::nullopt;
    const auto best = std::max_element(votes.begin(), votes.end(),
                                        [](const auto& a, const auto& b) { return a.second < b.second; });
    lastVoteCount_ = best->second;
    lastVoteCandidates_ = votes.size();
    return best->first;
}

PlayerGuardSample SekiroPlayerGuardReader::Poll() {
    ++stats_.polls;
    PlayerGuardSample sample;

    if (!primed_) {
        if (Prime() != RootResolveResult::Resolved) {
            sample.status = rootResolver_.Current().result == RootResolveResult::UnsupportedBuild
                                ? GuardReadStatus::UnsupportedBuild
                                : GuardReadStatus::RootUnavailable;
            ++stats_.failed;
            Invalidate();
            return sample;
        }
    }

    // Cheap: re-read the static pointer slot, no AOB rescan.
    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.worldChrManVftableRva)) {
        sample.status = GuardReadStatus::RootUnavailable;
        ++stats_.failed;
        Invalidate();
        return sample;
    }

    const auto playerIns = ReadPointer(root.objectAddress + static_cast<std::uintptr_t>(layout_.playerInsOffset));
    if (!playerIns.has_value() || !Plausible(*playerIns) ||
        !HasVftable(*playerIns, layout_.playerInsVftableRva)) {
        sample.status = GuardReadStatus::PlayerInsUnavailable;
        ++stats_.failed;
        Invalidate();
        return sample;
    }

    // BUG 1 fix: a changed PlayerIns invalidates every cached module, even one
    // whose vftable pointer still reads correctly out of freed memory.
    if (*playerIns != cachedPlayerIns_) {
        if (cachedPlayerIns_ != 0) ++stats_.playerInsChanges;
        cachedPlayerIns_ = *playerIns;
        cachedModule_ = 0;
    }

    if (cachedModule_ == 0 || !HasVftable(cachedModule_, layout_.actionFlagVftableRva)) {
        const auto found = FindPlayerOwnedModule(*playerIns);
        if (!found.has_value()) {
            cachedModule_ = 0;
            sample.status = GuardReadStatus::ModuleNotFound;
            ++stats_.failed;
            return sample;
        }
        if (*found != cachedModule_) {
            ++generation_;
            ++stats_.moduleChanges;
        }
        cachedModule_ = *found;
        stats_.lastVoteCount = lastVoteCount_;
        stats_.lastVoteCandidates = lastVoteCandidates_;
    }

    const std::size_t span = layout_.outcomeOffset + 1;
    if (span > scratch_.size()) scratch_.resize(span);
    if (cachedModule_ > std::numeric_limits<std::uintptr_t>::max() - span ||
        reader_.ReadBytes(cachedModule_, scratch_.data(), span) != ProcessReaderResult::Success) {
        sample.status = GuardReadStatus::ReadFailed;
        ++stats_.failed;
        // Do not keep a module we could not read -- force a re-search.
        cachedModule_ = 0;
        return sample;
    }

    sample.status = GuardReadStatus::Ok;
    sample.moduleAddress = cachedModule_;
    sample.generation = generation_;
    sample.pulse = scratch_[layout_.pulseOffset];
    sample.outcome = scratch_[layout_.outcomeOffset];
    ++stats_.ok;
    return sample;
}

} // namespace sekiro_haptics::process
