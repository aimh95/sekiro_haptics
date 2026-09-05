#pragma once

// SEK-PROBE-001D Stage A: resolves a small number of known, AOB-anchored
// "root" singleton pointers (e.g. GameDataMan) to their live object address,
// and lets a caller follow one further pointer-sized dereference from an
// already-resolved root to reach a dependent object (e.g. PlayerGameData =
// *(GameDataMan + 0x8)). Built entirely on top of the existing AobScanner.hpp
// (unique-match scanning, unchanged) and RipRelative.hpp (disp32 arithmetic,
// unchanged) -- this module adds only the parts those two don't cover:
// dereferencing the RIP-relative target once to reach a live object,
// identity-gated scanning, and generation tracking so a caller can tell
// "the same root instance as last time" from "a different instance" without
// re-scanning every sample. See docs/07-combat-signal-reader.md.
//
// Deliberately conservative about what counts as evidence: every AOB
// pattern/offset consumed here is a *hypothesis* until cross-checked against
// a real attached process (see AddressSpec's module-relative philosophy) --
// nothing in this file invents or falls back to a hardcoded RVA for an
// unrecognized build. See RootResolveResult::UnsupportedBuild.

#include "sekiro_haptics/process/AobPattern.hpp"
#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"

#include <cstdint>
#include <string>

namespace sekiro_haptics::process {

/// Outcome of one root/child-pointer resolve attempt. Mirrors this
/// project's existing typed-result convention (ProcessReaderResult,
/// AobScanResult, ...) -- every failure is distinct and none of them ever
/// leaves a stale address usable.
enum class RootResolveResult {
    Resolved,
    /// The attached process's (fileSizeBytes, sha256) identity doesn't
    /// match the build this KnownRootSpec's AOB pattern/offsets were
    /// validated against -- no AOB scan (or any other process read) is
    /// attempted in this case. Checked once per Resolve() call, cheaply,
    /// before anything else.
    UnsupportedBuild,
    NotAttached,
    ProcessExited,
    /// KnownRootSpec::moduleName didn't resolve to exactly one loaded module.
    ModuleNotFound,
    /// The AOB pattern matched nowhere in the module.
    SignatureNotFound,
    /// The AOB pattern matched more than once in the module.
    AmbiguousSignature,
    /// The RIP-relative displacement calculation itself failed (invalid
    /// layout, address-space overflow, or the computed pointer-slot address
    /// fell outside the module's own bounds).
    AddressCalculationFailed,
    /// A process memory read (the pointer-slot dereference, or a child
    /// dereference) failed or returned fewer bytes than requested.
    ReadFailed,
    /// A dereferenced pointer-sized value was exactly zero -- never treated
    /// as a valid object address.
    NullPointer,
};
const char* ToString(RootResolveResult result);

/// One AOB-anchored root to resolve: `pattern` must match exactly once in
/// `moduleName`, at which point the RIP-relative displacement (per
/// `instructionOffset`/`displacementOffset`/`instructionLength`, same
/// convention as RipRelativeSpec) is computed to find a *pointer slot*
/// address -- a static global variable holding a pointer to the live
/// object. That slot is then dereferenced once (an 8-byte pointer read) to
/// reach the live object's own address, which is what Resolve() reports.
struct KnownRootSpec {
    /// Human-readable identifier for logging/diagnostics only (e.g.
    /// "GameDataMan") -- never used to select behavior.
    std::string rootId;
    std::string moduleName;
    AobPattern pattern;
    std::size_t instructionOffset = 0;
    std::size_t displacementOffset = 0;
    std::size_t instructionLength = 0;
};

/// One resolve attempt's outcome. `objectAddress` is only meaningful when
/// `result == Resolved` -- every other result leaves it 0, so a caller that
/// forgets to check `result` gets a guaranteed-invalid address rather than a
/// stale one. `generation` increments whenever a *successful* resolve
/// reports a different `objectAddress` than the previous successful
/// resolve (the first successful resolve is generation 1); a failed resolve
/// never changes `generation`. Two snapshots taken with different
/// `generation` values must never have their fields diffed against each
/// other (see docs/07-combat-signal-reader.md).
struct ResolvedRoot {
    RootResolveResult result = RootResolveResult::SignatureNotFound;
    std::uintptr_t objectAddress = 0;
    std::uint64_t generation = 0;
};

/// Resolves and tracks one AOB-anchored root. Re-scans (AOB + RIP-relative +
/// one pointer dereference) only when Resolve() is explicitly called --
/// never automatically, and never once per sample; a caller that wants a
/// live combat-signal loop is expected to call Resolve() only at session
/// start and after a detected failure/reattach, then reuse the resulting
/// `objectAddress` for many subsequent field reads.
class SekiroKnownRootResolver {
public:
    /// `expectedIdentity` is the exact build (fileSizeBytes + sha256 only --
    /// path and runtime module placement are ignored, matching
    /// SignatureProfileRepository::SelectFor()'s convention) this spec's AOB
    /// pattern/offsets were validated against. `currentIdentity` is the
    /// attached process's own identity, computed once by the caller (see
    /// ExecutableIdentity.hpp -- computing it is Win32-only and stays out of
    /// this OS-independent module). If the two don't match on those two
    /// fields, every Resolve() call fails immediately with UnsupportedBuild
    /// and never scans.
    SekiroKnownRootResolver(IProcessReader& reader, IProcessInspector& inspector, KnownRootSpec spec,
                             ExecutableIdentity expectedIdentity, ExecutableIdentity currentIdentity);

    /// Performs one full resolve attempt (identity check -> module lookup ->
    /// unique AOB scan -> RIP-relative -> one pointer dereference), updating
    /// and returning the tracked state. Safe to call repeatedly; each call
    /// is an independent, fresh scan -- nothing here is cached across calls
    /// except the address used for generation comparison.
    ResolvedRoot Resolve();

    /// The outcome of the most recent Resolve() call, without touching the
    /// process. Default-constructed (SignatureNotFound, address 0,
    /// generation 0) if Resolve() has never been called.
    ResolvedRoot Current() const { return current_; }

private:
    IProcessReader& reader_;
    IProcessInspector& inspector_;
    KnownRootSpec spec_;
    bool identitySupported_;

    ResolvedRoot current_;
    /// The last *successfully* resolved address, kept separately from
    /// `current_.objectAddress` (which is zeroed on any failure) so a later
    /// successful resolve can still tell "same instance as before" from "a
    /// different instance" even after an intervening failure.
    std::uintptr_t lastKnownAddress_ = 0;
    std::uint64_t nextGeneration_ = 1;
};

/// Tracks one pointer-sized dereference from an already-resolved parent
/// address (e.g. PlayerGameData = *(GameDataMan + 0x8)) with the same
/// typed-failure/generation discipline as SekiroKnownRootResolver, but
/// without any AOB scan of its own -- the parent's live address is supplied
/// by the caller on every Resolve() call, so this class never has an
/// opinion about how the parent itself was obtained. Kept as a separate
/// small class (rather than folding into SekiroKnownRootResolver) so a
/// child's generation tracks its *own* address changes independently of its
/// parent's.
class SekiroChildPointerResolver {
public:
    /// `byteOffsetFromParent` may be negative; overflow/underflow of the
    /// address arithmetic is checked in Resolve(), never wrapped silently.
    SekiroChildPointerResolver(IProcessReader& reader, std::int64_t byteOffsetFromParent);

    /// Reads one pointer-sized value at `parentAddress + byteOffsetFromParent`
    /// and reports it as this child's live object address. `parentAddress`
    /// must be a value the caller already knows came from a Resolved result
    /// (a zero `parentAddress` is rejected immediately as NullPointer,
    /// without attempting any read) -- this class does not re-validate
    /// whatever produced it.
    ResolvedRoot Resolve(std::uintptr_t parentAddress);

    /// The outcome of the most recent Resolve() call, without touching the
    /// process.
    ResolvedRoot Current() const { return current_; }

private:
    IProcessReader& reader_;
    std::int64_t byteOffsetFromParent_;

    ResolvedRoot current_;
    std::uintptr_t lastKnownAddress_ = 0;
    std::uint64_t nextGeneration_ = 1;
};

} // namespace sekiro_haptics::process
