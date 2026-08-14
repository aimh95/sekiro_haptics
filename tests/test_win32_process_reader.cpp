// Fake-API unit tests for Win32ProcessReader. Every OS-level behavior
// (enumeration results, OpenProcess success/failure, ReadProcessMemory
// success/partial/failure, process liveness) is driven through
// FakeWin32Api -- no real process is ever touched here.

#include "sekiro_haptics/process/Win32ProcessReader.hpp"
#include "testing.hpp"

#include "FakeWin32Api.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <type_traits>

using namespace sekiro_haptics::process;

namespace {

// Widens a plain-ASCII test name to std::wstring -- lossless because every
// existing call site here only ever passes ASCII names. Unicode-specific
// tests below construct ModuleEnumEntry with an explicit wide literal
// instead of going through this helper.
std::wstring WidenAscii(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

ModuleEnumEntry MakeModule(const std::string& name, const std::wstring& path, std::uintptr_t base,
                            std::size_t size) {
    ModuleEnumEntry m;
    m.name = WidenAscii(name);
    m.path = path;
    m.baseAddress = base;
    m.imageSize = size;
    return m;
}

ModuleEnumEntry MakeModuleW(const std::wstring& name, const std::wstring& path, std::uintptr_t base,
                             std::size_t size) {
    ModuleEnumEntry m;
    m.name = name;
    m.path = path;
    m.baseAddress = base;
    m.imageSize = size;
    return m;
}

} // namespace

// Copying would double-close the underlying handle on destruction -- must
// not compile. (Move is intentionally not supported either -- there is
// always exactly one owner.)
static_assert(!std::is_copy_constructible_v<Win32ProcessReader>);
static_assert(!std::is_copy_assignable_v<Win32ProcessReader>);

// --- Attach: by PID ---

SH_TEST(Win32ProcessReader_AttachByPid_Success) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);

    ProcessReaderResult result = reader.AttachByPid(1234);

    SH_CHECK(result == ProcessReaderResult::Success);
    SH_CHECK(reader.IsAttached());
    SH_CHECK(reader.Pid() == 1234);
}

SH_TEST(Win32ProcessReader_AttachByPid_RequestsExactAccessMask) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);

    reader.AttachByPid(1234);

    SH_CHECK(api.LastRequestedAccessMask() == kProcessAccessMask);
    SH_CHECK(api.LastOpenedPid() == 1234);
}

SH_TEST(Win32ProcessReader_AttachByPid_OpenProcessFails_ReturnsOpenFailed) {
    FakeWin32Api api;
    api.SetOpenShouldSucceed(false);
    Win32ProcessReader reader(api);

    ProcessReaderResult result = reader.AttachByPid(1234);

    SH_CHECK(result == ProcessReaderResult::OpenFailed);
    SH_CHECK(reader.IsAttached() == false);
}

// --- Attach: by name ---

SH_TEST(Win32ProcessReader_AttachByName_ExactMatch_Success) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}, {5678, "other.exe"}});
    Win32ProcessReader reader(api);

    ProcessReaderResult result = reader.AttachByName("game.exe");

    SH_CHECK(result == ProcessReaderResult::Success);
    SH_CHECK(reader.Pid() == 1234);
}

SH_TEST(Win32ProcessReader_AttachByName_CaseInsensitive_Matches) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "Game.EXE"}});
    Win32ProcessReader reader(api);

    SH_CHECK(reader.AttachByName("game.exe") == ProcessReaderResult::Success);
}

SH_TEST(Win32ProcessReader_AttachByName_SubstringDoesNotMatch) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "notgame.exe"}, {5678, "game.exe.bak"}});
    Win32ProcessReader reader(api);

    SH_CHECK(reader.AttachByName("game.exe") == ProcessReaderResult::ProcessNotFound);
}

SH_TEST(Win32ProcessReader_AttachByName_ZeroMatches_ReturnsProcessNotFound) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "other.exe"}});
    Win32ProcessReader reader(api);

    SH_CHECK(reader.AttachByName("game.exe") == ProcessReaderResult::ProcessNotFound);
}

SH_TEST(Win32ProcessReader_AttachByName_MultipleMatches_DoesNotPickArbitraryOne) {
    FakeWin32Api api;
    api.SetProcessList({{1111, "game.exe"}, {2222, "game.exe"}});
    Win32ProcessReader reader(api);

    ProcessReaderResult result = reader.AttachByName("game.exe");

    SH_CHECK(result == ProcessReaderResult::MultipleMatches);
    SH_CHECK(reader.IsAttached() == false);
}

// --- ReadBytes ---

SH_TEST(Win32ProcessReader_ReadBytes_BeforeAttach_ReturnsNotAttached) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);

    std::uint8_t buffer[4] = {};
    SH_CHECK(reader.ReadBytes(0x1000, buffer, sizeof(buffer)) == ProcessReaderResult::NotAttached);
}

SH_TEST(Win32ProcessReader_ReadBytes_FullRead_Succeeds) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    std::uint8_t pattern[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    api.PokeBytes(0x2000, pattern, sizeof(pattern));
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::uint8_t buffer[4] = {};
    ProcessReaderResult result = reader.ReadBytes(0x2000, buffer, sizeof(buffer));

    SH_CHECK(result == ProcessReaderResult::Success);
    SH_CHECK(std::memcmp(buffer, pattern, sizeof(pattern)) == 0);
}

SH_TEST(Win32ProcessReader_ReadBytes_PartialRead_IsNotTreatedAsSuccess) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.ForcePartialReadNext(2); // only 2 of 4 requested bytes

    std::uint8_t buffer[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ProcessReaderResult result = reader.ReadBytes(0x2000, buffer, sizeof(buffer));

    SH_CHECK(result == ProcessReaderResult::PartialRead);
}

SH_TEST(Win32ProcessReader_ReadBytes_ReadFailure_ReturnsReadFailed) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.ForceNextReadToFail();

    std::uint8_t buffer[4] = {};
    SH_CHECK(reader.ReadBytes(0x2000, buffer, sizeof(buffer)) == ProcessReaderResult::ReadFailed);
}

SH_TEST(Win32ProcessReader_ReadBytes_NullDestination_ReturnsInvalidArgument) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    SH_CHECK(reader.ReadBytes(0x2000, nullptr, 4) == ProcessReaderResult::InvalidArgument);
}

SH_TEST(Win32ProcessReader_ReadBytes_AddressPlusSizeOverflow_ReturnsInvalidArgument) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::uint8_t buffer[4] = {};
    std::uintptr_t nearMax = std::numeric_limits<std::uintptr_t>::max() - 1;
    SH_CHECK(reader.ReadBytes(nearMax, buffer, 4) == ProcessReaderResult::InvalidArgument);
}

SH_TEST(Win32ProcessReader_ReadBytes_ZeroSize_SucceedsTriviallyWithoutTouchingBuffer) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::uint8_t buffer[1] = {0x42};
    ProcessReaderResult result = reader.ReadBytes(0x2000, buffer, 0);

    SH_CHECK(result == ProcessReaderResult::Success);
    SH_CHECK(buffer[0] == 0x42); // untouched
}

SH_TEST(Win32ProcessReader_ReadBytes_ProcessExited_ReturnsProcessExitedNotSuccess) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetProcessAlive(1234, false);

    std::uint8_t buffer[4] = {};
    SH_CHECK(reader.ReadBytes(0x2000, buffer, sizeof(buffer)) == ProcessReaderResult::ProcessExited);
}

// --- Liveness ---

SH_TEST(Win32ProcessReader_IsAlive_ReflectsProcessState) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    SH_CHECK(reader.IsAlive());

    api.SetProcessAlive(1234, false);
    SH_CHECK(reader.IsAlive() == false);
}

SH_TEST(Win32ProcessReader_IsAlive_WhenNotAttached_ReturnsFalse) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);
    SH_CHECK(reader.IsAlive() == false);
}

// --- Detach / reattach / handle lifetime ---

SH_TEST(Win32ProcessReader_Detach_ClosesHandleAndClearsState) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    reader.Detach();

    SH_CHECK(reader.IsAttached() == false);
    SH_CHECK(reader.Pid() == 0);
    SH_CHECK(api.CloseCount() == 1);
    SH_CHECK(api.OpenHandleCount() == 0);
}

SH_TEST(Win32ProcessReader_Detach_CalledRepeatedly_IsSafeAndClosesOnlyOnce) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    reader.Detach();
    reader.Detach();
    reader.Detach();

    SH_CHECK(api.CloseCount() == 1);
}

SH_TEST(Win32ProcessReader_AttachDetachReattach_WorksCorrectly) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}, {5678, "other.exe"}});
    Win32ProcessReader reader(api);

    SH_CHECK(reader.AttachByPid(1234) == ProcessReaderResult::Success);
    reader.Detach();
    SH_CHECK(reader.IsAttached() == false);

    SH_CHECK(reader.AttachByPid(5678) == ProcessReaderResult::Success);
    SH_CHECK(reader.Pid() == 5678);
}

SH_TEST(Win32ProcessReader_FailedReattachByPid_PreservesExistingAttachment) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    SH_CHECK(reader.AttachByPid(1234) == ProcessReaderResult::Success);

    api.SetOpenShouldSucceed(false); // subsequent attach attempts fail

    ProcessReaderResult result = reader.AttachByPid(5678);

    SH_CHECK(result == ProcessReaderResult::OpenFailed);
    SH_CHECK(reader.IsAttached());   // still attached...
    SH_CHECK(reader.Pid() == 1234);  // ...to the ORIGINAL process
    SH_CHECK(api.CloseCount() == 0); // original handle was never closed
}

SH_TEST(Win32ProcessReader_FailedReattachByName_MultipleMatches_PreservesExistingAttachment) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    SH_CHECK(reader.AttachByPid(1234) == ProcessReaderResult::Success);

    api.SetProcessList({{1111, "other.exe"}, {2222, "other.exe"}});

    ProcessReaderResult result = reader.AttachByName("other.exe");

    SH_CHECK(result == ProcessReaderResult::MultipleMatches);
    SH_CHECK(reader.IsAttached());
    SH_CHECK(reader.Pid() == 1234);
}

SH_TEST(Win32ProcessReader_ReattachToNewProcess_ClosesOldHandleExactlyOnce) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}, {5678, "other.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    SH_CHECK(reader.AttachByPid(5678) == ProcessReaderResult::Success);

    SH_CHECK(api.CloseCount() == 1);      // the old (1234) handle, closed exactly once
    SH_CHECK(api.OpenHandleCount() == 1); // only the new one remains open
    SH_CHECK(reader.Pid() == 5678);
}

SH_TEST(Win32ProcessReader_Destructor_ClosesHandleExactlyOnce) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    {
        Win32ProcessReader reader(api);
        reader.AttachByPid(1234);
    }
    SH_CHECK(api.CloseCount() == 1);
    SH_CHECK(api.OpenHandleCount() == 0);
}

SH_TEST(Win32ProcessReader_Destructor_WhenNeverAttached_ClosesNothing) {
    FakeWin32Api api;
    { Win32ProcessReader reader(api); }
    SH_CHECK(api.CloseCount() == 0);
}

// --- GetImagePath ---

SH_TEST(Win32ProcessReader_GetImagePath_BeforeAttach_ReturnsNotAttached) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);

    std::filesystem::path path;
    SH_CHECK(reader.GetImagePath(path) == ProcessInspectionResult::NotAttached);
}

SH_TEST(Win32ProcessReader_GetImagePath_UnicodeFullPath_PreservedLosslessly) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    // A non-ASCII path component (Korean "hangul") -- proves the path
    // isn't narrowed/mangled anywhere on the way out.
    std::wstring unicodePath = L"C:\\Users\\\uD55C\uAE00\\game.exe";
    api.SetImagePath(1234, unicodePath);
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    std::filesystem::path path;
    ProcessInspectionResult result = reader.GetImagePath(path);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(path.wstring() == unicodePath);
}

SH_TEST(Win32ProcessReader_GetImagePath_QueryFails_ReturnsImagePathQueryFailed) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetImagePathQueryShouldSucceed(false);

    std::filesystem::path path;
    SH_CHECK(reader.GetImagePath(path) == ProcessInspectionResult::ImagePathQueryFailed);
}

SH_TEST(Win32ProcessReader_GetImagePath_ProcessExited_ReturnsProcessExited) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetProcessAlive(1234, false);

    std::filesystem::path path;
    SH_CHECK(reader.GetImagePath(path) == ProcessInspectionResult::ProcessExited);
}

// --- GetMainModule ---

SH_TEST(Win32ProcessReader_GetMainModule_Success) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetImagePath(1234, L"C:\\Games\\game.exe");
    api.SetModules(1234, {
                              MakeModule("kernel32.dll", L"C:\\Windows\\System32\\kernel32.dll", 0x10000, 0x1000),
                              MakeModule("game.exe", L"C:\\Games\\game.exe", 0x400000, 0x50000),
                          });
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    ProcessInspectionResult result = reader.GetMainModule(module);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(module.name == "game.exe");
    SH_CHECK(module.baseAddress == 0x400000);
    SH_CHECK(module.imageSize == 0x50000);
}

SH_TEST(Win32ProcessReader_GetMainModule_NotAssumedToBeFirstEnumeratedEntry) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetImagePath(1234, L"C:\\Games\\game.exe");
    // game.exe deliberately listed LAST -- proves GetMainModule() doesn't
    // just take modules.front().
    api.SetModules(1234, {
                              MakeModule("kernel32.dll", L"C:\\Windows\\System32\\kernel32.dll", 0x10000, 0x1000),
                              MakeModule("user32.dll", L"C:\\Windows\\System32\\user32.dll", 0x20000, 0x2000),
                              MakeModule("game.exe", L"C:\\Games\\game.exe", 0x400000, 0x50000),
                          });
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.GetMainModule(module) == ProcessInspectionResult::Success);
    SH_CHECK(module.name == "game.exe");
}

SH_TEST(Win32ProcessReader_GetMainModule_PathComparisonIsCaseInsensitive) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetImagePath(1234, L"C:\\Games\\Game.EXE");
    api.SetModules(1234, {MakeModule("game.exe", L"c:\\games\\game.exe", 0x400000, 0x50000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.GetMainModule(module) == ProcessInspectionResult::Success);
}

SH_TEST(Win32ProcessReader_GetMainModule_NoMatchingModule_ReturnsMainModuleNotFound) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetImagePath(1234, L"C:\\Games\\game.exe");
    api.SetModules(1234, {MakeModule("kernel32.dll", L"C:\\Windows\\System32\\kernel32.dll", 0x10000, 0x1000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.GetMainModule(module) == ProcessInspectionResult::MainModuleNotFound);
}

SH_TEST(Win32ProcessReader_GetMainModule_AmbiguousMultipleMatches_FailsClosedNotArbitrary) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetImagePath(1234, L"C:\\Games\\game.exe");
    // Two modules improbably reporting the exact same path -- must not
    // arbitrarily pick one.
    api.SetModules(1234, {
                              MakeModule("game.exe", L"C:\\Games\\game.exe", 0x400000, 0x50000),
                              MakeModule("game.exe", L"C:\\Games\\game.exe", 0x500000, 0x1000),
                          });
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.GetMainModule(module) == ProcessInspectionResult::MainModuleNotFound);
}

// --- FindModuleExact ---

SH_TEST(Win32ProcessReader_FindModuleExact_Success) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("d3d11.dll", L"C:\\Windows\\System32\\d3d11.dll", 0x50000, 0x3000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    ProcessInspectionResult result = reader.FindModuleExact("d3d11.dll", module);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(module.baseAddress == 0x50000);
}

SH_TEST(Win32ProcessReader_FindModuleExact_CaseInsensitive_Matches) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("D3D11.DLL", L"C:\\Windows\\System32\\D3D11.DLL", 0x50000, 0x3000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("d3d11.dll", module) == ProcessInspectionResult::Success);
}

SH_TEST(Win32ProcessReader_FindModuleExact_SubstringDoesNotMatch) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("notd3d11.dll", L"C:\\x\\notd3d11.dll", 0x1000, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("d3d11.dll", module) == ProcessInspectionResult::ModuleNotFound);
}

SH_TEST(Win32ProcessReader_FindModuleExact_ZeroModules_ReturnsModuleNotFound) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("d3d11.dll", module) == ProcessInspectionResult::ModuleNotFound);
}

SH_TEST(Win32ProcessReader_FindModuleExact_MultipleMatches_ReturnsMultipleModules) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {
                              MakeModule("plugin.dll", L"C:\\a\\plugin.dll", 0x1000, 0x100),
                              MakeModule("plugin.dll", L"C:\\b\\plugin.dll", 0x2000, 0x200),
                          });
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("plugin.dll", module) == ProcessInspectionResult::MultipleModules);
}

SH_TEST(Win32ProcessReader_FindModuleExact_BaseAddressZero_ReturnsInvalidModuleRange) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("weird.dll", L"C:\\weird.dll", 0, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("weird.dll", module) == ProcessInspectionResult::InvalidModuleRange);
}

SH_TEST(Win32ProcessReader_FindModuleExact_ImageSizeZero_ReturnsInvalidModuleRange) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("weird.dll", L"C:\\weird.dll", 0x1000, 0)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("weird.dll", module) == ProcessInspectionResult::InvalidModuleRange);
}

SH_TEST(Win32ProcessReader_FindModuleExact_BaseSizeOverflow_ReturnsInvalidModuleRange) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    std::uintptr_t nearMax = std::numeric_limits<std::uintptr_t>::max() - 1;
    api.SetModules(1234, {MakeModule("weird.dll", L"C:\\weird.dll", nearMax, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("weird.dll", module) == ProcessInspectionResult::InvalidModuleRange);
}

SH_TEST(Win32ProcessReader_FindModuleExact_EnumerationFails_ReturnsModuleEnumerationFailed) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetModuleEnumerationShouldSucceed(false);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("anything.dll", module) == ProcessInspectionResult::ModuleEnumerationFailed);
}

SH_TEST(Win32ProcessReader_FindModuleExact_BeforeAttach_ReturnsNotAttached) {
    FakeWin32Api api;
    Win32ProcessReader reader(api);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("anything.dll", module) == ProcessInspectionResult::NotAttached);
}

SH_TEST(Win32ProcessReader_FindModuleExact_ProcessExited_ReturnsProcessExited) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    api.SetModules(1234, {MakeModule("d3d11.dll", L"C:\\d3d11.dll", 0x1000, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);
    api.SetProcessAlive(1234, false);

    ModuleInfo module;
    SH_CHECK(reader.FindModuleExact("d3d11.dll", module) == ProcessInspectionResult::ProcessExited);
}

// --- FindModuleExact: Unicode module-name matching ---
//
// These replace the previous NarrowAsciiApprox-based matching (which
// replaced any character above 0x7F with '?', letting genuinely different
// Unicode module names collide) -- see docs/05-process-access.md.

SH_TEST(Win32ProcessReader_FindModuleExact_UnicodeName_ExactMatch_Succeeds) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    // Korean "hangul module" -- a genuinely non-ASCII module name.
    std::wstring unicodeName = L"\uD55C\uAE00\uBAA8\uB4C8.dll";
    api.SetModules(1234, {MakeModuleW(unicodeName, L"C:\\mods\\" + unicodeName, 0x60000, 0x4000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    // The search term is passed as UTF-8 (this project's narrow-string
    // convention for non-ASCII content) -- "\xed\x95\x9c\xea\xb8\x80\xeb\xaa\xa8\xeb\x93\x88.dll".
    ProcessInspectionResult result =
        reader.FindModuleExact("\xED\x95\x9C\xEA\xB8\x80\xEB\xAA\xA8\xEB\x93\x88.dll", module);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(module.baseAddress == 0x60000);
    // ModuleInfo::name carries the name losslessly as UTF-8 -- not the
    // old '?'-mangled ASCII approximation.
    SH_CHECK(module.name == "\xED\x95\x9C\xEA\xB8\x80\xEB\xAA\xA8\xEB\x93\x88.dll");
}

SH_TEST(Win32ProcessReader_FindModuleExact_UnicodeName_SubstringDoesNotMatch) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    std::wstring fullName = L"\uD55C\uAE00\uBAA8\uB4C8.dll"; // "hangulmodule.dll"
    api.SetModules(1234, {MakeModuleW(fullName, L"C:\\mods\\" + fullName, 0x60000, 0x4000)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    // Missing the ".dll" suffix -- must not match as a substring.
    ProcessInspectionResult result = reader.FindModuleExact("\xED\x95\x9C\xEA\xB8\x80\xEB\xAA\xA8\xEB\x93\x88", module);

    SH_CHECK(result == ProcessInspectionResult::ModuleNotFound);
}

SH_TEST(Win32ProcessReader_FindModuleExact_TwoNamesThatWouldCollideUnderAsciiApproximation_DoNotMatch) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    // Under the old NarrowAsciiApprox scheme, both of these non-ASCII
    // names collapse to the identical narrow string "caf?.dll" (both
    // non-ASCII characters become '?') -- the fixed implementation must
    // treat them as genuinely different modules.
    std::wstring nameA = L"caf\u00E9.dll";  // caf + LATIN SMALL LETTER E WITH ACUTE
    std::wstring nameB = L"caf\u00E8.dll";  // caf + LATIN SMALL LETTER E WITH GRAVE
    api.SetModules(1234, {MakeModuleW(nameA, L"C:\\mods\\a.dll", 0x1000, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    // Search for nameB (UTF-8: "caf\xC3\xA8.dll") -- must NOT match the
    // loaded nameA module, even though both would have collapsed to the
    // same '?'-approximated string under the old buggy scheme.
    ProcessInspectionResult result = reader.FindModuleExact("caf\xC3\xA8.dll", module);

    SH_CHECK(result == ProcessInspectionResult::ModuleNotFound);
}

SH_TEST(Win32ProcessReader_FindModuleExact_UnicodeName_CaseInsensitive_Matches) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    // "CAFÉ.DLL" (uppercase, including the accented letter) as loaded...
    std::wstring loadedName = L"CAF\u00C9.DLL";
    api.SetModules(1234, {MakeModuleW(loadedName, L"C:\\mods\\cafe.dll", 0x1000, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    // ...searched for as lowercase "café.dll" (UTF-8: "caf\xC3\xA9.dll") --
    // must match via Windows-correct case-insensitive comparison.
    ProcessInspectionResult result = reader.FindModuleExact("caf\xC3\xA9.dll", module);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(module.baseAddress == 0x1000);
}

SH_TEST(Win32ProcessReader_FindModuleExact_UnicodeName_DifferentBaseLetterDoesNotMatch) {
    FakeWin32Api api;
    api.SetProcessList({{1234, "game.exe"}});
    // "café.dll" loaded...
    std::wstring loadedName = L"caf\u00E9.dll";
    api.SetModules(1234, {MakeModuleW(loadedName, L"C:\\mods\\cafe.dll", 0x1000, 0x100)});
    Win32ProcessReader reader(api);
    reader.AttachByPid(1234);

    ModuleInfo module;
    // ...searched for as "cafe.dll" (plain 'e', a genuinely different
    // base character, not a case variant of 'é') -- must not match.
    ProcessInspectionResult result = reader.FindModuleExact("cafe.dll", module);

    SH_CHECK(result == ProcessInspectionResult::ModuleNotFound);
}
