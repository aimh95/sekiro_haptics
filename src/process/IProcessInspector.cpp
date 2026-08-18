#include "sekiro_haptics/process/IProcessInspector.hpp"

namespace sekiro_haptics::process {

// Moved here (from Win32ProcessReader.cpp, its original home) as part of
// SEK-PROBE-001C: this is pure string-mapping logic with no OS dependency,
// so it belongs in this OS-independent file rather than the Win32-only
// implementation file -- AddressResolver.cpp (portable) needs to report
// ProcessInspectionResult failures without depending on Win32ProcessReader.
const char* ToString(ProcessInspectionResult result) {
    switch (result) {
        case ProcessInspectionResult::Success:
            return "Success";
        case ProcessInspectionResult::NotAttached:
            return "NotAttached";
        case ProcessInspectionResult::ProcessExited:
            return "ProcessExited";
        case ProcessInspectionResult::ImagePathQueryFailed:
            return "ImagePathQueryFailed";
        case ProcessInspectionResult::ModuleEnumerationFailed:
            return "ModuleEnumerationFailed";
        case ProcessInspectionResult::MainModuleNotFound:
            return "MainModuleNotFound";
        case ProcessInspectionResult::ModuleNotFound:
            return "ModuleNotFound";
        case ProcessInspectionResult::MultipleModules:
            return "MultipleModules";
        case ProcessInspectionResult::InvalidModuleRange:
            return "InvalidModuleRange";
        case ProcessInspectionResult::FileOpenFailed:
            return "FileOpenFailed";
        case ProcessInspectionResult::FileReadFailed:
            return "FileReadFailed";
        case ProcessInspectionResult::HashFailed:
            return "HashFailed";
    }
    return "Unknown";
}

} // namespace sekiro_haptics::process
