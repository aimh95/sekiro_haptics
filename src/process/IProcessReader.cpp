#include "sekiro_haptics/process/IProcessReader.hpp"

namespace sekiro_haptics::process {

// Moved here (from Win32ProcessReader.cpp, its original home) as part of
// SEK-PROBE-001C: this is pure string-mapping logic with no OS dependency,
// so it belongs in this OS-independent file rather than the Win32-only
// implementation file, for the same reason ToString(MemoryMapResult) and
// ToString(ProcessInspectionResult) were moved alongside it.
const char* ToString(ProcessReaderResult result) {
    switch (result) {
        case ProcessReaderResult::Success:
            return "Success";
        case ProcessReaderResult::NotAttached:
            return "NotAttached";
        case ProcessReaderResult::ProcessNotFound:
            return "ProcessNotFound";
        case ProcessReaderResult::MultipleMatches:
            return "MultipleMatches";
        case ProcessReaderResult::OpenFailed:
            return "OpenFailed";
        case ProcessReaderResult::ProcessExited:
            return "ProcessExited";
        case ProcessReaderResult::InvalidArgument:
            return "InvalidArgument";
        case ProcessReaderResult::ReadFailed:
            return "ReadFailed";
        case ProcessReaderResult::PartialRead:
            return "PartialRead";
    }
    return "Unknown";
}

} // namespace sekiro_haptics::process
