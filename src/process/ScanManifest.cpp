#include "sekiro_haptics/process/ScanManifest.hpp"

#include "sekiro_haptics/Json.hpp"

#include <fstream>
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

bool ParseHexAddress(const std::string& text, std::uintptr_t& outAddress) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        unsigned long long value = std::stoull(text.substr(2), &consumed, 16);
        if (consumed != text.size() - 2) {
            return false;
        }
        outAddress = static_cast<std::uintptr_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadFileToString(const std::filesystem::path& path, std::string& outContent) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    outContent = buffer.str();
    return true;
}

} // namespace

const char* ToString(ScanManifestState state) {
    switch (state) {
        case ScanManifestState::Planning:
            return "Planning";
        case ScanManifestState::WritingBaseline:
            return "WritingBaseline";
        case ScanManifestState::BaselineComplete:
            return "BaselineComplete";
        case ScanManifestState::Filtering:
            return "Filtering";
        case ScanManifestState::CandidatesComplete:
            return "CandidatesComplete";
        case ScanManifestState::Interrupted:
            return "Interrupted";
        case ScanManifestState::Failed:
            return "Failed";
    }
    return "Unknown";
}

bool ParseScanManifestState(const std::string& text, ScanManifestState& outState) {
    if (text == "Planning") {
        outState = ScanManifestState::Planning;
    } else if (text == "WritingBaseline") {
        outState = ScanManifestState::WritingBaseline;
    } else if (text == "BaselineComplete") {
        outState = ScanManifestState::BaselineComplete;
    } else if (text == "Filtering") {
        outState = ScanManifestState::Filtering;
    } else if (text == "CandidatesComplete") {
        outState = ScanManifestState::CandidatesComplete;
    } else if (text == "Interrupted") {
        outState = ScanManifestState::Interrupted;
    } else if (text == "Failed") {
        outState = ScanManifestState::Failed;
    } else {
        return false;
    }
    return true;
}

bool ScanSessionIdentity::operator==(const ScanSessionIdentity& other) const {
    return executableFileSizeBytes == other.executableFileSizeBytes && sha256Hex == other.sha256Hex &&
           pid == other.pid && mainModuleBaseAddress == other.mainModuleBaseAddress &&
           mainModuleImageSize == other.mainModuleImageSize && valueType == other.valueType && scope == other.scope;
}

bool ScanSessionIdentity::operator!=(const ScanSessionIdentity& other) const {
    return !(*this == other);
}

bool WriteScanManifest(const std::filesystem::path& path, const ScanManifest& manifest) {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    out << "{";
    out << "\"schemaVersion\":" << manifest.schemaVersion << ",";
    out << "\"storageFormatVersion\":" << manifest.storageFormatVersion << ",";

    out << "\"identity\":{";
    out << "\"executableFileSizeBytes\":" << manifest.identity.executableFileSizeBytes << ",";
    out << "\"sha256Hex\":";
    WriteJsonString(out, manifest.identity.sha256Hex);
    out << ",";
    out << "\"pid\":" << manifest.identity.pid << ",";
    out << "\"mainModuleBaseAddress\":";
    WriteJsonString(out, ToHexAddress(manifest.identity.mainModuleBaseAddress));
    out << ",";
    out << "\"mainModuleImageSize\":" << manifest.identity.mainModuleImageSize << ",";
    out << "\"valueType\":";
    WriteJsonString(out, manifest.identity.valueType);
    out << ",";
    out << "\"scope\":";
    WriteJsonString(out, manifest.identity.scope);
    out << "},";

    out << "\"alignment\":" << manifest.alignment << ",";

    out << "\"regions\":[";
    for (std::size_t i = 0; i < manifest.regions.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        const ScanManifestRegion& region = manifest.regions[i];
        out << "{\"baseAddress\":";
        WriteJsonString(out, ToHexAddress(region.baseAddress));
        out << ",\"sizeBytes\":" << region.sizeBytes;
        out << ",\"kind\":";
        WriteJsonString(out, ToString(region.kind));
        out << "}";
    }
    out << "],";

    out << "\"totalScopeBytes\":" << manifest.totalScopeBytes << ",";
    out << "\"totalValueCount\":" << manifest.totalValueCount << ",";
    out << "\"processedBytes\":" << manifest.processedBytes << ",";
    out << "\"coveragePercent\":" << manifest.coveragePercent << ",";
    out << "\"memoryBudgetBytes\":" << manifest.memoryBudgetBytes << ",";
    out << "\"generation\":" << manifest.generation << ",";
    out << "\"candidateCount\":" << manifest.candidateCount << ",";

    out << "\"state\":";
    WriteJsonString(out, ToString(manifest.state));
    out << ",";

    out << "\"failureReason\":";
    WriteJsonString(out, manifest.failureReason);
    out << ",";

    out << "\"startMonotonicUs\":" << manifest.startMonotonicUs << ",";
    out << "\"completeMonotonicUs\":" << manifest.completeMonotonicUs << ",";
    out << "\"completedNormally\":" << (manifest.completedNormally ? "true" : "false");
    out << "}";

    out.flush();
    return static_cast<bool>(out);
}

bool ReadScanManifest(const std::filesystem::path& path, ScanManifest& outManifest, std::string& outError) {
    std::string content;
    if (!ReadFileToString(path, content)) {
        outError = "could not open file: " + path.string();
        return false;
    }

    json::JsonParseResult parsed = json::ParseJson(content);
    if (!parsed.ok) {
        outError = "JSON parse error: " + parsed.error;
        return false;
    }
    if (!parsed.value.IsObject()) {
        outError = "top-level JSON value must be an object";
        return false;
    }
    const json::JsonValue& root = parsed.value;

    ScanManifest manifest;

    const json::JsonValue* schemaVersion = root.Find("schemaVersion");
    if (schemaVersion == nullptr || !schemaVersion->IsNumber()) {
        outError = "missing or non-numeric \"schemaVersion\"";
        return false;
    }
    manifest.schemaVersion = static_cast<int>(schemaVersion->AsNumber());

    const json::JsonValue* storageFormatVersion = root.Find("storageFormatVersion");
    if (storageFormatVersion == nullptr || !storageFormatVersion->IsNumber()) {
        outError = "missing or non-numeric \"storageFormatVersion\"";
        return false;
    }
    manifest.storageFormatVersion = static_cast<int>(storageFormatVersion->AsNumber());

    const json::JsonValue* identity = root.Find("identity");
    if (identity == nullptr || !identity->IsObject()) {
        outError = "missing \"identity\" object";
        return false;
    }
    manifest.identity.executableFileSizeBytes =
        static_cast<std::uint64_t>(identity->GetNumber("executableFileSizeBytes", 0.0));
    manifest.identity.sha256Hex = identity->GetString("sha256Hex");
    manifest.identity.pid = static_cast<std::uint32_t>(identity->GetNumber("pid", 0.0));
    {
        std::string hexAddr = identity->GetString("mainModuleBaseAddress");
        std::uintptr_t address = 0;
        if (!hexAddr.empty() && !ParseHexAddress(hexAddr, address)) {
            outError = "invalid \"identity.mainModuleBaseAddress\" hex address";
            return false;
        }
        manifest.identity.mainModuleBaseAddress = address;
    }
    manifest.identity.mainModuleImageSize = static_cast<std::size_t>(identity->GetNumber("mainModuleImageSize", 0.0));
    manifest.identity.valueType = identity->GetString("valueType");
    manifest.identity.scope = identity->GetString("scope");

    manifest.alignment = static_cast<std::size_t>(root.GetNumber("alignment", 0.0));

    const json::JsonValue* regions = root.Find("regions");
    if (regions == nullptr || !regions->IsArray()) {
        outError = "missing \"regions\" array";
        return false;
    }
    for (const json::JsonValue& entry : regions->AsArray()) {
        if (!entry.IsObject()) {
            outError = "a \"regions\" entry is not a JSON object";
            return false;
        }
        ScanManifestRegion region;
        std::string hexAddr = entry.GetString("baseAddress");
        if (!ParseHexAddress(hexAddr, region.baseAddress)) {
            outError = "invalid region \"baseAddress\" hex address";
            return false;
        }
        region.sizeBytes = static_cast<std::size_t>(entry.GetNumber("sizeBytes", 0.0));
        std::string kindText = entry.GetString("kind");
        if (kindText == "Image") {
            region.kind = MemoryRegionKind::Image;
        } else if (kindText == "Mapped") {
            region.kind = MemoryRegionKind::Mapped;
        } else if (kindText == "Private") {
            region.kind = MemoryRegionKind::Private;
        } else {
            outError = "invalid region \"kind\": " + kindText;
            return false;
        }
        manifest.regions.push_back(region);
    }

    manifest.totalScopeBytes = static_cast<std::uint64_t>(root.GetNumber("totalScopeBytes", 0.0));
    manifest.totalValueCount = static_cast<std::uint64_t>(root.GetNumber("totalValueCount", 0.0));
    manifest.processedBytes = static_cast<std::uint64_t>(root.GetNumber("processedBytes", 0.0));
    manifest.coveragePercent = root.GetNumber("coveragePercent", 0.0);
    manifest.memoryBudgetBytes = static_cast<std::size_t>(root.GetNumber("memoryBudgetBytes", 0.0));
    manifest.generation = static_cast<int>(root.GetNumber("generation", 0.0));
    manifest.candidateCount = static_cast<std::uint64_t>(root.GetNumber("candidateCount", 0.0));

    std::string stateText = root.GetString("state");
    if (!ParseScanManifestState(stateText, manifest.state)) {
        outError = "invalid or missing \"state\": " + stateText;
        return false;
    }

    manifest.failureReason = root.GetString("failureReason");
    manifest.startMonotonicUs = static_cast<std::int64_t>(root.GetNumber("startMonotonicUs", 0.0));
    manifest.completeMonotonicUs = static_cast<std::int64_t>(root.GetNumber("completeMonotonicUs", 0.0));
    manifest.completedNormally = root.GetBool("completedNormally", false);

    outManifest = std::move(manifest);
    return true;
}

} // namespace sekiro_haptics::process
