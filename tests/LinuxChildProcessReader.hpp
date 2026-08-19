#pragma once

// Test-only Linux IProcessReader/IProcessInspector/IProcessMemoryMap
// implementation for SEK-PROBE-001C-LINUX-E2E: real process_vm_readv()
// cross-process reads and real /proc/<pid>/maps region validation against
// a genuinely separate child process -- never a Fake. Deliberately NOT a
// general-purpose Linux process backend: it only ever attaches to the
// single PID it was constructed with (the exact child this same test
// binary directly forked), and EnumerateReadableRegions() only ever
// reports the single pre-registered arena region it was told about, never
// the process's full memory map. This exists purely so the existing,
// unmodified DiskCandidateScanner/CandidateStorage/ScanManifest/
// SignalProbeScanController code can be exercised against a real separate
// process's real memory in this Linux development sandbox -- it is not,
// and must never become, a Linux/Proton product backend (see
// docs/06-signal-discovery-probe.md and CMakeLists.txt's
// if(UNIX AND NOT APPLE) gating).
//
// Never used to attach to an arbitrary existing system process: the
// authorized PID is fixed at construction time from a real fork() return
// value the caller captured directly.

#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessMemoryMap.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"

#include <sys/types.h>

#include <cstdint>

namespace sekiro_haptics::process {

class LinuxChildProcessReader final : public IProcessReader, public IProcessInspector, public IProcessMemoryMap {
public:
    /// `authorizedPid` must be a real child this test binary directly
    /// forked. `arenaBase`/`arenaSize` are the exact bounds the helper
    /// itself reported over its READY handshake -- EnumerateReadableRegions()
    /// validates (never assumes) these bounds against a real
    /// /proc/<authorizedPid>/maps read before ever reporting them as a
    /// scannable region.
    LinuxChildProcessReader(pid_t authorizedPid, std::uintptr_t arenaBase, std::size_t arenaSize);

    // --- IProcessReader ---

    /// Succeeds only if `pid` matches the PID this object was constructed
    /// with -- refuses (ProcessNotFound) any other PID, including a real,
    /// currently-running one. This is the structural enforcement of "only
    /// ever attach to the child this test itself launched."
    ProcessReaderResult AttachByPid(std::uint32_t pid) override;
    /// Always ProcessNotFound -- this test-only adapter never resolves a
    /// process by name; every caller already knows the exact authorized PID.
    ProcessReaderResult AttachByName(const std::string& exeFileName) override;
    void Detach() override;
    bool IsAttached() const override;
    /// kill(pid, 0) == 0. Callers that just sent EXIT and want an accurate
    /// answer must reap the child first (LinuxHelperProcess::WaitForExit())
    /// -- an unreaped zombie's PID slot still answers kill(pid,0)
    /// successfully even though the process is no longer really running.
    bool IsAlive() const override;
    std::uint32_t Pid() const override;
    /// Real process_vm_readv() against the attached PID. requestedSize==0
    /// always succeeds trivially per the interface contract. A short
    /// transfer (0 < bytesRead < requestedSize) is PartialRead, never
    /// Success. ESRCH is mapped to ProcessExited; every other failure
    /// (including EPERM) is ReadFailed -- see LastErrno() for the exact
    /// errno a caller needs to tell an unexpected EPERM apart from an
    /// ordinary unmapped-address failure.
    ProcessReaderResult ReadBytes(std::uintptr_t address, void* destination, std::size_t requestedSize) override;

    /// The errno from the most recent failed process_vm_readv() call (0 if
    /// the most recent call succeeded, or none has been made yet).
    /// Test-only diagnostic accessor -- not part of IProcessReader itself.
    int LastErrno() const { return lastErrno_; }

    // --- IProcessInspector ---
    // This Linux test adapter has no PE-style "module" concept and this
    // ticket only ever exercises CandidateScanScope::PrivateReadable
    // (the helper's own mmap arena) -- MainModule-scope callers are
    // intentionally unsupported here, honestly reported as such rather
    // than fabricating a fake module.

    /// Real readlink("/proc/<pid>/exe") -- genuine OS introspection, not
    /// fabricated, even though nothing in this ticket's scenario uses it.
    ProcessInspectionResult GetImagePath(std::filesystem::path& outPath) override;
    /// Always MainModuleNotFound -- see the class comment above.
    ProcessInspectionResult GetMainModule(ModuleInfo& outModule) override;
    /// Always ModuleNotFound -- see the class comment above.
    ProcessInspectionResult FindModuleExact(const std::string& moduleName, ModuleInfo& outModule) override;

    // --- IProcessMemoryMap ---

    /// Reads real /proc/<pid>/maps and reports exactly one region -- the
    /// pre-registered arena -- only if a mapping line is found that fully
    /// contains [arenaBase, arenaBase+arenaSize), has read permission, and
    /// is a private mapping ('p', not 's'). Never reports any other region
    /// from the process's real memory map (its full libc/heap/stack/etc.
    /// layout is deliberately never exposed to the scanner through this
    /// adapter). EnumerationFailed if the arena can't be confirmed this way.
    MemoryMapResult EnumerateReadableRegions(std::vector<ProcessMemoryRegion>& outRegions) const override;

private:
    pid_t authorizedPid_ = -1;
    pid_t attachedPid_ = -1;
    std::uintptr_t arenaBase_ = 0;
    std::size_t arenaSize_ = 0;
    mutable int lastErrno_ = 0;
};

} // namespace sekiro_haptics::process
