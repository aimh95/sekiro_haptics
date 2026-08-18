#include "sekiro_haptics/process/IProcessMemoryMap.hpp"

namespace sekiro_haptics::process {

// Moved here (from Win32ProcessReader.cpp, its original home) as part of
// SEK-PROBE-001C: this is pure string-mapping logic with no OS dependency,
// so it belongs in this OS-independent file rather than the Win32-only
// implementation file -- the disk-backed scanner's portable code needs to
// report MemoryMapResult failures without depending on Win32ProcessReader.
const char* ToString(MemoryMapResult result) {
    switch (result) {
        case MemoryMapResult::Success:
            return "Success";
        case MemoryMapResult::NotAttached:
            return "NotAttached";
        case MemoryMapResult::ProcessExited:
            return "ProcessExited";
        case MemoryMapResult::EnumerationFailed:
            return "EnumerationFailed";
        case MemoryMapResult::AddressOverflow:
            return "AddressOverflow";
        case MemoryMapResult::RegionCountLimitExceeded:
            return "RegionCountLimitExceeded";
    }
    return "Unknown";
}

const char* ToString(MemoryRegionKind kind) {
    switch (kind) {
        case MemoryRegionKind::Image:
            return "Image";
        case MemoryRegionKind::Mapped:
            return "Mapped";
        case MemoryRegionKind::Private:
            return "Private";
    }
    return "Unknown";
}

} // namespace sekiro_haptics::process
