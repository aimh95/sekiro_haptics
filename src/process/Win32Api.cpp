// Real IWin32Api implementation. This is the only file in this module that
// includes <windows.h>/<tlhelp32.h> or calls an actual Win32 API -- keeping
// that isolated here is what lets Win32ProcessReader.cpp (and its tests) be
// exercised entirely through the IWin32Api seam.

#include "sekiro_haptics/process/IWin32Api.hpp"
#include "sekiro_haptics/process/Win32ProcessReader.hpp"

#include <windows.h>

#include <tlhelp32.h>

#include <array>

namespace sekiro_haptics::process {

namespace {

class RealWin32ApiImpl final : public IWin32Api {
public:
    std::vector<ProcessEnumEntry> EnumerateProcesses() override {
        std::vector<ProcessEnumEntry> result;

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return result;
        }

        PROCESSENTRY32 entry;
        entry.dwSize = sizeof(entry);
        if (Process32First(snapshot, &entry)) {
            do {
                ProcessEnumEntry e;
                e.pid = static_cast<std::uint32_t>(entry.th32ProcessID);
                e.exeName = entry.szExeFile;
                result.push_back(std::move(e));
            } while (Process32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);

        return result;
    }

    void* OpenProcess(std::uint32_t desiredAccessMask, std::uint32_t pid) override {
        HANDLE handle = ::OpenProcess(desiredAccessMask, FALSE, pid);
        return handle == nullptr ? nullptr : static_cast<void*>(handle);
    }

    bool ReadProcessMemory(void* handle, std::uintptr_t address, void* buffer, std::size_t size,
                            std::size_t& outBytesRead) override {
        SIZE_T bytesRead = 0;
        BOOL ok = ::ReadProcessMemory(static_cast<HANDLE>(handle), reinterpret_cast<LPCVOID>(address), buffer, size,
                                       &bytesRead);
        outBytesRead = static_cast<std::size_t>(bytesRead);
        return ok != FALSE;
    }

    bool IsProcessAlive(void* handle) override {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(static_cast<HANDLE>(handle), &exitCode)) {
            return false;
        }
        return exitCode == STILL_ACTIVE;
    }

    void CloseHandle(void* handle) override {
        if (handle != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(handle));
        }
    }

    bool GetProcessImagePath(void* handle, std::wstring& outPath) override {
        // A generous, long-path-safe buffer -- QueryFullProcessImageNameW
        // fails (rather than truncating) if the real path doesn't fit, so
        // a failure here is always a genuine failure, never silent
        // truncation.
        std::array<wchar_t, 32768> buffer{};
        DWORD size = static_cast<DWORD>(buffer.size());
        if (!QueryFullProcessImageNameW(static_cast<HANDLE>(handle), 0, buffer.data(), &size)) {
            return false;
        }
        outPath.assign(buffer.data(), size);
        return true;
    }

    bool EnumerateModules(std::uint32_t pid, std::vector<ModuleEnumEntry>& outModules) override {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }

        MODULEENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                ModuleEnumEntry m;
                m.name = entry.szModule;
                m.path = entry.szExePath;
                m.baseAddress = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                m.imageSize = entry.modBaseSize;
                outModules.push_back(std::move(m));
            } while (Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);

        return true;
    }
};

} // namespace

IWin32Api& Win32ProcessReader::RealWin32Api() {
    static RealWin32ApiImpl instance;
    return instance;
}

} // namespace sekiro_haptics::process
