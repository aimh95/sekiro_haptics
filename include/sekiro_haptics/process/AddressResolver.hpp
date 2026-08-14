#pragma once

// Orchestrates resolving every AddressSpec in a selected SignatureProfile
// against a live attached process: exact module lookup, module-relative
// scan range validation, unique AOB scan (reusing AobScanner.hpp
// unchanged), and single-step address calculation (reusing
// RipRelative.hpp unchanged) -- never a new pattern-matching or
// RIP-relative implementation. No pointer dereferencing happens here; see
// docs/05-process-access.md for what SEK-READ-001D does and does not
// cover.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SignatureProfile.hpp"
#include "sekiro_haptics/process/SignatureProfileRepository.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// Coarse outcome of resolving one AddressSpec -- the "minimum states"
/// this module reports. Every failure reason (AddressResolutionReason)
/// implies Disabled; Resolved is the only state a caller may read
/// `ResolvedAddress::address` from.
enum class AddressResolutionState {
    Resolved,
    Disabled,
};

/// Why a single address ended up Disabled. `None` is only valid alongside
/// AddressResolutionState::Resolved.
enum class AddressResolutionReason {
    None,
    /// No profile matched the process's ExecutableIdentity at all -- every
    /// address in the (non-existent) resolution attempt is Disabled with
    /// this reason; no per-address resolution is attempted.
    UnsupportedBuild,
    /// The profile selection itself was invalid (e.g. more than one loaded
    /// profile ambiguously matched the same build identity) -- a
    /// repository-construction problem, not "this build isn't supported."
    ProfileInvalid,
    /// AddressSpec::moduleName didn't resolve to exactly one loaded module.
    ModuleNotFound,
    /// The module-relative scan or target range didn't fit within the
    /// resolved module's actual bounds, or overflowed.
    InvalidScanRange,
    /// The AOB pattern matched nowhere in the scan range.
    PatternNotFound,
    /// The AOB pattern matched more than once in the scan range.
    PatternAmbiguous,
    /// A process memory read failed (not attached, process exited, a hard
    /// read failure, or a partial read) at any step.
    ProcessReadFailed,
    /// The resolution-kind-specific address arithmetic itself failed
    /// (e.g. a RIP-relative displacement outside its declared instruction,
    /// or an address-space overflow/underflow).
    AddressCalculationFailed,
    /// The final computed address fell outside AddressSpec's declared
    /// module-relative target range (or was exactly 0).
    TargetOutOfRange,
};

/// The outcome of resolving a single AddressSpec.
struct ResolvedAddress {
    std::string addressId;
    AddressResolutionState state = AddressResolutionState::Disabled;
    AddressResolutionReason reason = AddressResolutionReason::None;
    /// Only meaningful when state == Resolved.
    std::uintptr_t address = 0;
    /// The lower-level typed result (ToString() of a ProcessInspectionResult,
    /// ProcessReaderResult, or AobScanResult) that produced `reason`, kept
    /// for diagnostics without collapsing its more specific meaning into
    /// this module's coarser reason -- empty when `reason` doesn't
    /// originate from a lower layer (e.g. UnsupportedBuild, ProfileInvalid,
    /// InvalidScanRange computed purely from the profile's own numbers).
    std::string underlyingResult;
};

/// Result of resolving every AddressSpec in whichever profile matches
/// `identity` (see ResolveAddressesForIdentity()).
struct ProfileResolutionOutcome {
    /// False if profile selection itself failed (UnsupportedBuild or
    /// ProfileInvalid) -- `addresses` is empty and no scan/read of any kind
    /// was attempted in that case. True means a profile was selected and
    /// every one of its AddressSpecs was independently attempted.
    bool buildSupported = false;
    /// Only meaningful when !buildSupported: UnsupportedBuild or
    /// ProfileInvalid.
    AddressResolutionReason buildFailureReason = AddressResolutionReason::None;
    /// One entry per AddressSpec in the selected profile, in the same
    /// order, only populated when buildSupported == true.
    std::vector<ResolvedAddress> addresses;
};

/// Selects a profile from `repository` matching `identity` exactly (see
/// SignatureProfileRepository::SelectFor()), then resolves every one of
/// its AddressSpecs against the process attached behind `inspector`/
/// `reader` (both must refer to the same already-attached process).
///
/// If profile selection fails, this returns immediately with
/// `buildSupported == false` and an empty `addresses` -- no module lookup,
/// scan, or process read of any kind is attempted for any address.
///
/// If a profile is selected, each AddressSpec is resolved independently:
/// module lookup -> module-relative scan/target range validated against
/// the module's actual runtime bounds -> unique AOB scan
/// (ScanProcessRange(), unchanged) -> resolution-kind-specific address
/// calculation (a signed offset applied to the match address, or
/// ResolveRipRelativeFromProcess(), unchanged) -> final target-range
/// check. One address's failure never affects another's -- a Disabled
/// address never carries over a stale address from a prior resolution
/// attempt, and a Resolved address is never a guess (first match, nearest
/// module, or similar heuristic). No pointer is ever dereferenced past the
/// single resolved address.
ProfileResolutionOutcome ResolveAddressesForIdentity(const SignatureProfileRepository& repository,
                                                       const ExecutableIdentity& identity, IProcessInspector& inspector,
                                                       IProcessReader& reader);

} // namespace sekiro_haptics::process
