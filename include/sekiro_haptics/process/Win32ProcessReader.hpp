#pragma once

#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/IWin32Api.hpp"

#include <cstdint>
#include <vector>

namespace sekiro_haptics::process {

/// Win32 implementation of IProcessReader. Despite living in this
/// project, nothing here is Sekiro-specific -- AttachByPid()/AttachByName()
/// take any PID/name, so this class is reusable for any external Windows
/// process. See docs/05-process-access.md.
///
/// Access rights requested are exactly kProcessAccessMask
/// (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ) -- never
/// PROCESS_VM_WRITE, PROCESS_VM_OPERATION, or PROCESS_ALL_ACCESS, and
/// WriteProcessMemory/VirtualAllocEx/VirtualProtectEx/CreateRemoteThread/
/// SetThreadContext are never called anywhere in this class. That
/// omission -- not just IProcessReader's missing WriteBytes() -- is this
/// module's enforced read-only boundary.
///
/// Reattach contract: AttachByPid()/AttachByName() only replace the
/// current attachment *after* the new handle has been successfully
/// opened. A failed reattach (process not found, ambiguous name, open
/// failure) leaves any existing attachment completely untouched -- a
/// caller can never lose a good connection because a *subsequent* attach
/// attempt failed.
///
/// All Win32 calls go through an injected IWin32Api, defaulting to the
/// real implementation (see Win32Api.cpp) -- this is what lets unit tests
/// substitute a fake and assert exact call arguments (the access mask in
/// particular) or simulate failures no reliable real-OS test could
/// provoke on demand.
///
/// Not copyable (a copy would double-close the handle on destruction);
/// move is not supported either -- there is always exactly one owner.
///
/// Also implements IProcessInspector (path/module/identity queries) on
/// the same attached process, through the same handle -- see
/// docs/05-process-access.md for why this is one class implementing two
/// focused interfaces rather than either bloating IProcessReader or
/// standing up a second class that would need its own (redundant) OS
/// handle to the same process.
class Win32ProcessReader final : public IProcessReader, public IProcessInspector {
public:
    /// `api` must outlive this reader.
    explicit Win32ProcessReader(IWin32Api& api = RealWin32Api());
    ~Win32ProcessReader() override;

    Win32ProcessReader(const Win32ProcessReader&) = delete;
    Win32ProcessReader& operator=(const Win32ProcessReader&) = delete;

    // IProcessReader
    ProcessReaderResult AttachByPid(std::uint32_t pid) override;
    ProcessReaderResult AttachByName(const std::string& exeFileName) override;
    void Detach() override;
    bool IsAttached() const override;
    bool IsAlive() const override;
    std::uint32_t Pid() const override;
    ProcessReaderResult ReadBytes(std::uintptr_t address, void* destination, std::size_t requestedSize) override;

    // IProcessInspector
    ProcessInspectionResult GetImagePath(std::filesystem::path& outPath) override;
    ProcessInspectionResult GetMainModule(ModuleInfo& outModule) override;
    ProcessInspectionResult FindModuleExact(const std::string& moduleName, ModuleInfo& outModule) override;

    /// The process-wide real IWin32Api instance, used as this class's
    /// default constructor argument.
    static IWin32Api& RealWin32Api();

private:
    ProcessInspectionResult EnumerateModulesChecked(std::vector<ModuleEnumEntry>& outModules);
    ProcessInspectionResult ToModuleInfo(const ModuleEnumEntry& entry, ModuleInfo& outModule) const;

    IWin32Api& api_;
    void* handle_ = nullptr;
    std::uint32_t pid_ = 0;
};

} // namespace sekiro_haptics::process
