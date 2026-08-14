#include "sekiro_haptics/process/SignatureProfileRepository.hpp"

#include "sekiro_haptics/Json.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace sekiro_haptics::process {

namespace {

constexpr int kSupportedSignatureProfileSchemaVersion = 1;

bool ReadFileToString(const std::string& path, std::string& outContent) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    outContent = buffer.str();
    return true;
}

// Strict "0x"-prefixed hex-string parser -- rejects decimals, negatives,
// missing/invalid prefix, non-hex characters, and 64-bit overflow. JSON
// numbers (IEEE-754 double) are never used for offsets/sizes/file sizes
// in this schema: doubles lose precision above 2^53, which this module's
// module-relative offsets and file sizes must never silently do.
bool ParseHexU64(const std::string& text, std::uint64_t& outValue) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    for (std::size_t i = 2; i < text.size(); ++i) {
        char c = text[i];
        bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!isHex) {
            return false;
        }
    }
    try {
        std::size_t consumed = 0;
        unsigned long long value = std::stoull(text.substr(2), &consumed, 16);
        if (consumed != text.size() - 2) {
            return false;
        }
        outValue = static_cast<std::uint64_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

// Same grammar as ParseHexU64, with an optional leading '-' -- used only
// for AddressSpec::matchOffset, the one genuinely signed field in this
// schema.
bool ParseSignedHexI64(const std::string& text, std::int64_t& outValue) {
    bool negative = false;
    std::string rest = text;
    if (!rest.empty() && rest[0] == '-') {
        negative = true;
        rest = rest.substr(1);
    }
    std::uint64_t magnitude = 0;
    if (!ParseHexU64(rest, magnitude)) {
        return false;
    }
    constexpr std::uint64_t kInt64MaxAsU64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
        if (magnitude > kInt64MaxAsU64 + 1ULL) {
            return false;
        }
        outValue = (magnitude == kInt64MaxAsU64 + 1ULL) ? std::numeric_limits<std::int64_t>::min()
                                                          : -static_cast<std::int64_t>(magnitude);
    } else {
        if (magnitude > kInt64MaxAsU64) {
            return false;
        }
        outValue = static_cast<std::int64_t>(magnitude);
    }
    return true;
}

// Validates `text` is exactly 64 hex characters and writes its canonical
// lowercase form to `outLower`.
bool NormalizeSha256Hex(const std::string& text, std::string& outLower) {
    if (text.size() != 64) {
        return false;
    }
    std::string lower;
    lower.reserve(64);
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            lower.push_back(c);
        } else if (c >= 'a' && c <= 'f') {
            lower.push_back(c);
        } else if (c >= 'A' && c <= 'F') {
            lower.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            return false;
        }
    }
    outLower = std::move(lower);
    return true;
}

int HexNibble(char c) {
    return (c <= '9') ? (c - '0') : (c - 'a' + 10);
}

Sha256Digest DigestFromLowerHex(const std::string& lowerHex) {
    Sha256Digest digest;
    for (std::size_t i = 0; i < 32; ++i) {
        digest.bytes[i] =
            static_cast<std::uint8_t>((HexNibble(lowerHex[i * 2]) << 4) | HexNibble(lowerHex[i * 2 + 1]));
    }
    return digest;
}

bool ContainsString(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

SignatureProfileLoadOutcome SignatureProfileRepository::LoadFromFile(const std::string& path) {
    SignatureProfileLoadOutcome outcome;

    std::string content;
    if (!ReadFileToString(path, content)) {
        outcome.fatalError = "could not open file: " + path;
        return outcome;
    }

    json::JsonParseResult parsed = json::ParseJson(content);
    if (!parsed.ok) {
        outcome.fatalError = "JSON parse error: " + parsed.error;
        return outcome;
    }
    if (!parsed.value.IsObject()) {
        outcome.fatalError = "top-level JSON value must be an object";
        return outcome;
    }

    const json::JsonValue* profilesArray = parsed.value.Find("profiles");
    if (profilesArray == nullptr || !profilesArray->IsArray()) {
        outcome.fatalError = "missing \"profiles\" array";
        return outcome;
    }

    std::vector<SignatureProfile> parsedProfiles;
    std::vector<std::string> seenProfileIds;
    std::vector<std::pair<std::uint64_t, Sha256Digest>> seenBuildIdentities;

    const std::vector<json::JsonValue>& entries = profilesArray->AsArray();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string errorPrefix = "profiles[" + std::to_string(i) + "]: ";
        const json::JsonValue& entry = entries[i];
        if (!entry.IsObject()) {
            outcome.fatalError = errorPrefix + "entry is not a JSON object";
            return outcome;
        }

        const json::JsonValue* schemaVersionValue = entry.Find("schemaVersion");
        if (schemaVersionValue == nullptr || !schemaVersionValue->IsNumber()) {
            outcome.fatalError = errorPrefix + "missing or non-numeric \"schemaVersion\"";
            return outcome;
        }
        double schemaVersionRaw = schemaVersionValue->AsNumber();
        int schemaVersion = static_cast<int>(schemaVersionRaw);
        if (static_cast<double>(schemaVersion) != schemaVersionRaw ||
            schemaVersion != kSupportedSignatureProfileSchemaVersion) {
            outcome.fatalError = errorPrefix + "unsupported schemaVersion";
            return outcome;
        }

        const json::JsonValue* profileIdValue = entry.Find("profileId");
        if (profileIdValue == nullptr || !profileIdValue->IsString() || profileIdValue->AsString().empty()) {
            outcome.fatalError = errorPrefix + "missing or empty \"profileId\"";
            return outcome;
        }
        std::string profileId = profileIdValue->AsString();
        bool profileIdAlreadyLoaded =
            std::any_of(profiles_.begin(), profiles_.end(), [&](const SignatureProfile& p) { return p.profileId == profileId; });
        if (ContainsString(seenProfileIds, profileId) || profileIdAlreadyLoaded) {
            outcome.fatalError = errorPrefix + "duplicate profileId: " + profileId;
            return outcome;
        }

        const json::JsonValue* buildIdentityValue = entry.Find("buildIdentity");
        if (buildIdentityValue == nullptr || !buildIdentityValue->IsObject()) {
            outcome.fatalError = errorPrefix + "missing \"buildIdentity\" object";
            return outcome;
        }

        const json::JsonValue* fileSizeValue = buildIdentityValue->Find("fileSizeBytes");
        if (fileSizeValue == nullptr || !fileSizeValue->IsString()) {
            outcome.fatalError = errorPrefix + "missing \"buildIdentity.fileSizeBytes\" (hex string)";
            return outcome;
        }
        std::uint64_t fileSizeBytes = 0;
        if (!ParseHexU64(fileSizeValue->AsString(), fileSizeBytes)) {
            outcome.fatalError = errorPrefix + "invalid \"buildIdentity.fileSizeBytes\" hex value";
            return outcome;
        }
        if (fileSizeBytes == 0) {
            outcome.fatalError = errorPrefix + "\"buildIdentity.fileSizeBytes\" must not be 0";
            return outcome;
        }

        const json::JsonValue* shaValue = buildIdentityValue->Find("sha256");
        if (shaValue == nullptr || !shaValue->IsString()) {
            outcome.fatalError = errorPrefix + "missing \"buildIdentity.sha256\"";
            return outcome;
        }
        std::string lowerSha;
        if (!NormalizeSha256Hex(shaValue->AsString(), lowerSha)) {
            outcome.fatalError = errorPrefix + "\"buildIdentity.sha256\" must be exactly 64 hex characters";
            return outcome;
        }
        Sha256Digest sha256 = DigestFromLowerHex(lowerSha);

        bool duplicateBuildIdentity =
            std::any_of(seenBuildIdentities.begin(), seenBuildIdentities.end(),
                        [&](const auto& seen) { return seen.first == fileSizeBytes && seen.second == sha256; }) ||
            std::any_of(profiles_.begin(), profiles_.end(), [&](const SignatureProfile& p) {
                return p.fileSizeBytes == fileSizeBytes && p.sha256 == sha256;
            });
        if (duplicateBuildIdentity) {
            outcome.fatalError = errorPrefix + "duplicate build identity (fileSizeBytes + sha256)";
            return outcome;
        }

        const json::JsonValue* addressesValue = entry.Find("addresses");
        if (addressesValue == nullptr || !addressesValue->IsArray() || addressesValue->AsArray().empty()) {
            outcome.fatalError = errorPrefix + "missing or empty \"addresses\" array";
            return outcome;
        }

        std::vector<AddressSpec> addressSpecs;
        std::vector<std::string> seenAddressIds;
        const std::vector<json::JsonValue>& addressEntries = addressesValue->AsArray();
        for (std::size_t j = 0; j < addressEntries.size(); ++j) {
            const std::string addrErrorPrefix = errorPrefix + "addresses[" + std::to_string(j) + "]: ";
            const json::JsonValue& addrEntry = addressEntries[j];
            if (!addrEntry.IsObject()) {
                outcome.fatalError = addrErrorPrefix + "entry is not a JSON object";
                return outcome;
            }

            const json::JsonValue* addressIdValue = addrEntry.Find("addressId");
            if (addressIdValue == nullptr || !addressIdValue->IsString() || addressIdValue->AsString().empty()) {
                outcome.fatalError = addrErrorPrefix + "missing or empty \"addressId\"";
                return outcome;
            }
            std::string addressId = addressIdValue->AsString();
            if (ContainsString(seenAddressIds, addressId)) {
                outcome.fatalError = addrErrorPrefix + "duplicate addressId: " + addressId;
                return outcome;
            }

            const json::JsonValue* moduleNameValue = addrEntry.Find("moduleName");
            if (moduleNameValue == nullptr || !moduleNameValue->IsString() || moduleNameValue->AsString().empty()) {
                outcome.fatalError = addrErrorPrefix + "missing or empty \"moduleName\"";
                return outcome;
            }

            const json::JsonValue* scanOffsetValue = addrEntry.Find("scanOffset");
            const json::JsonValue* scanSizeValue = addrEntry.Find("scanSize");
            if (scanOffsetValue == nullptr || !scanOffsetValue->IsString() || scanSizeValue == nullptr ||
                !scanSizeValue->IsString()) {
                outcome.fatalError = addrErrorPrefix + "missing \"scanOffset\"/\"scanSize\" (hex strings)";
                return outcome;
            }
            std::uint64_t scanOffset = 0;
            std::uint64_t scanSize = 0;
            if (!ParseHexU64(scanOffsetValue->AsString(), scanOffset) ||
                !ParseHexU64(scanSizeValue->AsString(), scanSize)) {
                outcome.fatalError = addrErrorPrefix + "invalid \"scanOffset\"/\"scanSize\" hex value";
                return outcome;
            }
            if (scanSize == 0) {
                outcome.fatalError = addrErrorPrefix + "\"scanSize\" must not be 0";
                return outcome;
            }
            if (scanOffset + scanSize < scanOffset) {
                outcome.fatalError = addrErrorPrefix + "\"scanOffset\" + \"scanSize\" overflows";
                return outcome;
            }

            const json::JsonValue* patternValue = addrEntry.Find("pattern");
            if (patternValue == nullptr || !patternValue->IsString()) {
                outcome.fatalError = addrErrorPrefix + "missing \"pattern\"";
                return outcome;
            }
            AobPattern pattern;
            if (ParseAobPattern(patternValue->AsString(), pattern) != AobScanResult::Success) {
                outcome.fatalError = addrErrorPrefix + "invalid AOB \"pattern\"";
                return outcome;
            }

            const json::JsonValue* kindValue = addrEntry.Find("resolutionKind");
            if (kindValue == nullptr || !kindValue->IsString()) {
                outcome.fatalError = addrErrorPrefix + "missing \"resolutionKind\"";
                return outcome;
            }
            std::string kindText = kindValue->AsString();

            AddressSpec spec;
            spec.addressId = addressId;
            spec.moduleName = moduleNameValue->AsString();
            spec.scanOffset = static_cast<std::size_t>(scanOffset);
            spec.scanSize = static_cast<std::size_t>(scanSize);
            spec.pattern = std::move(pattern);

            if (kindText == "match_address") {
                spec.kind = AddressResolutionKind::MatchAddress;
                const json::JsonValue* offsetValue = addrEntry.Find("matchOffset");
                if (offsetValue == nullptr || !offsetValue->IsString()) {
                    outcome.fatalError = addrErrorPrefix + "missing \"matchOffset\" (signed hex string)";
                    return outcome;
                }
                std::int64_t matchOffset = 0;
                if (!ParseSignedHexI64(offsetValue->AsString(), matchOffset)) {
                    outcome.fatalError = addrErrorPrefix + "invalid \"matchOffset\" signed hex value";
                    return outcome;
                }
                spec.matchOffset = matchOffset;
            } else if (kindText == "rip_relative_32") {
                spec.kind = AddressResolutionKind::RipRelative32;
                const json::JsonValue* instrOffsetValue = addrEntry.Find("instructionOffset");
                const json::JsonValue* dispOffsetValue = addrEntry.Find("displacementOffset");
                const json::JsonValue* instrLenValue = addrEntry.Find("instructionLength");
                if (instrOffsetValue == nullptr || !instrOffsetValue->IsString() || dispOffsetValue == nullptr ||
                    !dispOffsetValue->IsString() || instrLenValue == nullptr || !instrLenValue->IsString()) {
                    outcome.fatalError = addrErrorPrefix +
                                          "missing \"instructionOffset\"/\"displacementOffset\"/\"instructionLength\"";
                    return outcome;
                }
                std::uint64_t instrOffset = 0;
                std::uint64_t dispOffset = 0;
                std::uint64_t instrLen = 0;
                if (!ParseHexU64(instrOffsetValue->AsString(), instrOffset) ||
                    !ParseHexU64(dispOffsetValue->AsString(), dispOffset) ||
                    !ParseHexU64(instrLenValue->AsString(), instrLen)) {
                    outcome.fatalError = addrErrorPrefix + "invalid instruction/displacement hex value";
                    return outcome;
                }
                if (dispOffset < instrOffset || (dispOffset - instrOffset) + 4 > instrLen) {
                    outcome.fatalError = addrErrorPrefix + "displacement does not lie within the instruction";
                    return outcome;
                }
                spec.instructionOffset = static_cast<std::size_t>(instrOffset);
                spec.displacementOffset = static_cast<std::size_t>(dispOffset);
                spec.instructionLength = static_cast<std::size_t>(instrLen);
            } else {
                outcome.fatalError = addrErrorPrefix + "unknown \"resolutionKind\": " + kindText;
                return outcome;
            }

            const json::JsonValue* targetOffsetValue = addrEntry.Find("targetRangeOffset");
            const json::JsonValue* targetSizeValue = addrEntry.Find("targetRangeSize");
            if (targetOffsetValue == nullptr || !targetOffsetValue->IsString() || targetSizeValue == nullptr ||
                !targetSizeValue->IsString()) {
                outcome.fatalError = addrErrorPrefix + "missing \"targetRangeOffset\"/\"targetRangeSize\" (hex strings)";
                return outcome;
            }
            std::uint64_t targetOffset = 0;
            std::uint64_t targetSize = 0;
            if (!ParseHexU64(targetOffsetValue->AsString(), targetOffset) ||
                !ParseHexU64(targetSizeValue->AsString(), targetSize)) {
                outcome.fatalError = addrErrorPrefix + "invalid \"targetRangeOffset\"/\"targetRangeSize\" hex value";
                return outcome;
            }
            if (targetSize == 0) {
                outcome.fatalError = addrErrorPrefix + "\"targetRangeSize\" must not be 0";
                return outcome;
            }
            if (targetOffset + targetSize < targetOffset) {
                outcome.fatalError = addrErrorPrefix + "\"targetRangeOffset\" + \"targetRangeSize\" overflows";
                return outcome;
            }
            spec.targetRangeOffset = static_cast<std::size_t>(targetOffset);
            spec.targetRangeSize = static_cast<std::size_t>(targetSize);

            seenAddressIds.push_back(addressId);
            addressSpecs.push_back(std::move(spec));
        }

        SignatureProfile profile;
        profile.schemaVersion = schemaVersion;
        profile.profileId = profileId;
        profile.fileSizeBytes = fileSizeBytes;
        profile.sha256 = sha256;
        profile.addresses = std::move(addressSpecs);

        seenProfileIds.push_back(profileId);
        seenBuildIdentities.push_back({fileSizeBytes, sha256});
        parsedProfiles.push_back(std::move(profile));
    }

    // Every profile in this file validated successfully -- commit
    // atomically. A prior successful LoadFromFile() call's profiles (in
    // profiles_ already) are untouched; this is a pure append.
    outcome.loadedCount = parsedProfiles.size();
    for (SignatureProfile& profile : parsedProfiles) {
        profiles_.push_back(std::move(profile));
    }
    outcome.ok = true;
    return outcome;
}

ProfileSelectionResult SignatureProfileRepository::SelectFor(const ExecutableIdentity& identity,
                                                               const SignatureProfile*& outProfile) const {
    const SignatureProfile* matched = nullptr;
    std::size_t matchCount = 0;
    for (const SignatureProfile& profile : profiles_) {
        if (profile.fileSizeBytes == identity.fileSizeBytes && profile.sha256 == identity.sha256) {
            matched = &profile;
            ++matchCount;
        }
    }

    if (matchCount == 0) {
        return ProfileSelectionResult::UnsupportedBuild;
    }
    if (matchCount > 1) {
        return ProfileSelectionResult::AmbiguousProfile;
    }
    outProfile = matched;
    return ProfileSelectionResult::Success;
}

std::size_t SignatureProfileRepository::Size() const {
    return profiles_.size();
}

} // namespace sekiro_haptics::process
