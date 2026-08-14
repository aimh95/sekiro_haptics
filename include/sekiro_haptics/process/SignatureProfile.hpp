#pragma once

// Pure data types for a versioned signature profile: which build
// (ExecutableIdentity) it applies to, and the module-relative address
// specifications it defines. Parsing/validation lives in
// SignatureProfileRepository.hpp; resolving these against a live process
// lives in AddressResolver.hpp. Part of SEK-READ-001D -- see
// docs/05-process-access.md.

#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// How a single AddressSpec's unique AOB match is turned into a resolved
/// address.
enum class AddressResolutionKind {
    /// The match address itself, plus a signed module-relative offset.
    MatchAddress,
    /// A signed 32-bit RIP-relative displacement read from the match
    /// (see RipRelative.hpp).
    RipRelative32,
};

/// One address to resolve within a selected SignatureProfile. Every
/// offset/size here is **module-relative** -- never a runtime absolute
/// address -- so the same AddressSpec is valid across different attaches
/// to the same build even though ASLR changes the module's actual base
/// every run. Combined with a live module's base address at resolve time
/// (see AddressResolver.hpp) to produce runtime ranges/addresses.
struct AddressSpec {
    /// Unique (within its owning SignatureProfile) identifier for this address.
    std::string addressId;
    /// Exact (not substring), case-insensitive module basename -- passed
    /// to IProcessInspector::FindModuleExact() unchanged.
    std::string moduleName;

    /// Module-relative [scanOffset, scanOffset + scanSize) range to search
    /// for `pattern` in. Must end up fully inside the resolved module's
    /// own bounds at resolve time -- never scans outside it.
    std::size_t scanOffset = 0;
    std::size_t scanSize = 0;
    AobPattern pattern;

    AddressResolutionKind kind = AddressResolutionKind::MatchAddress;

    /// AddressResolutionKind::MatchAddress only: a signed offset applied
    /// to the unique match address.
    std::int64_t matchOffset = 0;

    /// AddressResolutionKind::RipRelative32 only: offsets relative to the
    /// match address, matching RipRelativeSpec's convention exactly (see
    /// RipRelative.hpp).
    std::size_t instructionOffset = 0;
    std::size_t displacementOffset = 0;
    std::size_t instructionLength = 0;

    /// Module-relative [targetRangeOffset, targetRangeOffset +
    /// targetRangeSize) range the *final resolved address* must fall
    /// within, regardless of `kind` -- never widened or guessed at.
    std::size_t targetRangeOffset = 0;
    std::size_t targetRangeSize = 0;
};

/// A versioned collection of AddressSpecs tied to one exact build,
/// identified the same way ExecutableIdentity is: file size + SHA-256.
/// Never a runtime address anywhere in this struct.
struct SignatureProfile {
    int schemaVersion = 0;
    std::string profileId;
    std::uint64_t fileSizeBytes = 0;
    Sha256Digest sha256;
    std::vector<AddressSpec> addresses;
};

} // namespace sekiro_haptics::process
