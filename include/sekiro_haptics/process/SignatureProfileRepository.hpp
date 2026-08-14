#pragma once

// Strict, fail-closed JSON loader and exact-build-identity selector for
// SignatureProfile. See docs/05-process-access.md for the full schema and
// validation policy. Part of SEK-READ-001D.

#include "sekiro_haptics/process/SignatureProfile.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Result of SignatureProfileRepository::LoadFromFile().
struct SignatureProfileLoadOutcome {
    /// False if the file could not be opened/parsed as JSON, or if ANY
    /// single profile in it failed validation -- this repository never
    /// loads "the good ones" from a file that also contains a bad one; the
    /// whole file is rejected as one unit. See LoadFromFile's doc comment.
    bool ok = false;
    /// Only meaningful when ok == true: how many profiles from this file
    /// were added to the repository.
    std::size_t loadedCount = 0;
    /// Only meaningful when ok == false: a human-readable description of
    /// the first validation problem found (which profile/field, and why).
    std::string fatalError;
};

/// Outcome of SignatureProfileRepository::SelectFor().
enum class ProfileSelectionResult {
    Success,
    /// No loaded profile's (fileSizeBytes, sha256) matched the given
    /// ExecutableIdentity exactly. Never a reason to fall back to a
    /// "closest" or "similar" profile -- there is no fallback.
    UnsupportedBuild,
    /// More than one loaded profile matched -- a repository-construction
    /// problem LoadFromFile's own duplicate-build-identity check should
    /// already prevent, but SelectFor() checks independently rather than
    /// trusting that invariant and picking one arbitrarily.
    AmbiguousProfile,
};

/// Loads, validates, and looks up SignatureProfile instances by exact
/// build identity (file size + SHA-256) -- never by path, filename, module
/// base, image size, or any partial/approximate match. See
/// docs/05-process-access.md.
class SignatureProfileRepository {
public:
    /// Parses `path` as a JSON file of the form `{"profiles": [...]}` and
    /// validates every profile entry (schema version, required fields,
    /// SHA-256 format, uniqueness of profileId/build-identity/addressId,
    /// AOB pattern validity, range/overflow checks -- see
    /// docs/05-process-access.md for the full list). If the file cannot be
    /// opened, is not valid JSON, or ANY single profile fails validation,
    /// this returns `ok == false` and the repository's existing state
    /// (from any prior successful LoadFromFile() call) is left completely
    /// untouched -- never partially overwritten by a failed load.
    ///
    /// On success, this file's profiles are added to the repository
    /// (additively -- a prior successful LoadFromFile() call's profiles
    /// remain, so multiple files can be loaded across calls). Uniqueness
    /// checks (profileId, build identity) apply across the whole
    /// repository, not just within this one file.
    SignatureProfileLoadOutcome LoadFromFile(const std::string& path);

    /// Selects the single loaded profile whose (fileSizeBytes, sha256)
    /// matches `identity` exactly. `outProfile` is only written on
    /// ProfileSelectionResult::Success -- left untouched otherwise. The
    /// returned pointer is valid as long as this repository is not
    /// destroyed or given another LoadFromFile() call.
    ProfileSelectionResult SelectFor(const ExecutableIdentity& identity, const SignatureProfile*& outProfile) const;

    std::size_t Size() const;

private:
    std::vector<SignatureProfile> profiles_;
};

} // namespace sekiro_haptics::process
