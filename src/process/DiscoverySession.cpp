#include "sekiro_haptics/process/DiscoverySession.hpp"

#include <cmath>
#include <ios>
#include <sstream>

namespace sekiro_haptics::process {

namespace {

void WriteJsonString(std::ostream& out, const std::string& text) {
    out << '"';
    for (char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    out << '"';
}

std::string ToHexAddress(std::uintptr_t address) {
    std::ostringstream oss;
    oss << "0x" << std::hex << address;
    return oss.str();
}

void WriteCandidateValueJson(std::ostream& out, CandidateValueType type, const CandidateValue& value) {
    switch (type) {
        case CandidateValueType::U8:
            out << static_cast<unsigned int>(std::get<std::uint8_t>(value));
            return;
        case CandidateValueType::U16:
            out << std::get<std::uint16_t>(value);
            return;
        case CandidateValueType::U32:
            out << std::get<std::uint32_t>(value);
            return;
        case CandidateValueType::I32:
            out << std::get<std::int32_t>(value);
            return;
        case CandidateValueType::F32: {
            float f = std::get<float>(value);
            // JSON has no NaN/Infinity literal -- represented as a string
            // rather than silently coerced to some numeric placeholder.
            if (std::isnan(f)) {
                out << "\"NaN\"";
            } else if (std::isinf(f)) {
                out << (f > 0.0f ? "\"Infinity\"" : "\"-Infinity\"");
            } else {
                out << f;
            }
            return;
        }
    }
}

} // namespace

bool WriteDiscoverySessionMetadata(const std::string& path, const DiscoverySessionMetadata& metadata) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "{";
    out << "\"schemaVersion\":" << metadata.schemaVersion << ",";

    out << "\"processImagePath\":";
    WriteJsonString(out, metadata.processImagePath);
    out << ",";

    out << "\"executableFileSizeBytes\":" << metadata.executableFileSizeBytes << ",";

    out << "\"sha256\":";
    WriteJsonString(out, metadata.sha256Hex);
    out << ",";

    out << "\"mainModuleName\":";
    WriteJsonString(out, metadata.mainModuleName);
    out << ",";

    out << "\"mainModuleBaseAddress\":";
    WriteJsonString(out, ToHexAddress(metadata.mainModuleBaseAddress));
    out << ",";

    out << "\"mainModuleImageSize\":" << metadata.mainModuleImageSize << ",";
    out << "\"pid\":" << metadata.pid << ",";

    out << "\"sessionStartWallClock\":";
    WriteJsonString(out, metadata.sessionStartWallClock);
    out << ",";

    out << "\"selectedScope\":";
    WriteJsonString(out, metadata.selectedScope);
    out << ",";

    out << "\"selectedValueType\":";
    WriteJsonString(out, metadata.selectedValueType);
    out << ",";

    out << "\"samplingIntervalMs\":" << metadata.samplingIntervalMs << ",";

    out << "\"toolVersion\":";
    WriteJsonString(out, metadata.toolVersion);
    out << ",";

    out << "\"endedNormally\":" << (metadata.endedNormally ? "true" : "false") << ",";

    out << "\"endReason\":";
    WriteJsonString(out, metadata.endReason);

    out << "}";

    return static_cast<bool>(out);
}

DiscoveryWatchWriter::DiscoveryWatchWriter(const std::string& path) : stream_(path, std::ios::trunc) {}

DiscoveryWatchWriter::~DiscoveryWatchWriter() {
    Close();
}

bool DiscoveryWatchWriter::IsOpen() const {
    return stream_.is_open();
}

bool DiscoveryWatchWriter::WriteSample(std::int64_t timestampUs, const std::string& signalName,
                                        std::uintptr_t runtimeAddress, CandidateValueType valueType,
                                        const CandidateValue& value) {
    if (!stream_.is_open()) {
        return false;
    }

    stream_ << "{\"schemaVersion\":" << kDiscoverySessionSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
    stream_ << ",\"recordKind\":\"sample\"";
    stream_ << ",\"signalName\":";
    WriteJsonString(stream_, signalName);
    stream_ << ",\"runtimeAddress\":";
    WriteJsonString(stream_, ToHexAddress(runtimeAddress));
    stream_ << ",\"valueType\":";
    WriteJsonString(stream_, ToString(valueType));
    stream_ << ",\"value\":";
    WriteCandidateValueJson(stream_, valueType, value);
    stream_ << "}\n";

    return static_cast<bool>(stream_);
}

bool DiscoveryWatchWriter::WriteMarker(std::int64_t timestampUs, const std::string& label) {
    if (!stream_.is_open()) {
        return false;
    }

    stream_ << "{\"schemaVersion\":" << kDiscoverySessionSchemaVersion;
    stream_ << ",\"timestampUs\":" << timestampUs;
    stream_ << ",\"recordKind\":\"marker\"";
    stream_ << ",\"label\":";
    WriteJsonString(stream_, label);
    stream_ << "}\n";

    return static_cast<bool>(stream_);
}

void DiscoveryWatchWriter::Close() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

} // namespace sekiro_haptics::process
