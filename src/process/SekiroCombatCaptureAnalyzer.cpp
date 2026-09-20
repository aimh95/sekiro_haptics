#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"

#include "sekiro_haptics/Json.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace sekiro_haptics::process {

namespace {

constexpr const char* kGuardInputLabel = "guard_input";

struct ParsedRecord {
    std::int64_t timestampUs = 0;
    std::string recordKind;
    std::size_t offset = 0;        // "delta" only
    std::string label;             // "marker" only
    std::uint32_t previousU32 = 0; // "delta" only
    std::uint32_t currentU32 = 0;  // "delta" only
};

std::uint32_t HexBytesToU32LittleEndian(const std::string& hex) {
    std::uint8_t bytes[4] = {0, 0, 0, 0};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (int i = 0; i < 4 && static_cast<std::size_t>(i * 2 + 1) < hex.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    }
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

bool ParseLine(const std::string& line, ParsedRecord& outRecord, std::string& outError) {
    json::JsonParseResult parsed = json::ParseJson(line);
    if (!parsed.ok) {
        outError = parsed.error;
        return false;
    }

    const json::JsonValue* kind = parsed.value.Find("recordKind");
    if (kind == nullptr || !kind->IsString()) {
        outError = "missing recordKind";
        return false;
    }
    outRecord.recordKind = kind->AsString();

    const json::JsonValue* ts = parsed.value.Find("timestampUs");
    if (ts == nullptr || !ts->IsNumber()) {
        outError = "missing timestampUs";
        return false;
    }
    if (!std::isfinite(ts->AsNumber()) || ts->AsNumber() < 0 ||
        ts->AsNumber() > 9'007'199'254'740'991.0 || std::floor(ts->AsNumber()) != ts->AsNumber()) {
        outError = "timestampUs is not a nonnegative exact integer";
        return false;
    }
    outRecord.timestampUs = static_cast<std::int64_t>(ts->AsNumber());
    if (const auto* schema = parsed.value.Find("schemaVersion"); schema &&
        schema->IsNumber() && schema->AsNumber() >= 3) {
        outError = "schema v3 requires tools/deflect_capture.py (legacy guard correlation does not validate sample commits)";
        return false;
    }

    if (outRecord.recordKind == "delta") {
        const json::JsonValue* offset = parsed.value.Find("offset");
        if (offset == nullptr || !offset->IsNumber()) {
            outError = "delta record missing offset";
            return false;
        }
        if (!std::isfinite(offset->AsNumber()) || offset->AsNumber() < 0 ||
            offset->AsNumber() > 65532 || std::fmod(offset->AsNumber(), 4.0) != 0) {
            outError = "invalid aligned 4-byte offset";
            return false;
        }
        outRecord.offset = static_cast<std::size_t>(offset->AsNumber());

        const json::JsonValue* prevHex = parsed.value.Find("previousBytesHex");
        const json::JsonValue* curHex = parsed.value.Find("currentBytesHex");
        if (prevHex == nullptr || !prevHex->IsString() || curHex == nullptr || !curHex->IsString()) {
            outError = "delta record missing previous/currentBytesHex";
            return false;
        }
        const auto validHex = [](const std::string& text) {
            return text.size() == 8 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
        };
        if (!validHex(prevHex->AsString()) || !validHex(curHex->AsString())) {
            outError = "delta hex must contain exactly four bytes";
            return false;
        }
        outRecord.previousU32 = HexBytesToU32LittleEndian(prevHex->AsString());
        outRecord.currentU32 = HexBytesToU32LittleEndian(curHex->AsString());
    } else if (outRecord.recordKind == "marker") {
        const json::JsonValue* label = parsed.value.Find("label");
        if (label == nullptr || !label->IsString()) {
            outError = "marker record missing label";
            return false;
        }
        outRecord.label = label->AsString();
    }
    return true;
}

struct Transition {
    std::int64_t timestampUs = 0;
    std::uint32_t prevU32 = 0;
    std::uint32_t curU32 = 0;
};

struct Trial {
    std::int64_t anchorUs = 0;
    std::string label;
};

struct TrialBuildResult {
    std::vector<Trial> trials;
    std::uint64_t orphanMarkersExcluded = 0;
    std::uint64_t ambiguousMarkersExcluded = 0;
};

/// Links each non-guard_input marker, in time order, to the unique
/// not-yet-claimed "guard_input" within `lookbackUs` at or before it --
/// see the header's trial definition. Zero candidates: orphan, excluded.
/// More than one candidate: ambiguous (which press it belongs to is
/// genuinely unknown), excluded -- the candidates stay unclaimed rather
/// than guessed at, so a later, more precisely-timed marker can still claim
/// one of them unambiguously. Every guard_input still unclaimed once all
/// outcome markers are processed becomes its own "guard_input"-labeled
/// trial.
TrialBuildResult BuildTrials(const std::vector<ParsedRecord>& markers, std::int64_t lookbackUs) {
    std::vector<const ParsedRecord*> guardInputs;
    std::vector<const ParsedRecord*> outcomeMarkers;
    for (const ParsedRecord& m : markers) {
        if (m.label == kGuardInputLabel) {
            guardInputs.push_back(&m);
        } else {
            outcomeMarkers.push_back(&m);
        }
    }
    std::sort(guardInputs.begin(), guardInputs.end(),
              [](const ParsedRecord* a, const ParsedRecord* b) { return a->timestampUs < b->timestampUs; });
    std::sort(outcomeMarkers.begin(), outcomeMarkers.end(),
              [](const ParsedRecord* a, const ParsedRecord* b) { return a->timestampUs < b->timestampUs; });

    std::vector<bool> guardClaimed(guardInputs.size(), false);
    TrialBuildResult result;

    for (const ParsedRecord* marker : outcomeMarkers) {
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < guardInputs.size(); ++i) {
            if (guardClaimed[i] || guardInputs[i]->timestampUs > marker->timestampUs) {
                continue;
            }
            if (marker->timestampUs - guardInputs[i]->timestampUs <= lookbackUs) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            ++result.orphanMarkersExcluded;
        } else if (candidates.size() > 1) {
            ++result.ambiguousMarkersExcluded;
        } else {
            std::size_t claimedIdx = candidates.front();
            guardClaimed[claimedIdx] = true;
            result.trials.push_back(Trial{guardInputs[claimedIdx]->timestampUs, marker->label});
        }
    }
    for (std::size_t i = 0; i < guardInputs.size(); ++i) {
        if (!guardClaimed[i]) {
            result.trials.push_back(Trial{guardInputs[i]->timestampUs, std::string(kGuardInputLabel)});
        }
    }
    return result;
}

struct ChangeInfo {
    std::uint32_t beforeU32 = 0;
    std::uint32_t afterU32 = 0;
    std::int64_t firstChangeUs = 0; // relative to the trial anchor
    std::int64_t durationHeldUs = -1;
};

/// Value of this offset immediately before/at `ts` -- the last transition's
/// "current" value at or before `ts`, or the very first transition's
/// "previous" value if `ts` precedes every recorded transition.
std::optional<ChangeInfo> ComputeChangeInfo(const std::vector<Transition>& transitions, std::int64_t anchorUs,
                                             std::int64_t windowUs) {
    if (transitions.empty()) {
        return std::nullopt;
    }
    auto begin = std::lower_bound(transitions.begin(), transitions.end(), anchorUs,
                                   [](const Transition& t, std::int64_t value) { return t.timestampUs < value; });
    auto end = std::upper_bound(transitions.begin(), transitions.end(), anchorUs + windowUs,
                                 [](std::int64_t value, const Transition& t) { return value < t.timestampUs; });
    if (begin == end) {
        return std::nullopt; // no change within the window
    }

    ChangeInfo info;
    info.beforeU32 = begin->prevU32; // Includes a transition exactly at the anchor.
    info.firstChangeUs = begin->timestampUs - anchorUs;

    // `end` is the first transition strictly after the window (or
    // transitions.end() if none) -- exactly the "next transition after the
    // one that set the after-value" durationHeldUs needs.
    auto lastInWindow = std::prev(end);
    info.afterU32 = lastInWindow->curU32;
    if (end != transitions.end()) {
        info.durationHeldUs = end->timestampUs - lastInWindow->timestampUs;
    }
    return info;
}

std::vector<CombatCaptureBitFlip> ComputeBitFlips(std::uint32_t before, std::uint32_t after) {
    std::vector<CombatCaptureBitFlip> flips;
    std::uint32_t diff = before ^ after;
    for (int bit = 0; bit < 32; ++bit) {
        if ((diff >> bit) & 1u) {
            flips.push_back(CombatCaptureBitFlip{bit, ((after >> bit) & 1u) != 0});
        }
    }
    return flips;
}

} // namespace

CombatCaptureAnalysisReport AnalyzeCombatCaptureFile(const std::string& path, std::int64_t windowUs,
                                                      std::int64_t guardInputLookbackUs) {
    CombatCaptureAnalysisReport report;
    if (windowUs < 0 || windowUs > 60'000'000 || guardInputLookbackUs < 0 || guardInputLookbackUs > 60'000'000) {
        report.error = "analysis windows must be between 0 and 60000000 microseconds";
        return report;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        report.error = "could not open capture file: " + path;
        return report;
    }

    std::vector<ParsedRecord> records;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty()) {
            continue;
        }
        ParsedRecord record;
        std::string parseError;
        if (!ParseLine(line, record, parseError)) {
            std::ostringstream oss;
            oss << "malformed line " << lineNumber << ": " << parseError;
            report.error = oss.str();
            return report;
        }
        records.push_back(std::move(record));
    }

    std::vector<ParsedRecord> markers;
    std::map<std::size_t, std::vector<Transition>> transitionsByOffset;
    for (const ParsedRecord& record : records) {
        if (record.recordKind == "delta") {
            ++report.totalDeltaRecords;
            transitionsByOffset[record.offset].push_back(
                Transition{record.timestampUs, record.previousU32, record.currentU32});
        } else if (record.recordKind == "marker") {
            ++report.totalMarkers;
            markers.push_back(record);
        } else if (record.recordKind == "discontinuity") {
            ++report.totalDiscontinuities;
        } else if (record.recordKind == "dropped") {
            ++report.totalDropped;
        }
    }
    for (auto& entry : transitionsByOffset) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const Transition& a, const Transition& b) { return a.timestampUs < b.timestampUs; });
    }

    TrialBuildResult trialBuild = BuildTrials(markers, guardInputLookbackUs);
    std::vector<Trial>& trials = trialBuild.trials;
    report.totalTrials = trials.size();
    report.orphanMarkersExcluded = trialBuild.orphanMarkersExcluded;
    report.ambiguousMarkersExcluded = trialBuild.ambiguousMarkersExcluded;

    // A unique guard/outcome pairing does NOT make the analysis windows
    // disjoint. Exclude ambiguous overlapping trials from both numerator
    // and denominator rather than crediting the same delta twice.
    std::vector<bool> excluded(trials.size(), false);
    for (std::size_t i = 0; i < trials.size(); ++i) {
        for (std::size_t j = i + 1; j < trials.size(); ++j) {
            if (std::abs(trials[i].anchorUs - trials[j].anchorUs) <= windowUs) {
                excluded[i] = excluded[j] = true;
            }
        }
    }
    for (std::size_t i = 0; i < trials.size(); ++i) {
        if (excluded[i]) { ++report.overlappingTrialsExcluded; continue; }
        for (const auto& record : records) {
            if ((record.recordKind == "discontinuity" || record.recordKind == "dropped") &&
                record.timestampUs >= trials[i].anchorUs &&
                record.timestampUs - trials[i].anchorUs <= windowUs) {
                excluded[i] = true;
                ++report.discontinuousTrialsExcluded;
                break;
            }
        }
    }
    std::vector<Trial> retained;
    for (std::size_t i = 0; i < trials.size(); ++i) if (!excluded[i]) retained.push_back(trials[i]);
    trials = std::move(retained);
    report.totalTrials = trials.size();

    std::set<std::string> distinctLabels;
    for (const Trial& t : trials) {
        distinctLabels.insert(t.label);
    }

    // Cache each trial's ChangeInfo per offset so it's computed once
    // regardless of how many (label, otherLabel) combinations need it.
    std::map<std::size_t, std::vector<std::optional<ChangeInfo>>> changeByOffsetThenTrial;
    for (const auto& entry : transitionsByOffset) {
        std::vector<std::optional<ChangeInfo>> perTrial;
        perTrial.reserve(trials.size());
        for (const Trial& t : trials) {
            auto info = ComputeChangeInfo(entry.second, t.anchorUs, windowUs);
            // Legacy delta-only data cannot prove a value persisted through
            // unrecorded samples. Do not report an inferred hold duration.
            if (info) info->durationHeldUs = -1;
            perTrial.push_back(info);
        }
        changeByOffsetThenTrial[entry.first] = std::move(perTrial);
    }

    for (const auto& offsetEntry : transitionsByOffset) {
        std::size_t offset = offsetEntry.first;
        const std::vector<std::optional<ChangeInfo>>& perTrial = changeByOffsetThenTrial[offset];

        for (const std::string& label : distinctLabels) {
            CombatCaptureOffsetTrialStat stat;
            stat.offset = offset;
            stat.label = label;
            bool exampleSet = false;

            for (std::size_t i = 0; i < trials.size(); ++i) {
                bool sameLabel = (trials[i].label == label);
                bool changed = perTrial[i].has_value();
                if (sameLabel) {
                    ++stat.trialsForLabel;
                    if (changed) {
                        ++stat.trialsChangedForLabel;
                        if (!exampleSet) {
                            const ChangeInfo& info = *perTrial[i];
                            stat.exampleBeforeU32 = info.beforeU32;
                            stat.exampleAfterU32 = info.afterU32;
                            stat.exampleFirstChangeUs = info.firstChangeUs;
                            stat.exampleDurationHeldUs = info.durationHeldUs;
                            stat.exampleBitFlips = ComputeBitFlips(info.beforeU32, info.afterU32);
                            exampleSet = true;
                        }
                    }
                } else {
                    ++stat.trialsForOtherLabels;
                    if (changed) {
                        ++stat.trialsChangedForOtherLabels;
                    }
                }
            }

            if (stat.trialsChangedForLabel > 0) {
                report.offsetStats.push_back(std::move(stat));
            }
        }
    }

    std::sort(report.offsetStats.begin(), report.offsetStats.end(),
              [](const CombatCaptureOffsetTrialStat& a, const CombatCaptureOffsetTrialStat& b) {
                  if (a.SupportRate() != b.SupportRate()) {
                      return a.SupportRate() > b.SupportRate();
                  }
                  return a.FalsePositiveRate() < b.FalsePositiveRate();
              });

    report.ok = true;
    return report;
}

} // namespace sekiro_haptics::process
