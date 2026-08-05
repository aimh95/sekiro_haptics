#include "sekiro_haptics/replay/TraceJsonl.hpp"

#include "sekiro_haptics/Json.hpp"

#include <cctype>
#include <cmath>
#include <sstream>

namespace sekiro_haptics::trace {

const char* ToString(TraceReadResult result) {
    switch (result) {
        case TraceReadResult::Success:
            return "Success";
        case TraceReadResult::EndOfTrace:
            return "EndOfTrace";
        case TraceReadResult::MalformedJson:
            return "MalformedJson";
        case TraceReadResult::MissingTimestamp:
            return "MissingTimestamp";
        case TraceReadResult::MissingSignal:
            return "MissingSignal";
        case TraceReadResult::OutOfOrderTimestamp:
            return "OutOfOrderTimestamp";
        case TraceReadResult::IoError:
            return "IoError";
    }
    return "Unknown";
}

namespace {

bool IsBlank(const std::string& line) {
    for (char c : line) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// Trace scalar fields ("value", and any extra field) are always stringified
// regardless of their JSON type, so GameSignal stays uniformly string-typed.
std::string JsonScalarToString(const json::JsonValue& v) {
    switch (v.Type()) {
        case json::JsonType::Null:
            return "";
        case json::JsonType::Bool:
            return v.AsBool() ? "true" : "false";
        case json::JsonType::Number: {
            double d = v.AsNumber();
            if (d == static_cast<double>(static_cast<long long>(d))) {
                return std::to_string(static_cast<long long>(d));
            }
            std::ostringstream oss;
            oss << d;
            return oss.str();
        }
        case json::JsonType::String:
            return v.AsString();
        case json::JsonType::Array:
        case json::JsonType::Object:
            // Not expected for trace scalar fields; not supported.
            return "";
    }
    return "";
}

void SetError(std::string* outError, std::size_t lineNumber, const std::string& reason) {
    if (outError != nullptr) {
        *outError = "line " + std::to_string(lineNumber) + ": " + reason;
    }
}

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

} // namespace

TraceReader::TraceReader(const std::string& path) : path_(path), stream_(path) {
    opened_ = stream_.is_open();
}

bool TraceReader::IsOpen() const {
    return opened_;
}

std::size_t TraceReader::LineNumber() const {
    return lineNumber_;
}

TraceReadResult TraceReader::ReadNext(GameSignal& outSignal, std::string* outError) {
    if (!opened_) {
        if (outError != nullptr) {
            *outError = "trace file is not open: " + path_;
        }
        return TraceReadResult::IoError;
    }

    std::string line;
    while (true) {
        if (!std::getline(stream_, line)) {
            return TraceReadResult::EndOfTrace;
        }
        ++lineNumber_;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!IsBlank(line)) {
            break;
        }
    }

    json::JsonParseResult parsed = json::ParseJson(line);
    if (!parsed.ok) {
        SetError(outError, lineNumber_, "malformed JSON: " + parsed.error);
        return TraceReadResult::MalformedJson;
    }
    if (!parsed.value.IsObject()) {
        SetError(outError, lineNumber_, "malformed JSON: expected a JSON object");
        return TraceReadResult::MalformedJson;
    }

    const json::JsonValue* timestamp = parsed.value.Find("timestampUs");
    if (timestamp == nullptr || !timestamp->IsNumber()) {
        SetError(outError, lineNumber_, "missing or non-numeric \"timestampUs\" field");
        return TraceReadResult::MissingTimestamp;
    }

    const json::JsonValue* signalName = parsed.value.Find("signal");
    if (signalName == nullptr || !signalName->IsString() || signalName->AsString().empty()) {
        SetError(outError, lineNumber_, "missing or empty \"signal\" field");
        return TraceReadResult::MissingSignal;
    }

    GameSignal signal;
    signal.timestamp = std::chrono::microseconds(static_cast<long long>(std::llround(timestamp->AsNumber())));
    signal.signal = signalName->AsString();

    if (const json::JsonValue* value = parsed.value.Find("value")) {
        signal.value = JsonScalarToString(*value);
    }

    for (const auto& member : parsed.value.AsObject()) {
        if (member.first == "timestampUs" || member.first == "signal" || member.first == "value") {
            continue;
        }
        signal.extra[member.first] = JsonScalarToString(member.second);
    }

    bool outOfOrder = haveLastTimestamp_ && signal.timestamp < lastTimestamp_;
    haveLastTimestamp_ = true;
    lastTimestamp_ = signal.timestamp;

    if (outOfOrder) {
        outSignal = signal;
        SetError(outError, lineNumber_, "timestampUs decreased from the previous line");
        return TraceReadResult::OutOfOrderTimestamp;
    }

    outSignal = std::move(signal);
    return TraceReadResult::Success;
}

bool TraceReader::Rewind() {
    stream_.close();
    stream_.clear();
    stream_.open(path_);
    opened_ = stream_.is_open();
    lineNumber_ = 0;
    haveLastTimestamp_ = false;
    lastTimestamp_ = std::chrono::microseconds{0};
    return opened_;
}

TraceWriter::TraceWriter(const std::string& path) : stream_(path, std::ios::trunc) {}

bool TraceWriter::IsOpen() const {
    return stream_.is_open();
}

bool TraceWriter::WriteSignal(const GameSignal& signal) {
    if (!stream_.is_open()) {
        return false;
    }

    stream_ << "{\"timestampUs\":" << signal.timestamp.count();
    stream_ << ",\"signal\":";
    WriteJsonString(stream_, signal.signal);
    stream_ << ",\"value\":";
    WriteJsonString(stream_, signal.value);
    for (const auto& entry : signal.extra) {
        stream_ << ',';
        WriteJsonString(stream_, entry.first);
        stream_ << ':';
        WriteJsonString(stream_, entry.second);
    }
    stream_ << "}\n";

    return static_cast<bool>(stream_);
}

} // namespace sekiro_haptics::trace
