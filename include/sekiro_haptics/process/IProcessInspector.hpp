#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sekiro_haptics::process {

/// Outcome of a process-inspection or executable-identity operation. One
/// shared enum across IProcessInspector's methods and ExecutableIdentity's
/// (see ExecutableIdentity.hpp) -- matching this project's existing
/// TransportResult/ProcessReaderResult convention. Any single method only
/// produces a subset of these values; see each method's doc comment for
/// which.
enum class ProcessInspectionResult {
    Success,
    /// No process attached.
    NotAttached,
    /// The attached process is no longer running.
    ProcessExited,
    /// The OS image-path query itself failed (including a path too long
    /// for the query to return without truncation -- never treated as
    /// success).
    ImagePathQueryFailed,
    /// The OS module-enumeration mechanism itself failed.
    ModuleEnumerationFailed,
    /// The process's own image path didn't match exactly one enumerated
    /// module's path -- zero matches, or more than one that couldn't be
    /// disambiguated (e.g. via a symlink/junction alias this module does
    /// not attempt to resolve). Never guessed at.
    MainModuleNotFound,
    /// FindModuleExact() found no module with that exact name.
    ModuleNotFound,
    /// FindModuleExact() found more than one module with that exact name.
    MultipleModules,
    /// A module's base address or image size was zero, or base+imageSize
    /// would overflow the address space.
    InvalidModuleRange,
    /// ExecutableIdentity: the executable file couldn't be opened.
    FileOpenFailed,
    /// ExecutableIdentity: reading the file failed, or read fewer bytes
    /// than its own reported size (partial/changed file) -- never treated
    /// as success.
    FileReadFailed,
    /// ExecutableIdentity: the hashing operation itself failed.
    HashFailed,
};

/// Returns a human-readable name for a ProcessInspectionResult, e.g. for logging.
const char* ToString(ProcessInspectionResult result);

/// One loaded module in an attached process: identity and runtime
/// placement only, never a place to invent an interpretation of what's
/// inside it. `path` is a full, Unicode-safe path (std::filesystem::path,
/// backed by the OS's native wide representation on Windows -- never
/// narrowed lossily). `name` is `path`'s basename, kept separately as a
/// plain string for straightforward case-insensitive exact-match lookups
/// (see FindModuleExact).
struct ModuleInfo {
    std::string name;
    std::filesystem::path path;
    std::uintptr_t baseAddress = 0;
    std::size_t imageSize = 0;
};

/// Read-only inspection of an *already-attached* process's executable
/// identity and module layout -- a companion to IProcessReader, not a
/// replacement or extension of it (see docs/05-process-access.md for why
/// this is a separate interface). Implementations query OS metadata only;
/// none of this touches process memory contents, and none of it invents
/// or guesses an answer it can't verify -- every method fails closed (see
/// ProcessInspectionResult) rather than returning a partially-correct
/// result.
class IProcessInspector {
public:
    virtual ~IProcessInspector() = default;

    /// The attached process's own full executable image path.
    virtual ProcessInspectionResult GetImagePath(std::filesystem::path& outPath) = 0;

    /// The main module: the one loaded module whose path matches the
    /// process's own image path (case-insensitive, lexically normalized --
    /// no symlink/junction resolution attempted). This is *not* assumed to
    /// be the first entry module enumeration happens to return -- see
    /// docs/05-process-access.md. Fails (MainModuleNotFound) if that
    /// comparison doesn't resolve to exactly one module.
    virtual ProcessInspectionResult GetMainModule(ModuleInfo& outModule) = 0;

    /// Finds the (single) loaded module whose name matches `moduleName`
    /// exactly, case-insensitively -- no substring matching. Zero matches
    /// is ModuleNotFound; more than one is MultipleModules.
    virtual ProcessInspectionResult FindModuleExact(const std::string& moduleName, ModuleInfo& outModule) = 0;
};

} // namespace sekiro_haptics::process
