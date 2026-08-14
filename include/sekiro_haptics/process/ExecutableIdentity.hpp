#pragma once

#include "sekiro_haptics/process/IProcessInspector.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sekiro_haptics::process {

/// A SHA-256 digest: 32 raw bytes as the primary representation, with an
/// on-demand lowercase 64-character hex conversion for logging/JSON.
struct Sha256Digest {
    std::array<std::uint8_t, 32> bytes{};

    bool operator==(const Sha256Digest& other) const { return bytes == other.bytes; }
    bool operator!=(const Sha256Digest& other) const { return !(*this == other); }
};

/// Lowercase 64-character hex representation of `digest`.
std::string ToHex(const Sha256Digest& digest);

/// Identifies a specific executable **build**, decoupled from where it's
/// running or how it's currently laid out in memory.
///
/// Two different kinds of information are deliberately kept together but
/// distinct here:
/// - **Stable build identity**: `fileSizeBytes` + `sha256`. Two
///   ExecutableIdentity values with the same build identity came from
///   byte-for-byte the same file, full stop -- independent of path or
///   where ASLR happened to place it this run.
/// - **Runtime placement**: `mainModuleBaseAddress` + `mainModuleImageSize`.
///   Changes every run under ASLR, even for the exact same build.
///
/// Neither `executablePath` nor `mainModuleBaseAddress` should ever be
/// used as a substitute for the (fileSizeBytes, sha256) pair when deciding
/// "is this the same build I saw before" -- see docs/05-process-access.md.
struct ExecutableIdentity {
    std::filesystem::path executablePath;
    std::uint64_t fileSizeBytes = 0;
    Sha256Digest sha256;
    std::uintptr_t mainModuleBaseAddress = 0;
    std::size_t mainModuleImageSize = 0;
};

/// Reads the file at `path` in full and computes its SHA-256 + actual size,
/// combined with the caller-supplied module base/size (normally obtained
/// separately via IProcessInspector::GetMainModule() for the process
/// `path` came from) into a complete ExecutableIdentity.
///
/// Pure file I/O -- no live process, IProcessReader, or IProcessInspector
/// involvement, so this is directly testable against any file, including
/// plain test-authored temp files.
///
/// Fails closed: `outIdentity` is only written to on
/// ProcessInspectionResult::Success. A file that can't be opened, a read
/// that returns fewer bytes than the file's own reported size (changed/
/// truncated mid-read), or a hashing-API failure are distinct, typed
/// failures -- never silently treated as success, and never left to look
/// like a previous successful call's result.
ProcessInspectionResult ComputeExecutableIdentity(const std::filesystem::path& path,
                                                    std::uintptr_t mainModuleBaseAddress,
                                                    std::size_t mainModuleImageSize,
                                                    ExecutableIdentity& outIdentity);

/// Convenience orchestrator for the common case: GetImagePath() +
/// GetMainModule() via `inspector` (an already-attached process), then
/// ComputeExecutableIdentity() on the result. Stops and returns at the
/// first failing step -- never returns a partially-filled
/// ExecutableIdentity assembled from some steps succeeding and others not.
ProcessInspectionResult BuildExecutableIdentity(IProcessInspector& inspector, ExecutableIdentity& outIdentity);

} // namespace sekiro_haptics::process
