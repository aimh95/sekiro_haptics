#include "sekiro_haptics/process/Win32ProcessReader.hpp"

#include <windows.h>

#include <cctype>
#include <limits>

namespace sekiro_haptics::process {

static_assert(kProcessAccessMask == (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ),
              "kProcessAccessMask must match the real Win32 access rights it documents");

namespace {

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Genuinely Unicode-safe, exact (not substring) case-insensitive
// comparison for module names/paths, via the Windows-native ordinal
// comparison -- used instead of narrowing either string to ASCII first
// (which could make two different Unicode names collide) or hand-rolled
// towlower() (locale-dependent, not guaranteed correct for the full
// Unicode range). See docs/05-process-access.md.
bool EqualsIgnoreCaseWide(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// Compares two paths case-insensitively after lexical normalization only
// (no symlink/junction resolution -- see IProcessInspector::GetMainModule's
// doc comment for why that's out of scope).
bool PathsEqualIgnoreCase(std::filesystem::path a, std::filesystem::path b) {
    a = a.lexically_normal();
    b = b.lexically_normal();
    return EqualsIgnoreCaseWide(a.wstring(), b.wstring());
}

// Converts a caller-supplied module-name search term (assumed UTF-8, this
// project's convention for narrow strings carrying non-ASCII content) to
// the wide representation module names are compared in.
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    int required = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), required);
    return wide;
}

// Converts an OS-reported wide module name to UTF-8 for ModuleInfo::name --
// a lossless representation (unlike the previous ASCII-approximation),
// still a plain std::string so ModuleInfo's public shape doesn't change.
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    int required =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), required, nullptr,
                         nullptr);
    return utf8;
}

} // namespace


Win32ProcessReader::Win32ProcessReader(IWin32Api& api) : api_(api) {}

Win32ProcessReader::~Win32ProcessReader() {
    Detach();
}

ProcessReaderResult Win32ProcessReader::AttachByPid(std::uint32_t pid) {
    void* newHandle = api_.OpenProcess(kProcessAccessMask, pid);
    if (newHandle == nullptr) {
        return ProcessReaderResult::OpenFailed;
    }

    // Only now -- after the new handle is confirmed good -- release any
    // previous one. A failed attach never reaches this point, so an
    // existing good attachment is never lost to a failed reattach.
    if (handle_ != nullptr) {
        api_.CloseHandle(handle_);
    }
    handle_ = newHandle;
    pid_ = pid;
    return ProcessReaderResult::Success;
}

ProcessReaderResult Win32ProcessReader::AttachByName(const std::string& exeFileName) {
    std::vector<ProcessEnumEntry> processes = api_.EnumerateProcesses();

    std::uint32_t matchedPid = 0;
    std::size_t matchCount = 0;
    for (const ProcessEnumEntry& entry : processes) {
        if (EqualsIgnoreCase(entry.exeName, exeFileName)) {
            ++matchCount;
            matchedPid = entry.pid;
        }
    }

    if (matchCount == 0) {
        return ProcessReaderResult::ProcessNotFound;
    }
    if (matchCount > 1) {
        return ProcessReaderResult::MultipleMatches;
    }
    return AttachByPid(matchedPid);
}

void Win32ProcessReader::Detach() {
    if (handle_ != nullptr) {
        api_.CloseHandle(handle_);
        handle_ = nullptr;
        pid_ = 0;
    }
}

bool Win32ProcessReader::IsAttached() const {
    return handle_ != nullptr;
}

bool Win32ProcessReader::IsAlive() const {
    if (handle_ == nullptr) {
        return false;
    }
    return api_.IsProcessAlive(handle_);
}

std::uint32_t Win32ProcessReader::Pid() const {
    return pid_;
}

ProcessReaderResult Win32ProcessReader::ReadBytes(std::uintptr_t address, void* destination,
                                                    std::size_t requestedSize) {
    if (handle_ == nullptr) {
        return ProcessReaderResult::NotAttached;
    }
    if (destination == nullptr) {
        return ProcessReaderResult::InvalidArgument;
    }

    // Zero-byte reads are a trivial, defined no-op: nothing to read, so
    // nothing is asked of the OS and process liveness isn't even checked
    // -- consistent regardless of what state the attached process is in.
    if (requestedSize == 0) {
        return ProcessReaderResult::Success;
    }

    std::uintptr_t end = address + requestedSize;
    if (end < address) { // wrapped around the address space
        return ProcessReaderResult::InvalidArgument;
    }

    if (!api_.IsProcessAlive(handle_)) {
        return ProcessReaderResult::ProcessExited;
    }

    std::size_t bytesRead = 0;
    bool ok = api_.ReadProcessMemory(handle_, address, destination, requestedSize, bytesRead);
    if (!ok) {
        return ProcessReaderResult::ReadFailed;
    }
    if (bytesRead != requestedSize) {
        return ProcessReaderResult::PartialRead;
    }
    return ProcessReaderResult::Success;
}

ProcessInspectionResult Win32ProcessReader::GetImagePath(std::filesystem::path& outPath) {
    if (handle_ == nullptr) {
        return ProcessInspectionResult::NotAttached;
    }
    if (!api_.IsProcessAlive(handle_)) {
        return ProcessInspectionResult::ProcessExited;
    }

    std::wstring widePath;
    if (!api_.GetProcessImagePath(handle_, widePath)) {
        return ProcessInspectionResult::ImagePathQueryFailed;
    }

    outPath = std::filesystem::path(widePath);
    return ProcessInspectionResult::Success;
}

ProcessInspectionResult Win32ProcessReader::EnumerateModulesChecked(std::vector<ModuleEnumEntry>& outModules) {
    if (handle_ == nullptr) {
        return ProcessInspectionResult::NotAttached;
    }
    if (!api_.IsProcessAlive(handle_)) {
        return ProcessInspectionResult::ProcessExited;
    }
    if (!api_.EnumerateModules(pid_, outModules)) {
        return ProcessInspectionResult::ModuleEnumerationFailed;
    }
    return ProcessInspectionResult::Success;
}

ProcessInspectionResult Win32ProcessReader::ToModuleInfo(const ModuleEnumEntry& entry, ModuleInfo& outModule) const {
    if (entry.baseAddress == 0 || entry.imageSize == 0) {
        return ProcessInspectionResult::InvalidModuleRange;
    }
    std::uintptr_t end = entry.baseAddress + entry.imageSize;
    if (end < entry.baseAddress) { // wrapped around the address space
        return ProcessInspectionResult::InvalidModuleRange;
    }

    ModuleInfo info;
    info.name = WideToUtf8(entry.name);
    info.path = std::filesystem::path(entry.path);
    info.baseAddress = entry.baseAddress;
    info.imageSize = entry.imageSize;
    outModule = std::move(info);
    return ProcessInspectionResult::Success;
}

ProcessInspectionResult Win32ProcessReader::GetMainModule(ModuleInfo& outModule) {
    std::filesystem::path imagePath;
    ProcessInspectionResult pathResult = GetImagePath(imagePath);
    if (pathResult != ProcessInspectionResult::Success) {
        return pathResult;
    }

    std::vector<ModuleEnumEntry> modules;
    ProcessInspectionResult enumResult = EnumerateModulesChecked(modules);
    if (enumResult != ProcessInspectionResult::Success) {
        return enumResult;
    }

    // The main module is *identified*, not assumed to be the first
    // enumerated entry: it's whichever module's own path matches the
    // process's own image path. If that doesn't resolve to exactly one
    // module, this fails closed rather than guessing (e.g. a
    // symlink/junction alias this comparison can't see through).
    const ModuleEnumEntry* matched = nullptr;
    std::size_t matchCount = 0;
    for (const ModuleEnumEntry& entry : modules) {
        if (PathsEqualIgnoreCase(std::filesystem::path(entry.path), imagePath)) {
            ++matchCount;
            matched = &entry;
        }
    }

    if (matchCount != 1) {
        return ProcessInspectionResult::MainModuleNotFound;
    }
    return ToModuleInfo(*matched, outModule);
}

ProcessInspectionResult Win32ProcessReader::FindModuleExact(const std::string& moduleName, ModuleInfo& outModule) {
    std::vector<ModuleEnumEntry> modules;
    ProcessInspectionResult enumResult = EnumerateModulesChecked(modules);
    if (enumResult != ProcessInspectionResult::Success) {
        return enumResult;
    }

    std::wstring wideModuleName = Utf8ToWide(moduleName);
    const ModuleEnumEntry* matched = nullptr;
    std::size_t matchCount = 0;
    for (const ModuleEnumEntry& entry : modules) {
        if (EqualsIgnoreCaseWide(entry.name, wideModuleName)) {
            ++matchCount;
            matched = &entry;
        }
    }

    if (matchCount == 0) {
        return ProcessInspectionResult::ModuleNotFound;
    }
    if (matchCount > 1) {
        return ProcessInspectionResult::MultipleModules;
    }
    return ToModuleInfo(*matched, outModule);
}

namespace {
// A generous but finite safety bound against a runaway enumeration (a
// misbehaving seam, or -- in principle -- a process with a pathologically
// fragmented address space) -- real processes have nowhere near this many
// regions.
constexpr std::size_t kMaxMemoryRegionsPerEnumeration = 200000;
} // namespace

MemoryMapResult Win32ProcessReader::EnumerateReadableRegions(std::vector<ProcessMemoryRegion>& outRegions) const {
    if (handle_ == nullptr) {
        return MemoryMapResult::NotAttached;
    }
    if (!api_.IsProcessAlive(handle_)) {
        return MemoryMapResult::ProcessExited;
    }

    std::vector<ProcessMemoryRegion> collected;
    std::uintptr_t address = 0;
    std::size_t regionsWalked = 0;

    while (true) {
        RawMemoryRegionInfo raw;
        MemoryRegionQueryOutcome outcome = api_.QueryNextMemoryRegion(handle_, address, raw);
        if (outcome == MemoryRegionQueryOutcome::EndOfSpace) {
            break;
        }
        if (outcome == MemoryRegionQueryOutcome::QueryFailed) {
            return MemoryMapResult::EnumerationFailed;
        }

        ++regionsWalked;
        if (regionsWalked > kMaxMemoryRegionsPerEnumeration) {
            return MemoryMapResult::RegionCountLimitExceeded;
        }

        std::uintptr_t regionEnd = raw.baseAddress + raw.regionSize;
        if (regionEnd < raw.baseAddress) {
            // Enumeration cannot safely continue past an overflowing
            // region -- there is no valid "next address" to resume from.
            return MemoryMapResult::AddressOverflow;
        }

        bool readable = raw.committed && raw.readableProtection && !raw.guarded && !raw.noAccessProtection &&
                         raw.regionSize != 0;
        if (readable) {
            ProcessMemoryRegion region;
            region.baseAddress = raw.baseAddress;
            region.sizeBytes = raw.regionSize;
            region.committed = true;
            region.readable = true;
            switch (raw.kind) {
                case RawMemoryRegionKind::Image:
                    region.kind = MemoryRegionKind::Image;
                    break;
                case RawMemoryRegionKind::Mapped:
                    region.kind = MemoryRegionKind::Mapped;
                    break;
                case RawMemoryRegionKind::Private:
                case RawMemoryRegionKind::Unknown:
                default:
                    region.kind = MemoryRegionKind::Private;
                    break;
            }
            collected.push_back(region);
        }

        if (raw.regionSize == 0 || regionEnd <= address) {
            // Can't safely advance further -- treat as the end of what
            // can be enumerated.
            break;
        }
        address = regionEnd;
    }

    outRegions = std::move(collected);
    return MemoryMapResult::Success;
}

} // namespace sekiro_haptics::process
