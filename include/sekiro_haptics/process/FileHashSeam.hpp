#pragma once

// Internal seam for ExecutableIdentity.cpp's file-read and SHA-256
// hashing steps, and their own deterministic tests
// (test_executable_identity.cpp). This is NOT a general-purpose
// filesystem or crypto abstraction, and it is not part of
// IProcessReader/IProcessInspector -- those stay exactly as narrow as
// SEK-READ-001A/001B left them. It exists purely so a test can inject a
// specific, named failure (a particular CNG call failing, a short read,
// a read failing partway through) deterministically, the same way
// IWin32Api lets Win32ProcessReader's tests inject OS-level failures.
//
// ComputeExecutableIdentity() (ExecutableIdentity.hpp, unchanged public
// signature) is a thin wrapper that calls ComputeExecutableIdentityWithSeams()
// below with the real Win32/CNG implementations. Normal callers never see
// this header.

#include "sekiro_haptics/process/IProcessInspector.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace sekiro_haptics::process {

struct ExecutableIdentity;

/// Seam over the file-read side of ComputeExecutableIdentity(). The real
/// implementation (Win32FileReader, in ExecutableIdentity.cpp) opens the
/// file exactly once via CreateFileW and performs both the size query and
/// the full read through that single handle -- see
/// docs/05-process-access.md for the sharing-mode/stability contract this
/// exists to support.
class IFileReader {
public:
    virtual ~IFileReader() = default;

    /// Opens `path` for reading. Returns an opaque non-null handle on
    /// success, or nullptr on failure (missing file, a directory, access
    /// denied, or an incompatible existing open elsewhere).
    virtual void* Open(const std::filesystem::path& path) = 0;

    /// Queries the file's size from the SAME already-open `handle` -- never
    /// a separate path-based stat call, so this can't observe a different
    /// file state than the read that follows. False on failure.
    virtual bool GetSize(void* handle, std::uint64_t& outSize) = 0;

    /// Reads up to `bufferSize` bytes from `handle`'s current position.
    /// Returns false on a hard read failure. On true, `outBytesRead` may be
    /// less than `bufferSize` (short read) or 0 (end of file) -- both are
    /// normal true-returning outcomes; it is the caller's job to compare
    /// accumulated bytes read against GetSize()'s value.
    virtual bool Read(void* handle, void* buffer, std::size_t bufferSize, std::size_t& outBytesRead) = 0;

    /// Closes a handle returned by Open(). Safe to call with nullptr.
    virtual void Close(void* handle) = 0;
};

/// Seam over the CNG (SHA-256) side of ComputeExecutableIdentity(),
/// broken into the five distinct steps a real BCrypt-based SHA-256
/// computation involves, so each can independently be made to fail in a
/// test: opening the algorithm provider, querying the required hash-object
/// buffer size, creating the hash object, feeding data in, and finishing
/// the digest. The real implementation (Win32CryptoApi, in
/// ExecutableIdentity.cpp) is a thin wrapper over BCrypt*; nothing here
/// invents cryptography of its own.
class ICryptoApi {
public:
    virtual ~ICryptoApi() = default;

    /// Opens the SHA-256 algorithm provider. Non-null handle on success.
    virtual void* OpenAlgorithmProvider() = 0;

    /// Queries the byte length of the caller-allocated object buffer
    /// CreateHash() below needs. False on failure.
    virtual bool GetHashObjectLength(void* algHandle, std::size_t& outLength) = 0;

    /// Creates a hash object using a caller-allocated `hashObjectBuffer` of
    /// `hashObjectBufferLength` bytes (from GetHashObjectLength()). The
    /// buffer must remain valid for the hash object's lifetime. Non-null
    /// handle on success.
    virtual void* CreateHash(void* algHandle, void* hashObjectBuffer, std::size_t hashObjectBufferLength) = 0;

    /// Feeds `length` bytes at `data` into the hash. False on failure.
    virtual bool HashData(void* hashHandle, const void* data, std::size_t length) = 0;

    /// Finalizes the hash, writing exactly 32 bytes to `outDigest32`. False
    /// on failure -- `outDigest32` must not be trusted in that case.
    virtual bool FinishHash(void* hashHandle, std::uint8_t* outDigest32) = 0;

    /// Releases a handle from CreateHash(). Safe to call with nullptr.
    virtual void DestroyHash(void* hashHandle) = 0;

    /// Releases a handle from OpenAlgorithmProvider(). Safe to call with nullptr.
    virtual void CloseAlgorithmProvider(void* algHandle) = 0;
};

/// The real logic behind ComputeExecutableIdentity(), parameterized over
/// the file-read and hashing seams above so a test can inject a specific
/// deterministic failure at any step. ComputeExecutableIdentity() (the
/// public function, ExecutableIdentity.hpp) calls this with the real Win32
/// implementations -- see ExecutableIdentity.cpp.
ProcessInspectionResult ComputeExecutableIdentityWithSeams(const std::filesystem::path& path,
                                                             std::uintptr_t mainModuleBaseAddress,
                                                             std::size_t mainModuleImageSize, IFileReader& fileReader,
                                                             ICryptoApi& cryptoApi, ExecutableIdentity& outIdentity);

} // namespace sekiro_haptics::process
