#include "sekiro_haptics/process/SekiroCombatCaptureAnalyzer.hpp"

#include "sekiro_haptics/Json.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace sekiro_haptics::process {

namespace {

struct ParsedRecord {
    std::int64_t timestampUs = 0;
    std::string recordKind;
    std::size_t offset = 0;   // "delta" only
    std::string label;        // "marker" only
};

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
    outRecord.timestampUs = static_cast<std::int64_t>(ts->AsNumber());

    if (outRecord.recordKind == "delta") {
        const json::JsonValue* offset = parsed.value.Find("offset");
        if (offset == nullptr || !offset->IsNumber()) {
            outError = "delta record missing offset";
            return false;
        }
        outRecord.offset = static_cast<std::size_t>(offset->AsNumber());
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

} // namespace

CombatCaptureAnalysisReport AnalyzeCombatCaptureFile(const std::string& path, std::int64_t windowUs) {
    CombatCaptureAnalysisReport report;

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

    std::vector<const ParsedRecord*> markers;
    for (const ParsedRecord& record : records) {
        if (record.recordKind == "delta") {
            ++report.totalDeltaRecords;
        } else if (record.recordKind == "marker") {
            ++report.totalMarkers;
            markers.push_back(&record);
        } else if (record.recordKind == "discontinuity") {
            ++report.totalDiscontinuities;
        } else if (record.recordKind == "dropped") {
            ++report.totalDropped;
        }
    }

    std::map<std::string, std::uint64_t> occurrencesByLabel;
    for (const ParsedRecord* marker : markers) {
        ++occurrencesByLabel[marker->label];
    }

    // (offset, label) -> change count.
    std::map<std::pair<std::size_t, std::string>, std::uint64_t> changeCounts;
    for (const ParsedRecord& record : records) {
        if (record.recordKind != "delta") {
            continue;
        }
        for (const ParsedRecord* marker : markers) {
            std::int64_t distance = record.timestampUs - marker->timestampUs;
            if (distance < 0) {
                distance = -distance;
            }
            if (distance <= windowUs) {
                ++changeCounts[{record.offset, marker->label}];
            }
        }
    }

    report.offsetMarkerStats.reserve(changeCounts.size());
    for (const auto& entry : changeCounts) {
        CombatCaptureOffsetMarkerStat stat;
        stat.offset = entry.first.first;
        stat.label = entry.first.second;
        stat.changeCount = entry.second;
        stat.markerOccurrences = occurrencesByLabel[stat.label];
        report.offsetMarkerStats.push_back(stat);
    }
    std::sort(report.offsetMarkerStats.begin(), report.offsetMarkerStats.end(),
              [](const CombatCaptureOffsetMarkerStat& a, const CombatCaptureOffsetMarkerStat& b) {
                  return a.changeCount > b.changeCount;
              });

    report.ok = true;
    return report;
}

} // namespace sekiro_haptics::process
