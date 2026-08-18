#pragma once

// scan-manifest.json: the on-disk record of one disk-backed candidate scan
// session's identity, progress, and state (SEK-PROBE-001C). Written by
// hand (matching DiscoverySessionMetadata's existing precedent -- hex
// addresses, JSON numbers safe to 2^53), read via this project's own
// sekiro_haptics::json parser (matching PresetRepository's existing
// precedent). This is a separate file/module from DiscoverySession's
// session.json -- neither reads nor writes the other; they simply live
// side by side in the same captures/local/<session>/ directory.

#include "sekiro_haptics/process/CandidateScanner.hpp"
#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

inline constexpr int kScanManifestSchemaVersion = 1;

/// How far a disk-backed scan session has progressed. `BaselineComplete`
/// may only be set once the FULL coverage contract is met (see
/// DiskCandidateScanner.hpp) -- never on a partial/capped scan.
enum class ScanManifestState {
    Planning,
    WritingBaseline,
    BaselineComplete,
    Filtering,
    CandidatesComplete,
    Interrupted,
    Failed,
};
const char* ToString(ScanManifestState state);
/// Parses a value previously produced by ToString(ScanManifestState).
/// Returns false (leaving `outState` untouched) for anything else --
/// callers must not guess at an unrecognized state.
bool ParseScanManifestState(const std::string& text, ScanManifestState& outState);

/// One region's directory entry as recorded in the manifest -- mirrors
/// (a subset of) ProcessMemoryRegion. No separate "protection" field:
/// ProcessMemoryRegion itself doesn't expose one (see its doc comment),
/// and this manifest intentionally carries exactly what that struct
/// already exposes rather than inventing a new one.
struct ScanManifestRegion {
    std::uintptr_t baseAddress = 0;
    std::size_t sizeBytes = 0;
    MemoryRegionKind kind = MemoryRegionKind::Private;
};

/// The stable identity a resumed session must match exactly (see
/// DiskCandidateScanner.hpp's ResumeDiskCandidateSession()). Duplicated
/// into the manifest itself (rather than cross-referenced from
/// session.json) so scan-manifest.json is independently verifiable.
struct ScanSessionIdentity {
    std::uint64_t executableFileSizeBytes = 0;
    /// Lowercase, 64-character hex SHA-256 -- see ExecutableIdentity.hpp.
    std::string sha256Hex;
    std::uint32_t pid = 0;
    /// Valid only for the run that produced this identity -- ASLR-dependent.
    std::uintptr_t mainModuleBaseAddress = 0;
    std::size_t mainModuleImageSize = 0;
    /// ToString(CandidateValueType) / ToString(CandidateScanScope).
    std::string valueType;
    std::string scope;

    bool operator==(const ScanSessionIdentity& other) const;
    bool operator!=(const ScanSessionIdentity& other) const;
};

struct ScanManifest {
    int schemaVersion = kScanManifestSchemaVersion;
    int storageFormatVersion = kScanStorageFormatVersion;

    ScanSessionIdentity identity;
    /// CandidateValueTypeSize(the type named by identity.valueType).
    std::size_t alignment = 0;

    /// Ascending base address -- the same order
    /// IProcessMemoryMap::EnumerateReadableRegions()/ResolveScanScopeRegions()
    /// already produce, and the order baseline.bin's region runs are written in.
    std::vector<ScanManifestRegion> regions;

    std::uint64_t totalScopeBytes = 0;
    std::uint64_t totalValueCount = 0;
    std::uint64_t processedBytes = 0;
    double coveragePercent = 0.0;

    std::size_t memoryBudgetBytes = 0;

    /// 0 = baseline only, not yet filtered.
    int generation = 0;
    std::uint64_t candidateCount = 0;

    ScanManifestState state = ScanManifestState::Planning;
    std::string failureReason;

    /// steady_clock::now().time_since_epoch() microseconds -- NOT
    /// session-relative like DiscoverySession's timestamps, deliberately:
    /// these must stay comparable across a CLI restart for progress
    /// reporting. Informational only -- resume identity checks never read
    /// these fields.
    std::int64_t startMonotonicUs = 0;
    std::int64_t completeMonotonicUs = 0;
    bool completedNormally = false;
};

/// Writes `manifest` as a single JSON object to `path`, truncating any
/// existing content. Returns false on a write error (including failing to
/// open `path`).
bool WriteScanManifest(const std::filesystem::path& path, const ScanManifest& manifest);

/// Reads and parses `path` into `outManifest`. Returns false (leaving
/// `outManifest` untouched) on any I/O or parse error, with a
/// human-readable reason in `outError`. An unrecognized `state` string is
/// also a parse error -- this never guesses.
bool ReadScanManifest(const std::filesystem::path& path, ScanManifest& outManifest, std::string& outError);

} // namespace sekiro_haptics::process
