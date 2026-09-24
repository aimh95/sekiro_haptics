#include "sekiro_haptics/process/SekiroWireActionReader.hpp"

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

const char* ToString(WireReadStatus status) {
    switch (status) {
        case WireReadStatus::Ok: return "Ok";
        case WireReadStatus::UnsupportedBuild: return "UnsupportedBuild";
        case WireReadStatus::RootUnavailable: return "RootUnavailable";
        case WireReadStatus::PlayerInsUnavailable: return "PlayerInsUnavailable";
        case WireReadStatus::ModuleNotFound: return "ModuleNotFound";
        case WireReadStatus::ReadFailed: return "ReadFailed";
    }
    return "Unknown";
}

const char* ToString(WireEventKind kind) {
    switch (kind) {
        case WireEventKind::Started: return "started";
        case WireEventKind::Ended: return "ended";
        case WireEventKind::EndedUnobserved: return "ended-unobserved";
    }
    return "unknown";
}

SekiroWireActionReader::SekiroWireActionReader(IProcessReader& reader, IProcessInspector& inspector,
                                               KnownRootSpec worldChrManSpec,
                                               ExecutableIdentity expectedIdentity,
                                               ExecutableIdentity currentIdentity,
                                               std::uintptr_t moduleBaseAddress,
                                               WireActionLayout layout)
    : reader_(reader),
      rootResolver_(reader, inspector, std::move(worldChrManSpec), std::move(expectedIdentity),
                    std::move(currentIdentity)),
      moduleBase_(moduleBaseAddress),
      layout_(layout) {
    scratch_.resize(std::max<std::size_t>(layout_.searchWidthBytes, layout_.handleOffset + 8));
}

void SekiroWireActionReader::Invalidate() {
    cachedPlayerIns_ = 0;
    cachedModule_ = 0;
}

RootResolveResult SekiroWireActionReader::Prime() {
    const auto resolved = rootResolver_.Resolve();
    primed_ = resolved.result == RootResolveResult::Resolved;
    return resolved.result;
}

std::optional<std::uintptr_t> SekiroWireActionReader::ReadPointer(std::uintptr_t address) {
    if (!Plausible(address) || address > std::numeric_limits<std::uintptr_t>::max() - 8)
        return std::nullopt;
    std::uint64_t value = 0;
    if (reader_.ReadBytes(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) !=
        ProcessReaderResult::Success) {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(value);
}

bool SekiroWireActionReader::HasVftable(std::uintptr_t object, std::uint32_t rva) {
    const auto vptr = ReadPointer(object);
    return vptr.has_value() && *vptr == moduleBase_ + rva;
}

bool SekiroWireActionReader::FindPlayerOwnedModules(std::uintptr_t playerIns,
                                                    std::uintptr_t& wireOut,
                                                    std::uintptr_t& hitChangeOut) {
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

            std::uintptr_t wireModule = 0;
            std::uintptr_t hitChange = 0;
            bool hasPlayerDamage = false;
            for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= width;
                 offset += sizeof(std::uint64_t)) {
                std::uint64_t raw = 0;
                std::memcpy(&raw, scratch_.data() + offset, sizeof(raw));
                const auto candidate = static_cast<std::uintptr_t>(raw);
                if (!Plausible(candidate)) continue;
                const auto vptr = ReadPointer(candidate);
                if (!vptr.has_value()) continue;
                if (*vptr == moduleBase_ + layout_.wireActionVftableRva && wireModule == 0) {
                    wireModule = candidate;
                } else if (*vptr == moduleBase_ + layout_.wireHitChangeVftableRva &&
                           hitChange == 0) {
                    hitChange = candidate;
                } else if (*vptr == moduleBase_ + layout_.shared.playerDamageVftableRva) {
                    hasPlayerDamage = true;
                }
                if (std::find(seen_.begin(), seen_.end(), candidate) == seen_.end()) {
                    seen_.push_back(candidate);
                    if (depth + 1 < layout_.searchDepth) next_.push_back(candidate);
                }
            }
            // All in the SAME container: the wire modules are only the
            // player's if the container also holds the player-specific damage
            // module.
            if (wireModule != 0 && hitChange != 0 && hasPlayerDamage) {
                wireOut = wireModule;
                hitChangeOut = hitChange;
                return true;
            }
        }
        frontier_ = next_;
    }
    return false;
}

WireActionSample SekiroWireActionReader::Poll() {
    ++stats_.polls;
    WireActionSample sample;

    auto fail = [&](WireReadStatus status) {
        ++stats_.failed;
        sample.status = status;
        sample.generation = generation_;
        // Never carry the previous in-action flag forward: an unreadable state
        // is unknown, and the detector turns that into EndedUnobserved rather
        // than into a completion.
        sample.inWireAction = false;
        sample.targetAvailable = false;
        sample.groundTarget = false;
        sample.airTarget = false;
        sample.inFlight = false;
        cachedModule_ = 0;
        cachedHitChange_ = 0;
        return sample;
    };

    if (!primed_) {
        if (Prime() != RootResolveResult::Resolved) {
            return fail(rootResolver_.Current().result == RootResolveResult::UnsupportedBuild
                            ? WireReadStatus::UnsupportedBuild
                            : WireReadStatus::RootUnavailable);
        }
    }

    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.shared.worldChrManVftableRva))
        return fail(WireReadStatus::RootUnavailable);

    const auto playerIns = ReadPointer(root.objectAddress +
                                       static_cast<std::uintptr_t>(layout_.shared.playerInsOffset));
    if (!playerIns.has_value() || !HasVftable(*playerIns, layout_.shared.playerInsVftableRva))
        return fail(WireReadStatus::PlayerInsUnavailable);

    if (*playerIns != cachedPlayerIns_) {
        cachedPlayerIns_ = *playerIns;
        cachedModule_ = 0;
        cachedHitChange_ = 0;
    }
    if (cachedModule_ != 0 && (!HasVftable(cachedModule_, layout_.wireActionVftableRva) ||
                               !HasVftable(cachedHitChange_, layout_.wireHitChangeVftableRva))) {
        cachedModule_ = 0;
        cachedHitChange_ = 0;
    }

    if (cachedModule_ == 0) {
        if (!FindPlayerOwnedModules(*playerIns, cachedModule_, cachedHitChange_))
            return fail(WireReadStatus::ModuleNotFound);
        ++generation_;
        ++stats_.moduleChanges;
    }

    std::uint64_t handle = 0;
    if (reader_.ReadBytes(cachedModule_ + layout_.handleOffset,
                          reinterpret_cast<std::uint8_t*>(&handle), sizeof(handle)) !=
        ProcessReaderResult::Success) {
        return fail(WireReadStatus::ReadFailed);
    }

    std::uint8_t onGround = 0;
    std::uint8_t inAir = 0;
    std::uint8_t flying = 0;
    if (reader_.ReadBytes(cachedHitChange_ + layout_.targetAvailableOffset, &onGround, 1) !=
            ProcessReaderResult::Success ||
        reader_.ReadBytes(cachedHitChange_ + layout_.airTargetAvailableOffset, &inAir, 1) !=
            ProcessReaderResult::Success ||
        reader_.ReadBytes(cachedHitChange_ + layout_.inFlightOffset, &flying, 1) !=
            ProcessReaderResult::Success) {
        return fail(WireReadStatus::ReadFailed);
    }

    sample.status = WireReadStatus::Ok;
    sample.groundTarget = onGround != 0;
    sample.airTarget = inAir != 0;
    sample.targetAvailable = sample.groundTarget || sample.airTarget;
    sample.inFlight = flying != 0;
    sample.moduleAddress = cachedModule_;
    sample.generation = generation_;
    sample.handle = handle;
    sample.inWireAction = handle != layout_.idleHandle;
    ++stats_.ok;
    return sample;
}

// ---------------------------------------------------------------------------

std::vector<WireEvent> WireActionEventDetector::EndOpen(std::int64_t timestampUs,
                                                        WireEventKind kind) {
    std::vector<WireEvent> events;
    if (!inAction_) return events;
    WireEvent event;
    event.kind = kind;
    event.timestampUs = timestampUs;
    event.handle = handle_;
    event.durationUs = kind == WireEventKind::Ended ? timestampUs - startedUs_ : 0;
    events.push_back(event);
    inAction_ = false;
    handle_ = 0;
    return events;
}

std::vector<WireEvent> WireActionEventDetector::Update(const WireObservation& obs) {
    std::vector<WireEvent> events;

    if (obs.continuityBreak || !obs.readOk) {
        // We did not see the window. Whatever happened in it is unknown, so an
        // open action is closed as unobserved and the next sample re-baselines.
        auto closed = EndOpen(obs.timestampUs, WireEventKind::EndedUnobserved);
        events.insert(events.end(), closed.begin(), closed.end());
        haveBaseline_ = false;
        return events;
    }

    if (!haveBaseline_ || obs.generation != generation_) {
        auto closed = EndOpen(obs.timestampUs, WireEventKind::EndedUnobserved);
        events.insert(events.end(), closed.begin(), closed.end());
        // Baseline only. An action already running when we first looked is not
        // reported as having started -- we never saw it start.
        haveBaseline_ = true;
        generation_ = obs.generation;
        inAction_ = obs.inWireAction;
        handle_ = obs.handle;
        startedUs_ = obs.timestampUs;
        return events;
    }

    if (inAction_ && obs.inWireAction && obs.handle != handle_) {
        // A different anchor without passing through idle: two actions, not
        // one long one.
        WireEvent ended;
        ended.kind = WireEventKind::Ended;
        ended.timestampUs = obs.timestampUs;
        ended.handle = handle_;
        ended.durationUs = obs.timestampUs - startedUs_;
        events.push_back(ended);
        inAction_ = false;
    }

    if (!inAction_ && obs.inWireAction) {
        inAction_ = true;
        handle_ = obs.handle;
        startedUs_ = obs.timestampUs;
        WireEvent started;
        started.kind = WireEventKind::Started;
        started.timestampUs = obs.timestampUs;
        started.handle = obs.handle;
        events.push_back(started);
    } else if (inAction_ && !obs.inWireAction) {
        WireEvent ended;
        ended.kind = WireEventKind::Ended;
        ended.timestampUs = obs.timestampUs;
        ended.handle = handle_;
        ended.durationUs = obs.timestampUs - startedUs_;
        events.push_back(ended);
        inAction_ = false;
        handle_ = 0;
    }
    return events;
}

std::vector<WireEvent> WireActionEventDetector::Finish(std::int64_t timestampUs) {
    return EndOpen(timestampUs, WireEventKind::EndedUnobserved);
}

} // namespace sekiro_haptics::process
