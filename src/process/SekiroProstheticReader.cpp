#include "sekiro_haptics/process/SekiroProstheticReader.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace sekiro_haptics::process {

namespace {

bool Plausible(std::uintptr_t p) {
    return p > 0x10000 && p < 0x7FFF'FFFF'FFFFull;
}

} // namespace

const char* ToString(ProstheticReadStatus status) {
    switch (status) {
        case ProstheticReadStatus::Ok: return "Ok";
        case ProstheticReadStatus::UnsupportedBuild: return "UnsupportedBuild";
        case ProstheticReadStatus::RootUnavailable: return "RootUnavailable";
        case ProstheticReadStatus::PlayerInsUnavailable: return "PlayerInsUnavailable";
        case ProstheticReadStatus::PlayerGameDataNotFound: return "PlayerGameDataNotFound";
        case ProstheticReadStatus::EquipGameDataInvalid: return "EquipGameDataInvalid";
        case ProstheticReadStatus::ReadFailed: return "ReadFailed";
    }
    return "Unknown";
}

SekiroProstheticReader::SekiroProstheticReader(IProcessReader& reader, IProcessInspector& inspector,
                                               KnownRootSpec worldChrManSpec,
                                               ExecutableIdentity expectedIdentity,
                                               ExecutableIdentity currentIdentity,
                                               std::uintptr_t moduleBaseAddress,
                                               ProstheticLayout layout)
    : reader_(reader),
      rootResolver_(reader, inspector, std::move(worldChrManSpec), std::move(expectedIdentity),
                    std::move(currentIdentity)),
      moduleBase_(moduleBaseAddress),
      layout_(layout) {
    scratch_.resize(std::max<std::size_t>(layout_.searchWidthBytes,
                                          layout_.slotArrayOffset +
                                              layout_.slotCount * layout_.slotStride));
}

void SekiroProstheticReader::Invalidate() {
    cachedPlayerIns_ = 0;
    cachedGameData_ = 0;
}

RootResolveResult SekiroProstheticReader::Prime() {
    const auto resolved = rootResolver_.Resolve();
    primed_ = resolved.result == RootResolveResult::Resolved;
    return resolved.result;
}

std::optional<std::uintptr_t> SekiroProstheticReader::ReadPointer(std::uintptr_t address) {
    if (!Plausible(address) || address > std::numeric_limits<std::uintptr_t>::max() - 8)
        return std::nullopt;
    std::uint64_t value = 0;
    if (reader_.ReadBytes(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) !=
        ProcessReaderResult::Success) {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(value);
}

bool SekiroProstheticReader::HasVftable(std::uintptr_t object, std::uint32_t rva) {
    const auto vptr = ReadPointer(object);
    return vptr.has_value() && *vptr == moduleBase_ + rva;
}

std::optional<std::uintptr_t> SekiroProstheticReader::FindPlayerGameData(std::uintptr_t playerIns) {
    // Bounded breadth-first walk for an object whose vftable is
    // PlayerGameData's. Exactly one instance was reachable from PlayerIns when
    // this was measured, so unlike EquipMagicData there is nothing to choose
    // between -- but the count is still checked, and more than one makes the
    // answer ambiguous rather than "the first one".
    ++stats_.playerGameDataSearches;
    seen_.clear();
    frontier_.clear();
    frontier_.push_back(playerIns);
    seen_.push_back(playerIns);

    const std::size_t width = layout_.searchWidthBytes;
    std::uintptr_t found = 0;
    int matches = 0;

    for (int depth = 0; depth < layout_.searchDepth; ++depth) {
        next_.clear();
        for (std::uintptr_t node : frontier_) {
            if (node > std::numeric_limits<std::uintptr_t>::max() - width) continue;
            if (reader_.ReadBytes(node, scratch_.data(), width) != ProcessReaderResult::Success)
                continue;
            for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= width;
                 offset += sizeof(std::uint64_t)) {
                std::uint64_t raw = 0;
                std::memcpy(&raw, scratch_.data() + offset, sizeof(raw));
                const auto candidate = static_cast<std::uintptr_t>(raw);
                if (!Plausible(candidate)) continue;
                const auto vptr = ReadPointer(candidate);
                if (!vptr.has_value()) continue;
                if (*vptr == moduleBase_ + layout_.playerGameDataVftableRva) {
                    if (found == 0) found = candidate;
                    if (candidate != found) ++matches;
                }
                if (std::find(seen_.begin(), seen_.end(), candidate) == seen_.end()) {
                    seen_.push_back(candidate);
                    if (depth + 1 < layout_.searchDepth) next_.push_back(candidate);
                }
            }
        }
        // Shallowest first: stop as soon as this depth produced an answer, so a
        // deeper duplicate cannot displace it.
        if (found != 0) break;
        frontier_ = next_;
    }
    (void)matches;
    if (found == 0) return std::nullopt;
    return found;
}

ProstheticSample SekiroProstheticReader::Poll() {
    ++stats_.polls;
    ProstheticSample sample;

    auto fail = [&](ProstheticReadStatus status) {
        ++stats_.failed;
        sample.status = status;
        sample.generation = generation_;
        // A failure never carries the previous selection forward: an unknown
        // state has to look unknown, or a trigger set for the old tool would
        // sit there through a menu, a load or a death.
        sample.selectedEquipId = kEmptyEquipId;
        cachedGameData_ = 0;
        return sample;
    };

    if (!primed_) {
        if (Prime() != RootResolveResult::Resolved) {
            return fail(rootResolver_.Current().result == RootResolveResult::UnsupportedBuild
                            ? ProstheticReadStatus::UnsupportedBuild
                            : ProstheticReadStatus::RootUnavailable);
        }
    }

    // Cheap: re-read the static pointer slot, no AOB rescan.
    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.shared.worldChrManVftableRva))
        return fail(ProstheticReadStatus::RootUnavailable);

    const auto playerIns = ReadPointer(root.objectAddress +
                                       static_cast<std::uintptr_t>(layout_.shared.playerInsOffset));
    if (!playerIns.has_value() || !HasVftable(*playerIns, layout_.shared.playerInsVftableRva))
        return fail(ProstheticReadStatus::PlayerInsUnavailable);

    if (*playerIns != cachedPlayerIns_) {
        // PlayerIns was replaced. Everything below it is suspect even if its
        // vftable still reads correctly (DEFLECT_STATUS.md 7.8 bug 1).
        cachedPlayerIns_ = *playerIns;
        cachedGameData_ = 0;
    }

    if (cachedGameData_ != 0 && !HasVftable(cachedGameData_, layout_.playerGameDataVftableRva))
        cachedGameData_ = 0;

    if (cachedGameData_ == 0) {
        const auto found = FindPlayerGameData(*playerIns);
        if (!found.has_value()) return fail(ProstheticReadStatus::PlayerGameDataNotFound);
        cachedGameData_ = *found;
        ++generation_;
        ++stats_.playerGameDataChanges;
    }

    const auto equip = ReadPointer(cachedGameData_ + layout_.equipGameDataOffset);
    if (!equip.has_value() || !HasVftable(*equip, layout_.equipGameDataVftableRva))
        return fail(ProstheticReadStatus::EquipGameDataInvalid);

    std::uint8_t selected = 0;
    if (reader_.ReadBytes(*equip + layout_.selectedSlotOffset, &selected, 1) !=
        ProcessReaderResult::Success) {
        return fail(ProstheticReadStatus::ReadFailed);
    }

    const std::size_t bytes = layout_.slotCount * layout_.slotStride;
    if (reader_.ReadBytes(cachedGameData_ + layout_.slotArrayOffset, scratch_.data(), bytes) !=
        ProcessReaderResult::Success) {
        return fail(ProstheticReadStatus::ReadFailed);
    }

    sample.slots.reserve(layout_.slotCount);
    for (std::size_t index = 0; index < layout_.slotCount; ++index) {
        ProstheticSlot slot;
        std::memcpy(&slot.raw0, scratch_.data() + index * layout_.slotStride, sizeof(slot.raw0));
        std::memcpy(&slot.equipParamWeaponId,
                    scratch_.data() + index * layout_.slotStride + layout_.slotIdOffset,
                    sizeof(slot.equipParamWeaponId));
        sample.slots.push_back(slot);
    }

    sample.status = ProstheticReadStatus::Ok;
    sample.generation = generation_;
    sample.selectedSlot = selected;
    sample.selectedEquipId = selected < sample.slots.size()
                                 ? sample.slots[selected].equipParamWeaponId
                                 : kEmptyEquipId;
    ++stats_.ok;
    return sample;
}

} // namespace sekiro_haptics::process
