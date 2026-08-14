#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

/// One running process, as reported by process enumeration: PID and
/// executable filename only (no path, no other metadata) -- everything
/// Win32ProcessReader's name-matching logic needs and nothing more.
struct ProcessEnumEntry {
    std::uint32_t pid = 0;
    std::string exeName;
};

/// One loaded module, as reported by module enumeration. Both `name` and
/// `path` are kept in the OS's native wide representation, exactly as
/// reported by Module32FirstW/NextW -- never narrowed lossily. Matching
/// against `name` must be done via a genuinely Unicode-safe comparison
/// (e.g. CompareStringOrdinal with the case-insensitive flag), not by
/// narrowing either side to ASCII first -- see
/// docs/05-process-access.md's "Unicode module-name matching" section for
/// why (two different non-ASCII names must never be able to collide).
struct ModuleEnumEntry {
    std::wstring name;
    std::wstring path;
    std::uintptr_t baseAddress = 0;
    std::size_t imageSize = 0;
};

/// Seam between Win32ProcessReader and the actual Win32 API calls it
/// needs, so a test can substitute a fake and (a) assert exactly what
/// access mask/PID Win32ProcessReader requests, without a real process to
/// attach to, and (b) simulate OS-level failure modes (OpenProcess
/// failing, a short/failed ReadProcessMemory, a process that has exited)
/// that are impractical or impossible to provoke reliably against a real
/// OS. See src/process/Win32Api.cpp for the real implementation and
/// src/process/Win32ProcessReader.cpp for how it's used.
///
/// This interface exists purely as a test seam for *this* read-only
/// module -- it is intentionally shaped around exactly the four
/// operations Win32ProcessReader needs (enumerate, open, read, check
/// liveness, close) and nothing else. It has no write-memory method, to
/// match IProcessReader's own read-only boundary.
class IWin32Api {
public:
    virtual ~IWin32Api() = default;

    /// Lists every currently-running process this caller can see.
    virtual std::vector<ProcessEnumEntry> EnumerateProcesses() = 0;

    /// Opens `pid` with exactly `desiredAccessMask` (see
    /// kProcessAccessMask in IProcessReader.hpp -- callers must never pass
    /// anything else). Returns an opaque, non-null handle on success, or
    /// nullptr on failure. The returned handle must be released via
    /// CloseHandle() exactly once.
    virtual void* OpenProcess(std::uint32_t desiredAccessMask, std::uint32_t pid) = 0;

    /// Reads `size` bytes from `address` in the process owning `handle`
    /// into `buffer`. Returns false on a hard failure; on true,
    /// `outBytesRead` holds however many bytes were actually read, which
    /// may be less than `size` (a partial read) even though this returns
    /// true -- distinguishing that is the caller's (Win32ProcessReader's)
    /// job, not this seam's.
    virtual bool ReadProcessMemory(void* handle, std::uintptr_t address, void* buffer, std::size_t size,
                                    std::size_t& outBytesRead) = 0;

    /// Whether the process owning `handle` is still running.
    virtual bool IsProcessAlive(void* handle) = 0;

    /// Releases a handle returned by OpenProcess(). Safe to call with a
    /// handle already closed by this same call only once per handle --
    /// Win32ProcessReader guarantees it never calls this twice for the
    /// same handle (see its class comment).
    virtual void CloseHandle(void* handle) = 0;

    /// The full image path of the process owning `handle` (via
    /// QueryFullProcessImageNameW -- needs only
    /// PROCESS_QUERY_LIMITED_INFORMATION, already part of
    /// kProcessAccessMask, no extra access rights). False on failure,
    /// including a path too long for this call's internal buffer --
    /// never a silently truncated path.
    virtual bool GetProcessImagePath(void* handle, std::wstring& outPath) = 0;

    /// Lists every module currently loaded in the process with this PID.
    /// False only if the enumeration mechanism itself failed; an empty
    /// (but successfully obtained) list is reported as true with zero
    /// entries, not a failure.
    virtual bool EnumerateModules(std::uint32_t pid, std::vector<ModuleEnumEntry>& outModules) = 0;
};

} // namespace sekiro_haptics::process
