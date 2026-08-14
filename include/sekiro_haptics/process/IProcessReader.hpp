#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sekiro_haptics::process {

/// The exact OpenProcess access mask every IProcessReader implementation
/// must request: PROCESS_QUERY_LIMITED_INFORMATION (0x1000) |
/// PROCESS_VM_READ (0x0010). Read-only, minimal-privilege by construction
/// -- there is no corresponding write/allocate/thread-injection mask
/// anywhere in this module, and there must never be one. See
/// docs/05-process-access.md.
///
/// Defined here (not just inside the Win32 implementation) so a Fake-API
/// unit test can assert the real implementation requests exactly this
/// value without needing <windows.h> itself; Win32ProcessReader.cpp
/// static_asserts this equals the real PROCESS_QUERY_LIMITED_INFORMATION |
/// PROCESS_VM_READ macros.
inline constexpr std::uint32_t kProcessAccessMask = 0x1000u | 0x0010u;

/// Outcome of an IProcessReader attach/read operation. One shared enum
/// across both operation kinds (matching this project's existing
/// TransportResult/HapticBackendResult convention) rather than a separate
/// enum per method.
enum class ProcessReaderResult {
    Success,
    /// ReadBytes() called with no process attached.
    NotAttached,
    /// AttachByName() found no process with that exact (case-insensitive)
    /// executable name.
    ProcessNotFound,
    /// AttachByName() found more than one process with that name. The
    /// caller must disambiguate via AttachByPid() -- this reader never
    /// guesses which one was meant.
    MultipleMatches,
    /// OpenProcess (or equivalent) failed for a specific PID that does
    /// exist (e.g. access denied, or it exited between being found and
    /// being opened).
    OpenFailed,
    /// ReadBytes() (or an internal liveness check it performs) found the
    /// attached process is no longer running.
    ProcessExited,
    /// A null destination buffer, or an address+size that would overflow
    /// the address space, was passed to ReadBytes().
    InvalidArgument,
    /// The underlying read call itself failed (e.g. unmapped/protected
    /// address) for a still-running, attached process.
    ReadFailed,
    /// The read call reported success but returned fewer bytes than
    /// requested. Never treated as Success -- see ReadBytes()'s contract.
    PartialRead,
};

/// Returns a human-readable name for a ProcessReaderResult, e.g. for logging.
const char* ToString(ProcessReaderResult result);

/// Read-only abstraction over "an external OS process's memory," useful
/// for any external process, not tied to Sekiro or any other specific
/// game -- see docs/05-process-access.md. Deliberately narrow: this
/// interface only ever reports connection state, process identity,
/// liveness, and raw bytes at a caller-supplied address. It has no idea
/// what those bytes *mean* -- interpreting them (offsets, pointer chains,
/// signatures) is a higher layer's job (SEK-READ-001B), not this one's.
///
/// No implementation of this interface may expose a write path. There is
/// no WriteBytes() here, and there must never be one added -- this is the
/// structural enforcement of this project's read-only boundary, the same
/// pattern IDualSenseTransport already uses.
class IProcessReader {
public:
    virtual ~IProcessReader() = default;

    /// Attaches to the process with this exact PID. On success, replaces
    /// any previously-attached process; on failure, any existing
    /// attachment is left completely untouched (see Win32ProcessReader's
    /// class comment for why this ordering matters).
    virtual ProcessReaderResult AttachByPid(std::uint32_t pid) = 0;

    /// Attaches to the (single) running process whose executable filename
    /// matches `exeFileName` exactly, case-insensitively -- no substring
    /// matching. Zero matches is ProcessNotFound; more than one is
    /// MultipleMatches (the caller must resolve the ambiguity itself, via
    /// AttachByPid()). Same replace-only-on-success contract as
    /// AttachByPid().
    virtual ProcessReaderResult AttachByName(const std::string& exeFileName) = 0;

    /// Detaches from the current process, if any. Safe to call when not
    /// attached, and safe to call more than once.
    virtual void Detach() = 0;

    virtual bool IsAttached() const = 0;

    /// Whether the attached process is still running. False if not
    /// attached at all.
    virtual bool IsAlive() const = 0;

    /// The attached process's PID, or 0 if not attached.
    virtual std::uint32_t Pid() const = 0;

    /// Reads `requestedSize` bytes starting at `address` in the attached
    /// process into `destination`.
    ///
    /// requestedSize == 0 always succeeds trivially (nothing to read, no
    /// OS call made) regardless of process liveness, as long as a process
    /// is attached -- see docs/05-process-access.md for the full
    /// precedence of checks. A non-zero request that reads fewer bytes
    /// than asked for is PartialRead, never Success -- `destination`'s
    /// contents must not be treated as valid data on anything other than
    /// Success.
    virtual ProcessReaderResult ReadBytes(std::uintptr_t address, void* destination, std::size_t requestedSize) = 0;
};

} // namespace sekiro_haptics::process
