#include "sekiro_haptics/process/SekiroEnemyGuardReader.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace sekiro_haptics::process {

namespace {

bool Plausible(std::uintptr_t p) {
    return p > 0x10000 && p < 0x7FFF'FFFF'FFFFull;
}

std::int64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

SekiroEnemyGuardReader::SekiroEnemyGuardReader(IProcessReader& reader, IProcessInspector& inspector,
                                               KnownRootSpec worldChrManSpec,
                                               ExecutableIdentity expectedIdentity,
                                               ExecutableIdentity currentIdentity,
                                               std::uintptr_t moduleBaseAddress,
                                               EnemyGuardLayout layout)
    : reader_(reader),
      rootResolver_(reader, inspector, std::move(worldChrManSpec), std::move(expectedIdentity),
                    std::move(currentIdentity)),
      moduleBase_(moduleBaseAddress),
      layout_(layout) {
    scratch_.resize(std::max<std::size_t>(layout_.shared.searchWidthBytes,
                                          layout_.shared.outcomeOffset + 1));
}

void SekiroEnemyGuardReader::Invalidate() {
    tracked_.clear();
    samples_.clear();
    lastWorld_ = 0;
    lastPlayerIns_ = 0;
}

RootResolveResult SekiroEnemyGuardReader::Prime() {
    const auto resolved = rootResolver_.Resolve();
    primed_ = resolved.result == RootResolveResult::Resolved;
    return resolved.result;
}

std::optional<std::uintptr_t> SekiroEnemyGuardReader::ReadPointer(std::uintptr_t address) {
    if (!Plausible(address) || address > std::numeric_limits<std::uintptr_t>::max() - 8) return std::nullopt;
    std::uint64_t value = 0;
    if (reader_.ReadBytes(address, reinterpret_cast<std::uint8_t*>(&value), sizeof(value)) !=
        ProcessReaderResult::Success) {
        return std::nullopt;
    }
    return static_cast<std::uintptr_t>(value);
}

bool SekiroEnemyGuardReader::HasVftable(std::uintptr_t object, std::uint32_t rva) {
    const auto vptr = ReadPointer(object);
    return vptr.has_value() && *vptr == moduleBase_ + rva;
}

std::optional<std::uintptr_t> SekiroEnemyGuardReader::ModuleAtPath(std::uintptr_t character) {
    if (!pathKnown_) return std::nullopt;
    const auto container = ReadPointer(character + characterOffset_);
    if (!container.has_value() || !Plausible(*container)) return std::nullopt;
    const auto module = ReadPointer(*container + containerOffset_);
    if (!module.has_value() || !Plausible(*module)) return std::nullopt;
    if (!HasVftable(*module, layout_.shared.actionFlagVftableRva)) return std::nullopt;
    return *module;
}

std::optional<std::uintptr_t> SekiroEnemyGuardReader::PlayerModule(std::uintptr_t playerIns) {
    // The method already proven on the player: a container holding BOTH an
    // ActionFlagModule and a SprjPlayerDamageModule. Safe here precisely
    // because there is only one player -- the reason it is NOT safe on
    // enemies is that a second hop lands on a neighbour.
    const std::size_t width = layout_.shared.searchWidthBytes;
    std::vector<std::uint8_t> outer(width);
    if (playerIns > std::numeric_limits<std::uintptr_t>::max() - width) return std::nullopt;
    if (reader_.ReadBytes(playerIns, outer.data(), width) != ProcessReaderResult::Success)
        return std::nullopt;
    std::vector<std::uint8_t> inner(width);
    for (std::size_t o = 0; o + sizeof(std::uint64_t) <= width; o += sizeof(std::uint64_t)) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, outer.data() + o, sizeof(raw));
        const auto container = static_cast<std::uintptr_t>(raw);
        if (!Plausible(container) || container > std::numeric_limits<std::uintptr_t>::max() - width) continue;
        if (reader_.ReadBytes(container, inner.data(), width) != ProcessReaderResult::Success) continue;
        std::uintptr_t actionFlag = 0;
        bool hasPlayerDamage = false;
        for (std::size_t j = 0; j + sizeof(std::uint64_t) <= width; j += sizeof(std::uint64_t)) {
            std::uint64_t q = 0;
            std::memcpy(&q, inner.data() + j, sizeof(q));
            const auto candidate = static_cast<std::uintptr_t>(q);
            if (!Plausible(candidate)) continue;
            const auto vptr = ReadPointer(candidate);
            if (!vptr.has_value()) continue;
            if (*vptr == moduleBase_ + layout_.shared.actionFlagVftableRva && actionFlag == 0)
                actionFlag = candidate;
            else if (*vptr == moduleBase_ + layout_.playerDamageVftableRva)
                hasPlayerDamage = true;
        }
        if (actionFlag != 0 && hasPlayerDamage) return actionFlag;
    }
    return std::nullopt;
}

bool SekiroEnemyGuardReader::Calibrate(std::uintptr_t playerIns,
                                       const std::vector<std::uintptr_t>& characters) {
    pathKnown_ = false;
    stats_.pathKnown = false;
    const auto truth = PlayerModule(playerIns);
    if (!truth.has_value()) return false;

    // Every (ChrIns offset, container offset) from PlayerIns that reaches the
    // PROVEN module. The real path has to be one of these.
    const std::size_t width = layout_.shared.searchWidthBytes;
    const std::size_t innerWidth = layout_.containerScanBytes;
    std::vector<std::pair<std::size_t, std::size_t>> candidates;
    std::vector<std::uint8_t> outer(width);
    std::vector<std::uint8_t> inner(innerWidth);
    if (reader_.ReadBytes(playerIns, outer.data(), width) != ProcessReaderResult::Success) return false;
    for (std::size_t o = 0; o + sizeof(std::uint64_t) <= width; o += sizeof(std::uint64_t)) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, outer.data() + o, sizeof(raw));
        const auto container = static_cast<std::uintptr_t>(raw);
        if (!Plausible(container) || container > std::numeric_limits<std::uintptr_t>::max() - innerWidth) continue;
        if (reader_.ReadBytes(container, inner.data(), innerWidth) != ProcessReaderResult::Success) continue;
        for (std::size_t j = 0; j + sizeof(std::uint64_t) <= innerWidth; j += sizeof(std::uint64_t)) {
            std::uint64_t q = 0;
            std::memcpy(&q, inner.data() + j, sizeof(q));
            if (static_cast<std::uintptr_t>(q) == *truth) candidates.emplace_back(o, j);
        }
    }
    if (candidates.empty()) return false;

    // Of those, the pair that resolves the most enemies to DISTINCT modules.
    std::size_t bestScore = 0;
    for (const auto& pair : candidates) {
        std::vector<std::uintptr_t> modules;
        for (std::uintptr_t character : characters) {
            const auto container = ReadPointer(character + pair.first);
            if (!container.has_value() || !Plausible(*container)) continue;
            const auto module = ReadPointer(*container + pair.second);
            if (!module.has_value() || !Plausible(*module)) continue;
            if (!HasVftable(*module, layout_.shared.actionFlagVftableRva)) continue;
            modules.push_back(*module);
        }
        std::sort(modules.begin(), modules.end());
        const auto duplicatesStart = std::unique(modules.begin(), modules.end());
        const std::size_t distinct = static_cast<std::size_t>(duplicatesStart - modules.begin());
        // Sharing means the pair is wrong for someone, so it counts against it.
        const std::size_t score = distinct == modules.size() ? distinct : 0;
        if (score > bestScore) {
            bestScore = score;
            characterOffset_ = pair.first;
            containerOffset_ = pair.second;
            pathKnown_ = true;
        }
    }
    if (!pathKnown_) return false;
    if (stats_.pathKnown &&
        (stats_.pathCharacterOffset != characterOffset_ ||
         stats_.pathContainerOffset != containerOffset_)) {
        ++stats_.pathChanges;
    }
    stats_.pathKnown = true;
    stats_.pathCharacterOffset = characterOffset_;
    stats_.pathContainerOffset = containerOffset_;
    stats_.pathSupport = bestScore;
    return true;
}

bool SekiroEnemyGuardReader::NeedsDiscovery() {
    if (!primed_) return true;
    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.shared.worldChrManVftableRva)) {
        return true;
    }
    const auto playerIns =
        ReadPointer(root.objectAddress + static_cast<std::uintptr_t>(layout_.shared.playerInsOffset));
    if (!playerIns.has_value()) return true;
    // A changed world or player object means every cached character address is
    // suspect, whatever its vftable pointer still reads as.
    return root.objectAddress != lastWorld_ || *playerIns != lastPlayerIns_;
}

std::size_t SekiroEnemyGuardReader::Discover() {
    const auto started = NowUs();
    ++stats_.discoveries;
    // Built into a local first. Clearing the live set up front would blind the
    // poll loop for the whole several-second walk.
    std::vector<Tracked> built;

    if (!primed_ && Prime() != RootResolveResult::Resolved) {
        stats_.lastDiscoveryUs = NowUs() - started;
        return 0;
    }
    const auto root = rootResolver_.Refresh();
    if (root.result != RootResolveResult::Resolved ||
        !HasVftable(root.objectAddress, layout_.shared.worldChrManVftableRva)) {
        stats_.lastDiscoveryUs = NowUs() - started;
        return 0;
    }
    lastWorld_ = root.objectAddress;
    const auto playerIns =
        ReadPointer(root.objectAddress + static_cast<std::uintptr_t>(layout_.shared.playerInsOffset));
    lastPlayerIns_ = playerIns.value_or(0);

    // Find the characters by TYPE rather than through an assumed enemy-list
    // offset: no such offset has been verified on this build.
    const std::size_t width = layout_.shared.searchWidthBytes;
    const auto wantEnemy = moduleBase_ + layout_.enemyInsVftableRva;
    std::vector<std::uintptr_t> characters;
    std::unordered_set<std::uintptr_t> walked;
    std::vector<std::uintptr_t> level{root.objectAddress};
    std::vector<std::uintptr_t> nextLevel;
    walked.insert(root.objectAddress);

    for (int depth = 0; depth <= layout_.shared.searchDepth; ++depth) {
        nextLevel.clear();
        for (std::uintptr_t node : level) {
            if (node > std::numeric_limits<std::uintptr_t>::max() - width) continue;
            if (reader_.ReadBytes(node, scratch_.data(), width) != ProcessReaderResult::Success) continue;
            for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= width; offset += sizeof(std::uint64_t)) {
                std::uint64_t raw = 0;
                std::memcpy(&raw, scratch_.data() + offset, sizeof(raw));
                const auto candidate = static_cast<std::uintptr_t>(raw);
                if (!Plausible(candidate)) continue;
                if (!walked.insert(candidate).second) continue;
                const auto vptr = ReadPointer(candidate);
                if (!vptr.has_value()) continue;
                if (*vptr == wantEnemy) characters.push_back(candidate);
                else if (depth < layout_.shared.searchDepth) nextLevel.push_back(candidate);
                if (walked.size() > layout_.discoveryBudget) goto walked_enough;
            }
        }
        level.swap(nextLevel);
    }
walked_enough:

    if (!playerIns.has_value() || !Calibrate(*playerIns, characters)) {
        stats_.lastDiscoveryUs = NowUs() - started;
        return 0;
    }
    // One module, one character. If two characters resolve to the same module
    // the path is wrong for at least one of them and there is no way to tell
    // which, so BOTH are dropped rather than reporting a guess.
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> claims;  // module -> character
    for (std::uintptr_t character : characters) {
        const auto module = ModuleAtPath(character);
        if (!module.has_value()) continue;
        auto it = std::find_if(claims.begin(), claims.end(),
                               [&](const auto& e) { return e.first == *module; });
        if (it != claims.end()) { it->second = 0; ++stats_.ambiguousModules; continue; }
        claims.emplace_back(*module, character);
    }
    for (const auto& claim : claims) {
        if (claim.second == 0) continue;
        Tracked entry;
        entry.character = claim.second;
        entry.module = claim.first;
        entry.generation = ++generation_;
        built.push_back(entry);
    }
    {
        std::lock_guard<std::mutex> lock(trackedMutex_);
        // Carry over what each character last reported, so a rediscovery does
        // not reset an outcome byte and manufacture a fresh edge.
        for (auto& entry : built) {
            for (const auto& old : tracked_) {
                if (old.character == entry.character && old.module == entry.module) {
                    entry.generation = old.generation;
                    entry.lastOutcome = old.lastOutcome;
                    entry.lastPulse = old.lastPulse;
                    break;
                }
            }
        }
        tracked_.swap(built);
        stats_.charactersTracked = tracked_.size();
    }
    stats_.lastDiscoveryUs = NowUs() - started;
    return stats_.charactersTracked;
}

const std::vector<EnemyGuardSample>& SekiroEnemyGuardReader::Poll() {
    ++stats_.polls;
    samples_.clear();
    {
        std::lock_guard<std::mutex> lock(trackedMutex_);
        pollCopy_ = tracked_;
    }
    std::vector<std::uintptr_t> dropped;

    for (auto& entry : pollCopy_) {
        std::uint8_t pulse = 0;
        if (reader_.ReadBytes(entry.module + layout_.shared.pulseOffset, &pulse, 1) !=
            ProcessReaderResult::Success) {
            // Unreadable means unloaded, not "no event".
            ++stats_.readFailures;
            dropped.push_back(entry.character);
            continue;
        }
        EnemyGuardSample sample;
        sample.character = entry.character;
        sample.moduleAddress = entry.module;
        sample.generation = entry.generation;
        sample.pulse = pulse;
        if (pulse != 0) ++stats_.nonZeroPulseSamples;
        if (entry.lastPulse == 0 && pulse != 0) ++stats_.rawPulseEdges;
        entry.lastPulse = pulse;
        if (pulse != 0) {
            // Only now is a second read worth paying for. Reading the outcome
            // byte for every tracked character on every poll cost one read per
            // character per tick and pushed the interval past the single frame
            // the pulse lasts.
            std::uint8_t outcome = 0;
            if (reader_.ReadBytes(entry.module + layout_.shared.outcomeOffset, &outcome, 1) ==
                ProcessReaderResult::Success) {
                entry.lastOutcome = outcome;
            }
            // A set pulse is the one moment worth re-proving the type, so a
            // recycled allocation cannot report an event.
            if (!HasVftable(entry.module, layout_.shared.actionFlagVftableRva)) {
                dropped.push_back(entry.character);
                continue;
            }
        }
        sample.outcome = entry.lastOutcome;
        samples_.push_back(sample);
    }

    std::lock_guard<std::mutex> lock(trackedMutex_);
    for (auto& live : tracked_) {
        for (const auto& copy : pollCopy_) {
            if (copy.character == live.character && copy.module == live.module) {
                live.lastOutcome = copy.lastOutcome;
                live.lastPulse = copy.lastPulse;
                break;
            }
        }
    }
    for (std::uintptr_t character : dropped) {
        const auto it = std::find_if(tracked_.begin(), tracked_.end(),
                                     [&](const Tracked& e) { return e.character == character; });
        if (it != tracked_.end()) {
            tracked_.erase(it);
            ++stats_.charactersDropped;
        }
    }
    stats_.charactersTracked = tracked_.size();
    return samples_;
}

void SekiroEnemyGuardReader::StartBackgroundDiscovery(float intervalSeconds) {
    if (discoveryRunning_.exchange(true)) return;
    discoveryIntervalSeconds_ = intervalSeconds > 0.5f ? intervalSeconds : 4.0f;
    discoveryThread_ = std::thread([this] {
        while (discoveryRunning_.load()) {
            Discover();
            // Slept in short slices so shutdown does not wait out a full
            // interval.
            const int slices = static_cast<int>(discoveryIntervalSeconds_ * 10.0f);
            for (int i = 0; i < slices && discoveryRunning_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void SekiroEnemyGuardReader::StopBackgroundDiscovery() {
    if (!discoveryRunning_.exchange(false)) return;
    if (discoveryThread_.joinable()) discoveryThread_.join();
}

} // namespace sekiro_haptics::process
