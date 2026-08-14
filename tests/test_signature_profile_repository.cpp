// Unit tests for SignatureProfileRepository: strict JSON parsing/validation
// (fail-closed, whole-file rejection) and exact-build-identity selection.
// No real process, no real Sekiro data -- every profile/pattern here is a
// synthetic "fixture" value. See docs/05-process-access.md.

#include "sekiro_haptics/process/SignatureProfileRepository.hpp"
#include "testing.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path TestDir() {
    return std::filesystem::temp_directory_path() / "sh_signature_profile_tests";
}

std::filesystem::path WriteTempJson(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(TestDir());
    std::filesystem::path path = TestDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return path;
}

constexpr const char* kValidSha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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

struct ProfileFields {
    std::string schemaVersion = "1"; // raw JSON number, not a string
    std::string profileId = "fixture-profile-1";
    std::string fileSizeBytes = "0x1000";
    std::string sha256 = kValidSha256;
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

std::string WrapProfiles(const std::vector<std::string>& profilesJson) {
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

} // namespace

// ===========================================================================
// Profile parsing
// ===========================================================================

SH_TEST(SignatureProfileRepository_LoadFromFile_ValidProfile_LoadsSuccessfully) {
    std::string json = WrapProfiles({ProfileFields{}.Render()});
    std::filesystem::path path = WriteTempJson("valid.json", json);

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok);
    SH_CHECK(outcome.loadedCount == 1);
    SH_CHECK(repo.Size() == 1);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_UnsupportedSchemaVersion_Rejected) {
    ProfileFields fields;
    fields.schemaVersion = "2";
    std::filesystem::path path = WriteTempJson("bad_schema.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
    SH_CHECK(repo.Size() == 0);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_MalformedJson_Rejected) {
    std::filesystem::path path = WriteTempJson("malformed.json", "{ this is not valid json");

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
    SH_CHECK(!outcome.fatalError.empty());
}

SH_TEST(SignatureProfileRepository_LoadFromFile_MissingRequiredField_Rejected) {
    // Hand-built JSON missing "profileId" entirely.
    std::string json =
        "{\"profiles\":[{\"schemaVersion\":1,\"buildIdentity\":{\"fileSizeBytes\":\"0x1000\",\"sha256\":\"" +
        std::string(kValidSha256) + "\"},\"addresses\":[" + AddressFields{}.Render() + "]}]}";
    std::filesystem::path path = WriteTempJson("missing_field.json", json);

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_ShaWrongLength_Rejected) {
    ProfileFields fields;
    fields.sha256 = std::string(kValidSha256).substr(0, 63); // 63 chars, not 64
    std::filesystem::path path = WriteTempJson("sha_short.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_ShaNonHex_Rejected) {
    ProfileFields fields;
    fields.sha256 = std::string(63, 'a') + "g"; // 64 chars but last is not hex
    std::filesystem::path path = WriteTempJson("sha_nonhex.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_UppercaseSha_CanonicalizedToLowercase) {
    std::string upper = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    ProfileFields fields;
    fields.sha256 = upper;
    std::filesystem::path path = WriteTempJson("sha_upper.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());
    SH_CHECK(outcome.ok);

    ExecutableIdentity identity;
    identity.fileSizeBytes = 0x1000;
    // Build the digest from the same 64 lowercase hex characters the
    // profile canonicalizes to, regardless of the uppercase JSON input.
    Sha256Digest digest;
    std::string lower = kValidSha256;
    for (std::size_t i = 0; i < 32; ++i) {
        auto hv = [](char c) { return (c <= '9') ? c - '0' : c - 'a' + 10; };
        digest.bytes[i] = static_cast<std::uint8_t>((hv(lower[i * 2]) << 4) | hv(lower[i * 2 + 1]));
    }
    identity.sha256 = digest;

    const SignatureProfile* matched = nullptr;
    ProfileSelectionResult result = repo.SelectFor(identity, matched);
    SH_CHECK(result == ProfileSelectionResult::Success);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_FileSizeZero_Rejected) {
    ProfileFields fields;
    fields.fileSizeBytes = "0x0";
    std::filesystem::path path = WriteTempJson("filesize_zero.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_DuplicateProfileId_WholeFileRejected) {
    ProfileFields a;
    a.profileId = "same-id";
    ProfileFields b;
    b.profileId = "same-id";
    b.fileSizeBytes = "0x2000"; // different build identity, only the id collides
    std::filesystem::path path = WriteTempJson("dup_id.json", WrapProfiles({a.Render(), b.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
    SH_CHECK(repo.Size() == 0); // neither profile loaded, not even the first
}

SH_TEST(SignatureProfileRepository_LoadFromFile_DuplicateBuildIdentity_WholeFileRejected) {
    ProfileFields a;
    a.profileId = "profile-a";
    ProfileFields b;
    b.profileId = "profile-b"; // different id, same fileSizeBytes+sha256
    std::filesystem::path path = WriteTempJson("dup_build.json", WrapProfiles({a.Render(), b.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
    SH_CHECK(repo.Size() == 0);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_DuplicateAddressId_Rejected) {
    AddressFields addr1;
    addr1.addressId = "same-address";
    AddressFields addr2;
    addr2.addressId = "same-address";
    addr2.scanOffset = "0x500"; // different offset, only the id collides
    ProfileFields fields;
    fields.addressesJson = {addr1.Render(), addr2.Render()};
    std::filesystem::path path = WriteTempJson("dup_address.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_EmptyModuleName_Rejected) {
    AddressFields addr;
    addr.moduleName = "";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("empty_module.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_MalformedAobPattern_Rejected) {
    AddressFields addr;
    addr.pattern = "4? GG"; // partial wildcard + malformed hex
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("malformed_aob.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_ZeroScanSize_Rejected) {
    AddressFields addr;
    addr.scanSize = "0x0";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("zero_scan_size.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_ScanRangeOverflow_Rejected) {
    AddressFields addr;
    addr.scanOffset = "0xFFFFFFFFFFFFFFFF";
    addr.scanSize = "0x10";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("scan_overflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_UnknownResolutionKind_Rejected) {
    AddressFields addr;
    addr.resolutionKind = "totally_unknown_kind";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("unknown_kind.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_MissingResolutionField_Rejected) {
    // match_address entry hand-built without "matchOffset".
    std::string addrJson =
        "{\"addressId\":\"a\",\"moduleName\":\"fixture.exe\",\"scanOffset\":\"0x100\",\"scanSize\":\"0x100\","
        "\"pattern\":\"AB CD\",\"resolutionKind\":\"match_address\","
        "\"targetRangeOffset\":\"0x0\",\"targetRangeSize\":\"0x1000\"}";
    ProfileFields fields;
    fields.addressesJson = {addrJson};
    std::filesystem::path path = WriteTempJson("missing_resolution_field.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_InvalidTargetRange_Rejected) {
    AddressFields addr;
    addr.targetRangeSize = "0x0";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("invalid_target_range.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_RipDisplacementOutsideInstruction_Rejected) {
    AddressFields addr;
    addr.resolutionKind = "rip_relative_32";
    addr.instructionOffset = "0x0";
    addr.instructionLength = "0x6";
    addr.displacementOffset = "0x10"; // well outside [0x0, 0x6)
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("rip_bad_layout.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_FailedLoad_LeavesExistingStateUnchanged) {
    std::filesystem::path goodPath = WriteTempJson("good.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(goodPath.string()).ok);
    SH_CHECK(repo.Size() == 1);

    ProfileFields badFields;
    badFields.profileId = "another-profile";
    badFields.fileSizeBytes = "0x2000";
    badFields.schemaVersion = "999"; // unsupported -> whole file rejected
    std::filesystem::path badPath = WriteTempJson("bad.json", WrapProfiles({badFields.Render()}));

    SignatureProfileLoadOutcome secondOutcome = repo.LoadFromFile(badPath.string());
    SH_CHECK(secondOutcome.ok == false);
    SH_CHECK(repo.Size() == 1); // the first, successfully-loaded profile is untouched
}

// ===========================================================================
// Profile selection
// ===========================================================================

namespace {
ExecutableIdentity MakeIdentity(std::uint64_t fileSizeBytes, const std::string& shaHexLower,
                                 std::filesystem::path path = "C:\\anywhere\\fixture.exe",
                                 std::uintptr_t mainModuleBaseAddress = 0x400000) {
    ExecutableIdentity identity;
    identity.executablePath = std::move(path);
    identity.fileSizeBytes = fileSizeBytes;
    Sha256Digest digest;
    for (std::size_t i = 0; i < 32; ++i) {
        auto hv = [](char c) { return (c <= '9') ? c - '0' : c - 'a' + 10; };
        digest.bytes[i] = static_cast<std::uint8_t>((hv(shaHexLower[i * 2]) << 4) | hv(shaHexLower[i * 2 + 1]));
    }
    identity.sha256 = digest;
    identity.mainModuleBaseAddress = mainModuleBaseAddress;
    identity.mainModuleImageSize = 0x9000;
    return identity;
}
} // namespace

SH_TEST(SignatureProfileRepository_SelectFor_ExactSizeAndSha_Succeeds) {
    std::filesystem::path path = WriteTempJson("select_exact.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    ExecutableIdentity identity = MakeIdentity(0x1000, kValidSha256);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::Success);
    SH_CHECK(matched != nullptr);
    SH_CHECK(matched->profileId == "fixture-profile-1");
}

SH_TEST(SignatureProfileRepository_SelectFor_SizeOnlyMatches_ReturnsUnsupportedBuild) {
    std::filesystem::path path = WriteTempJson("select_size_only.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    std::string differentSha = std::string(63, '1') + "2";
    ExecutableIdentity identity = MakeIdentity(0x1000, differentSha);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::UnsupportedBuild);
}

SH_TEST(SignatureProfileRepository_SelectFor_ShaOnlyMatches_ReturnsUnsupportedBuild) {
    std::filesystem::path path = WriteTempJson("select_sha_only.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    ExecutableIdentity identity = MakeIdentity(0x9999, kValidSha256);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::UnsupportedBuild);
}

SH_TEST(SignatureProfileRepository_SelectFor_NeitherMatches_ReturnsUnsupportedBuild) {
    std::filesystem::path path = WriteTempJson("select_neither.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    std::string differentSha = std::string(63, '9') + "8";
    ExecutableIdentity identity = MakeIdentity(0x9999, differentSha);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::UnsupportedBuild);
}

SH_TEST(SignatureProfileRepository_SelectFor_AslrDifferentModuleBase_StillSelectsSameBuild) {
    std::filesystem::path path = WriteTempJson("select_aslr.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    ExecutableIdentity runA = MakeIdentity(0x1000, kValidSha256, "C:\\a\\fixture.exe", 0x400000);
    ExecutableIdentity runB = MakeIdentity(0x1000, kValidSha256, "C:\\a\\fixture.exe", 0x7FF612340000ULL);

    const SignatureProfile* matchedA = nullptr;
    const SignatureProfile* matchedB = nullptr;
    SH_CHECK(repo.SelectFor(runA, matchedA) == ProfileSelectionResult::Success);
    SH_CHECK(repo.SelectFor(runB, matchedB) == ProfileSelectionResult::Success);
    SH_CHECK(matchedA == matchedB);
}

SH_TEST(SignatureProfileRepository_SelectFor_DifferentPathSameBuild_StillSelects) {
    std::filesystem::path path = WriteTempJson("select_path.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);

    ExecutableIdentity identity = MakeIdentity(0x1000, kValidSha256, "D:\\completely\\different\\path.exe");
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::Success);
}

SH_TEST(SignatureProfileRepository_SelectFor_NoProfilesLoaded_ReturnsUnsupportedBuild) {
    SignatureProfileRepository repo;
    ExecutableIdentity identity = MakeIdentity(0x1000, kValidSha256);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::UnsupportedBuild);
}

SH_TEST(SignatureProfileRepository_SelectFor_UnknownBuild_NeverFallsBackToAnyLoadedProfile) {
    ProfileFields a;
    a.profileId = "profile-a";
    a.fileSizeBytes = "0x1000";
    ProfileFields b;
    b.profileId = "profile-b";
    b.fileSizeBytes = "0x2000";
    b.sha256 = std::string(63, '7') + "6";
    std::filesystem::path path = WriteTempJson("select_no_fallback.json", WrapProfiles({a.Render(), b.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(path.string()).ok);
    SH_CHECK(repo.Size() == 2);

    std::string unknownSha = std::string(63, '5') + "4";
    ExecutableIdentity identity = MakeIdentity(0x3333, unknownSha);
    const SignatureProfile* matched = nullptr;
    SH_CHECK(repo.SelectFor(identity, matched) == ProfileSelectionResult::UnsupportedBuild);
}

// ===========================================================================
// Additional validation TCs
// ===========================================================================

SH_TEST(SignatureProfileRepository_LoadFromFile_EmptyAddressesArray_Rejected) {
    ProfileFields fields;
    fields.addressesJson = {}; // empty
    std::filesystem::path path = WriteTempJson("empty_addresses.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_TargetRangeOverflow_Rejected) {
    AddressFields addr;
    addr.targetRangeOffset = "0xFFFFFFFFFFFFFFFF";
    addr.targetRangeSize = "0x10";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("target_overflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_RejectsDecimalInteger) {
    // fileSizeBytes as a plain decimal number string (missing 0x prefix)
    std::string json = "{\"profiles\":[{\"schemaVersion\":1,\"profileId\":\"p\","
                       "\"buildIdentity\":{\"fileSizeBytes\":\"4096\",\"sha256\":\"" +
                       std::string(kValidSha256) + "\"},\"addresses\":[" + AddressFields{}.Render() + "]}]}";
    std::filesystem::path path = WriteTempJson("hex_no_prefix.json", json);

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_RejectsNegativeValue) {
    AddressFields addr;
    addr.scanOffset = "-0x100"; // negative in an unsigned field
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("hex_negative.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_RejectsTrailingJunk) {
    AddressFields addr;
    addr.scanOffset = "0x100abc xyz"; // trailing non-hex characters
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("hex_trailing.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_RejectsWhitespace) {
    AddressFields addr;
    addr.scanOffset = "0x 100"; // space in hex string
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("hex_space.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_AcceptsLeadingZeros) {
    AddressFields addr;
    addr.scanOffset = "0x000100"; // leading zeros are fine
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("hex_leading_zeros.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_HexString_AcceptsBothCasePrefixAndDigits) {
    AddressFields addr;
    addr.scanOffset = "0X1aF"; // 0X prefix, mixed case digits
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("hex_mixed_case.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok);
}

// ===========================================================================
// Additional gap-fill TCs (SEK-READ-001D final checkpoint)
// ===========================================================================

SH_TEST(SignatureProfileRepository_LoadFromFile_EmptyProfileId_Rejected) {
    // profileId field exists but is empty string
    ProfileFields fields;
    fields.profileId = "";
    std::filesystem::path path = WriteTempJson("empty_profile_id.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_WhitespaceOnlyProfileId_Accepted) {
    // profileId is non-empty but contains only spaces — current implementation
    // only rejects truly empty "". Whitespace-only is accepted (valid JSON
    // string, and profileId uniqueness still enforces no collision).
    std::string json = "{\"profiles\":[{\"schemaVersion\":1,\"profileId\":\"   \","
                       "\"buildIdentity\":{\"fileSizeBytes\":\"0x1000\",\"sha256\":\"" +
                       std::string(kValidSha256) + "\"},\"addresses\":[" + AddressFields{}.Render() + "]}]}";
    std::filesystem::path path = WriteTempJson("ws_profile_id.json", json);

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    // Documents actual behaviour: whitespace-only profileId is accepted.
    SH_CHECK(outcome.ok == true);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_UnsignedHexOverflow_64bit_Rejected) {
    // 0x10000000000000000 = 2^64, exceeds uint64 range
    AddressFields addr;
    addr.scanOffset = "0x10000000000000000"; // 17 hex digits -> >u64
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("u64_overflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_FileSizeHexOverflow_Rejected) {
    ProfileFields fields;
    fields.fileSizeBytes = "0x10000000000000000"; // >u64
    std::filesystem::path path = WriteTempJson("filesize_overflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_SignedMatchOffsetOverflow_Rejected) {
    // matchOffset = 0x8000000000000000 (INT64_MAX + 1 as positive) must fail
    AddressFields addr;
    addr.matchOffset = "0x8000000000000000"; // > INT64_MAX
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("signed_overflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_SignedMatchOffsetUnderflow_Rejected) {
    // -0x8000000000000001 (below INT64_MIN) must fail
    AddressFields addr;
    addr.matchOffset = "-0x8000000000000001";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("signed_underflow.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == false);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_SignedMatchOffsetAtBoundary_Accepted) {
    // INT64_MAX = 0x7FFFFFFFFFFFFFFF must succeed
    AddressFields addr;
    addr.matchOffset = "0x7FFFFFFFFFFFFFFF";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("signed_max.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == true);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_SignedMatchOffsetNegativeAtBoundary_Accepted) {
    // -0x8000000000000000 = INT64_MIN must succeed
    AddressFields addr;
    addr.matchOffset = "-0x8000000000000000";
    ProfileFields fields;
    fields.addressesJson = {addr.Render()};
    std::filesystem::path path = WriteTempJson("signed_min.json", WrapProfiles({fields.Render()}));

    SignatureProfileRepository repo;
    SignatureProfileLoadOutcome outcome = repo.LoadFromFile(path.string());

    SH_CHECK(outcome.ok == true);
}

SH_TEST(SignatureProfileRepository_LoadFromFile_ParseFailure_LeavesExistingStateUnchanged_AfterMultipleFailures) {
    // Load one valid file, then attempt three different invalid files,
    // each must leave the repository untouched.
    std::filesystem::path goodPath = WriteTempJson("multi_fail_good.json", WrapProfiles({ProfileFields{}.Render()}));
    SignatureProfileRepository repo;
    SH_CHECK(repo.LoadFromFile(goodPath.string()).ok);
    SH_CHECK(repo.Size() == 1);

    // Failure 1: malformed JSON
    std::filesystem::path badJson = WriteTempJson("multi_fail_1.json", "not json at all");
    SH_CHECK(repo.LoadFromFile(badJson.string()).ok == false);
    SH_CHECK(repo.Size() == 1);

    // Failure 2: unsigned overflow in fileSizeBytes
    ProfileFields overflowProfile;
    overflowProfile.profileId = "overflow-p";
    overflowProfile.fileSizeBytes = "0x10000000000000000";
    std::filesystem::path badOverflow = WriteTempJson("multi_fail_2.json", WrapProfiles({overflowProfile.Render()}));
    SH_CHECK(repo.LoadFromFile(badOverflow.string()).ok == false);
    SH_CHECK(repo.Size() == 1);

    // Failure 3: signed matchOffset beyond range
    AddressFields badAddr;
    badAddr.matchOffset = "0x8000000000000000";
    ProfileFields badOffsetProfile;
    badOffsetProfile.profileId = "bad-offset-p";
    badOffsetProfile.fileSizeBytes = "0x3000";
    badOffsetProfile.sha256 = std::string(63, 'a') + "b";
    badOffsetProfile.addressesJson = {badAddr.Render()};
    std::filesystem::path badOffset = WriteTempJson("multi_fail_3.json", WrapProfiles({badOffsetProfile.Render()}));
    SH_CHECK(repo.LoadFromFile(badOffset.string()).ok == false);
    SH_CHECK(repo.Size() == 1);
}
