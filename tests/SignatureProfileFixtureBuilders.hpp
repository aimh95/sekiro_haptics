#pragma once

// Shared test-only JSON fixture builders for SignatureProfile-based tests
// (test_signature_profile_repository.cpp, test_address_resolver.cpp,
// test_signature_profile_positive_negative_fixtures.cpp). Every value here
// is synthetic ("fixture") -- never real Sekiro data. Lives under tests/,
// never included by production code.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

constexpr const char* kFixtureSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

inline std::filesystem::path SignatureProfileTestDir() {
    return std::filesystem::temp_directory_path() / "sh_signature_profile_tests";
}

inline std::filesystem::path WriteSignatureProfileTestJson(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(SignatureProfileTestDir());
    std::filesystem::path path = SignatureProfileTestDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

/// Builds one "addresses[]" entry's JSON. Mutate the fields you care about
/// from a default-constructed instance, then call Render().
struct AddressFields {
    std::string addressId = "fixture-address-1";
    std::string moduleName = "fixture_helper.exe";
    std::string scanOffset = "0x100";
    std::string scanSize = "0x100";
    std::string pattern = "AB CD ?? EF";
    std::string resolutionKind = "match_address";
    std::string matchOffset = "0x10";
    std::string instructionOffset = "0x0";
    std::string displacementOffset = "0x2";
    std::string instructionLength = "0x6";
    std::string targetRangeOffset = "0x0";
    std::string targetRangeSize = "0x10000";

    std::string Render() const {
        std::ostringstream oss;
        oss << "{\"addressId\":\"" << addressId << "\",\"moduleName\":\"" << moduleName << "\","
            << "\"scanOffset\":\"" << scanOffset << "\",\"scanSize\":\"" << scanSize << "\","
            << "\"pattern\":\"" << pattern << "\",\"resolutionKind\":\"" << resolutionKind << "\",";
        if (resolutionKind == "match_address") {
            oss << "\"matchOffset\":\"" << matchOffset << "\",";
        } else if (resolutionKind == "rip_relative_32") {
            oss << "\"instructionOffset\":\"" << instructionOffset << "\","
                << "\"displacementOffset\":\"" << displacementOffset << "\","
                << "\"instructionLength\":\"" << instructionLength << "\",";
        }
        oss << "\"targetRangeOffset\":\"" << targetRangeOffset << "\",\"targetRangeSize\":\"" << targetRangeSize
            << "\"}";
        return oss.str();
    }
};

/// Builds one "profiles[]" entry's JSON. Mutate fields (including
/// `addressesJson`, defaulting to one default AddressFields) from a
/// default-constructed instance, then call Render().
struct ProfileFields {
    std::string schemaVersion = "1"; // raw JSON number, not a string
    std::string profileId = "fixture-profile-1";
    std::string fileSizeBytes = "0x1000";
    std::string sha256 = kFixtureSha256;
    std::vector<std::string> addressesJson = {AddressFields{}.Render()};

    std::string Render() const {
        std::ostringstream oss;
        oss << "{\"schemaVersion\":" << schemaVersion << ",\"profileId\":\"" << profileId << "\","
            << "\"buildIdentity\":{\"fileSizeBytes\":\"" << fileSizeBytes << "\",\"sha256\":\"" << sha256 << "\"},"
            << "\"addresses\":[";
        for (std::size_t i = 0; i < addressesJson.size(); ++i) {
            if (i != 0) {
                oss << ",";
            }
            oss << addressesJson[i];
        }
        oss << "]}";
        return oss.str();
    }
};

inline std::string WrapSignatureProfiles(const std::vector<std::string>& profilesJson) {
    std::ostringstream oss;
    oss << "{\"profiles\":[";
    for (std::size_t i = 0; i < profilesJson.size(); ++i) {
        if (i != 0) {
            oss << ",";
        }
        oss << profilesJson[i];
    }
    oss << "]}";
    return oss.str();
}

} // namespace sekiro_haptics::process
