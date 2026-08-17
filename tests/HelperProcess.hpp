#pragma once

// Minimal child-process launcher with redirected stdin/stdout, shared by
// every integration test that drives tests/process_reader_helper_main.cpp
// as a genuinely separate process (test_win32_process_reader_integration.cpp,
// test_aob_scanner_integration.cpp). Test-only plumbing -- production code
// never creates processes or pipes, only opens/reads a process that
// already exists. No sleep is used anywhere here: readiness and exit are
// both real pipe/handle synchronization.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class HelperProcess {
public:
    ~HelperProcess() {
        if (stdoutRead_ != nullptr) CloseHandle(stdoutRead_);
        if (stdinWrite_ != nullptr) CloseHandle(stdinWrite_);
        if (threadHandle_ != nullptr) CloseHandle(threadHandle_);
        if (processHandle_ != nullptr) CloseHandle(processHandle_);
    }

    bool Start(const std::string& exePath) {
        SECURITY_ATTRIBUTES saAttr{};
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = TRUE;
        saAttr.lpSecurityDescriptor = nullptr;

        HANDLE childStdoutRead = nullptr;
        HANDLE childStdoutWrite = nullptr;
        HANDLE childStdinRead = nullptr;
        HANDLE childStdinWrite = nullptr;

        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &saAttr, 0)) {
            return false;
        }
        if (!SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }
        if (!CreatePipe(&childStdinRead, &childStdinWrite, &saAttr, 0)) {
            return false;
        }
        if (!SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }

        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.hStdOutput = childStdoutWrite;
        startupInfo.hStdError = childStdoutWrite;
        startupInfo.hStdInput = childStdinRead;
        startupInfo.dwFlags |= STARTF_USESTDHANDLES;

        PROCESS_INFORMATION processInfo{};
        std::string commandLine = "\"" + exePath + "\"";
        std::vector<char> commandLineBuf(commandLine.begin(), commandLine.end());
        commandLineBuf.push_back('\0');

        BOOL created = CreateProcessA(nullptr, commandLineBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                       &startupInfo, &processInfo);

        // These ends belong to the child now; the parent must not keep them
        // open, or reads/writes on the parent's ends can hang forever.
        CloseHandle(childStdoutWrite);
        CloseHandle(childStdinRead);

        if (!created) {
            CloseHandle(childStdoutRead);
            CloseHandle(childStdinWrite);
            return false;
        }

        processHandle_ = processInfo.hProcess;
        threadHandle_ = processInfo.hThread;
        childPid_ = processInfo.dwProcessId;
        stdoutRead_ = childStdoutRead;
        stdinWrite_ = childStdinWrite;
        return true;
    }

    /// Blocking line read from the child's stdout. This IS the
    /// synchronization for "the helper is ready": nothing is returned
    /// until the child has actually written (and flushed) its line.
    bool ReadLine(std::string& outLine) {
        outLine.clear();
        char c = 0;
        DWORD bytesRead = 0;
        while (true) {
            BOOL ok = ReadFile(stdoutRead_, &c, 1, &bytesRead, nullptr);
            if (!ok || bytesRead == 0) {
                return !outLine.empty();
            }
            if (c == '\n') {
                if (!outLine.empty() && outLine.back() == '\r') {
                    outLine.pop_back();
                }
                return true;
            }
            outLine.push_back(c);
        }
    }

    void SendExit() {
        const char message[] = "EXIT\n";
        DWORD written = 0;
        WriteFile(stdinWrite_, message, static_cast<DWORD>(sizeof(message) - 1), &written, nullptr);
    }

    /// Blocking wait (bounded by `timeoutMs`) for the child to actually
    /// terminate -- real process-exit synchronization, not polling/sleep.
    bool WaitForExit(DWORD timeoutMs) {
        return processHandle_ != nullptr && WaitForSingleObject(processHandle_, timeoutMs) == WAIT_OBJECT_0;
    }

    std::uint32_t Pid() const { return childPid_; }

    /// Sends one command line (e.g. "INCREMENT") and blocks until the
    /// helper's "OK" acknowledgement line comes back -- real pipe
    /// synchronization for "the command has been applied," no sleep on
    /// either side. Returns false if the write failed or the expected "OK"
    /// line wasn't read back.
    bool SendCommandAndWaitForAck(const std::string& command) {
        std::string message = command + "\n";
        DWORD written = 0;
        if (!WriteFile(stdinWrite_, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
            return false;
        }
        std::string ack;
        return ReadLine(ack) && ack == "OK";
    }

private:
    HANDLE processHandle_ = nullptr;
    HANDLE threadHandle_ = nullptr;
    HANDLE stdoutRead_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    std::uint32_t childPid_ = 0;
};

inline std::uint64_t ParseHelperKeyValue(const std::string& token) {
    auto pos = token.find('=');
    return std::stoull(token.substr(pos + 1));
}

} // namespace sekiro_haptics::process
