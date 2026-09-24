#include "sekiro_haptics/process/SekiroProstheticActionReader.hpp"

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

const char* ToString(ProstheticActionReadStatus status) {
    switch (status) {
        case ProstheticActionReadStatus::Ok: return "Ok";
        case ProstheticActionReadStatus::UnsupportedBuild: return "UnsupportedBuild";
        case ProstheticActionReadStatus::RootUnavailable: return "RootUnavailable";
        case ProstheticActionReadStatus::PlayerInsUnavailable: return "PlayerInsUnavailable";
        case ProstheticActionReadStatus::ModuleNotFound: return "ModuleNotFound";
        case ProstheticActionReadStatus::ReadFailed: return "ReadFailed";
    }
    return "Unknown";
}

const char* ToString(ProstheticTool tool) {
    switch (tool) {
        case ProstheticTool::Shuriken: return "shuriken";
        case ProstheticTool::Axe: return "axe";
        case ProstheticTool::Spear: return "spear";
    }
    return "unknown";
}

std::optional<ProstheticTool> ToolForActionCode(std::uint32_t code) {
    // Only families that were actually seen while using that tool.
    switch (code / 1'000'000u) {
        case 70: return ProstheticTool::Shuriken;
        case 73: return ProstheticTool::Axe;
        case 78: return ProstheticTool::Spear;
        default: return std::nullopt;
    }
}

ProstheticPhase PhaseForActionCode(std::uint32_t code) {
    const std::uint32_t tail = code % 100'000u;
    if (tail < 100u) return ProstheticPhase::Ready;
    if (tail < 200u) return ProstheticPhase::Attack;
    return ProstheticPhase::None;
}

SekiroProstheticActionReader::SekiroProstheticActionReader(
    IProcessReader& reader, IProcessInspector& inspector, KnownRootSpec worldChrManSpec,
    ExecutableIdentity expectedIdentity, ExecutableIdentity currentIdentity,
    std::uintptr_t moduleBaseAddress, ProstheticActionLayout layout)
    : reader_(reader),
      rootResolver_(reader, inspector, std::move(worldChrManSpec), std::move(expectedIdentity),
                    std::move(currentIdentity)),
      moduleBase_(moduleBaseAddress),
      layout_(layout) {
    scratch_.resize(layout_.searchWidthBytes);
}

RootResolveResult SekiroProstheticActionReader::Prime() {
    const auto resolved = rootResolver_.Resolve();
    primed_ = resolved.result == RootResolveResult::Resolved;
    return resolved.result;
}

std::optional<std::uintptr_t> SekiroProstheticActionReader::ReadPointer(std::uintptr_t address) {
    if (!Plausible(address) || address > std::numeric_limits<std::uintptr_t>::max() - 8)
        return std::nullopt;
    std::uint64_t value = 0;
    if (reader_.ReadBytes(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) !=
        ProcessReaderResult::Success)
        return std::nullopt;
    return static_cast<std::uintptr_t>(value);
}

bool SekiroProstheticActionReader::HasVftable(std::uintptr_t object, std::uint32_t rva) {
    const auto vptr = ReadPointer(object);
    return vptr.has_value() && *vptr == moduleBase_ + rva;
}

std::uintptr_t SekiroProstheticActionReader::FindPlayerOwnedModule(std::uintptr_t playerIns) {
    ++stats_.moduleSearches;
    seen_.clear();
    frontier_.clear();
    frontier_.push_back(playerIns);
    seen_.push_back(playerIns);
    const std::size_t width = layout_.searchWidthBytes;

    for (int depth = 0; depth < layout_.searchDepth; ++depth) {
        next_.clear();
        for (std::uintptr_t node : frontier_) {
            if (node > std::numeric_limits<std::uintptr_t>::max() - width) continue;
            if (reader_.ReadBytes(node, scratch_.data(), width) != ProcessReaderResult::Success)
                continue;
            std::uintptr_t found = 0;
            bool hasPlayerDamage = false;
            for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= width;
                 offset += sizeof(std::uint64_t)) {
                std::uint64_t raw = 0;
                std::memcpy(&raw, scratch_.data() + offset, sizeof(raw));
                const auto candidate = static_cast<std::uintptr_t>(raw);
                if (!Plausible(candidate)) continue;
                const auto vptr = ReadPointer(candidate);
                if (!vptr.has_value()) continue;
                if (*vptr == moduleBase_ + layout_.actionRequestVftableRva && found == 0)
                    found = candidate;
                else if (*vptr == moduleBase_ + layout_.shared.playerDamageVftableRva)
                    hasPlayerDamage = true;
                if (std::find(seen_.begin(), seen_.end(), candidate) == seen_.end()) {
                    seen_.push_back(candidate);
                    if (depth + 1 < layout_.searchDepth) next_.push_back(candidate);
                }
            }
            if (found != 0 && hasPlayerDamage) return found;
        }
        frontier_ = next_;
    }
    return 0;
}

ProstheticActionSample SekiroProstheticActionReader::Poll() {
    ++stats_.polls;
    ProstheticActionSample sample;
    auto fail = [&](ProstheticActionReadStatus status) {
        ++stats_.failed;
        sample.status = status;
        sample.generation = generation_;
        sample.actionCode = 0;
        cachedModule_ = 0;
        return sample;
    };

    if (!primed_) {
        if (Prime() != RootResolveResult::Resolved)
            return fail(rootResolver_.Current().result == RootResolveResult::UnsupportedBuild
                            ? ProstheticActionReadStatus::UnsupportedBuild
                            : ProstheticActionReadStatus::RootUnavailable);
    }

    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.shared.worldChrManVftableRva))
        return fail(ProstheticActionReadStatus::RootUnavailable);

    const auto playerIns = ReadPointer(root.objectAddress +
                                       static_cast<std::uintptr_t>(layout_.shared.playerInsOffset));
    if (!playerIns.has_value() || !HasVftable(*playerIns, layout_.shared.playerInsVftableRva))
        return fail(ProstheticActionReadStatus::PlayerInsUnavailable);

    // PlayerIns is re-read every poll and the module cache dropped when it
    // changes. A vptr check alone does not prove an object is live: a freed
    // allocation can keep the old vftable pointer for a long time (the
    // occ-03 capture read such a dead copy for 180 s while looking healthy).
    if (*playerIns != cachedPlayerIns_) {
        cachedPlayerIns_ = *playerIns;
        cachedModule_ = 0;
    }
    if (cachedModule_ != 0 && !HasVftable(cachedModule_, layout_.actionRequestVftableRva))
        cachedModule_ = 0;

    if (cachedModule_ == 0) {
        cachedModule_ = FindPlayerOwnedModule(*playerIns);
        if (cachedModule_ == 0) return fail(ProstheticActionReadStatus::ModuleNotFound);
        ++generation_;
        ++stats_.moduleChanges;
    }

    std::uint32_t code = 0;
    if (reader_.ReadBytes(cachedModule_ + layout_.actionCodeOffset,
                          reinterpret_cast<std::uint8_t*>(&code), sizeof(code)) !=
        ProcessReaderResult::Success)
        return fail(ProstheticActionReadStatus::ReadFailed);

    sample.status = ProstheticActionReadStatus::Ok;
    sample.generation = generation_;
    sample.actionCode = code;
    ++stats_.ok;
    return sample;
}

// ---------------------------------------------------------------------------

void ProstheticActionDetector::Reset() {
    baselined_ = false;
    generation_ = 0;
    lastCode_ = 0;
    armed_ = false;
    lastAttackCode_ = 0;
}

std::vector<ProstheticActionEvent> ProstheticActionDetector::Update(
    const ProstheticActionObservation& o) {
    std::vector<ProstheticActionEvent> events;

    // Read failure: forget everything. What comes back is a new baseline, so
    // a code that was showing when the read returned is not reported as new.
    if (!o.readOk) {
        Reset();
        return events;
    }
    // First sample, or a different object after a load / death: baseline only.
    if (!baselined_ || o.generation != generation_) {
        Reset();
        baselined_ = true;
        generation_ = o.generation;
        lastCode_ = o.actionCode;
        return events;
    }

    const std::uint32_t code = o.actionCode;
    if (code == lastCode_) return events;       // nothing entered
    lastCode_ = code;

    const auto tool = ToolForActionCode(code);
    if (!tool) return events;                   // idle values, the sword, unknown families
    const auto phase = PhaseForActionCode(code);

    if (phase == ProstheticPhase::Ready) {
        armed_ = true;
        events.push_back({*tool, ProstheticPhase::Ready, code, o.timestampUs});
    } else if (phase == ProstheticPhase::Attack) {
        if (armed_ || code != lastAttackCode_) {
            events.push_back({*tool, ProstheticPhase::Attack, code, o.timestampUs});
            armed_ = false;
            lastAttackCode_ = code;
        }
    }
    return events;
}

} // namespace sekiro_haptics::process
