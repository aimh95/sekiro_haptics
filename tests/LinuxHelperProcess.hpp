#pragma once

// Minimal POSIX child-process launcher with redirected stdin/stdout, for
// tests/linux_signal_probe_helper.cpp (SEK-PROBE-001C-LINUX-E2E). The
// Linux counterpart to tests/HelperProcess.hpp (which is Win32-only) --
// test-only plumbing, never part of production code. Only ever launches a
// process this same test binary directly forks (never attaches to an
// arbitrary existing system process). No sleep anywhere: readiness and
// exit are both real pipe/waitpid synchronization.

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sekiro_haptics::process {

class LinuxHelperProcess {
public:
    ~LinuxHelperProcess() {
        if (stdoutReadFd_ >= 0) {
            close(stdoutReadFd_);
        }
        if (stdinWriteFd_ >= 0) {
            close(stdinWriteFd_);
        }
        // Best-effort reap if the caller never called WaitForExit() --
        // never blocks (WNOHANG), just avoids leaking a zombie if a test
        // fails before reaching its own explicit exit-and-wait step.
        if (childPid_ > 0 && !reaped_) {
            int status = 0;
            waitpid(childPid_, &status, WNOHANG);
        }
    }

    LinuxHelperProcess(const LinuxHelperProcess&) = delete;
    LinuxHelperProcess& operator=(const LinuxHelperProcess&) = delete;
    LinuxHelperProcess() = default;

    /// Forks and execs `exePath` with `args` (argv[1..], NOT including
    /// argv[0]), with the child's stdin/stdout redirected to pipes this
    /// object owns the parent ends of.
    bool Start(const std::string& exePath, const std::vector<std::string>& args) {
        int stdoutPipe[2]; // [0]=read (parent), [1]=write (child)
        int stdinPipe[2];  // [0]=read (child), [1]=write (parent)
        if (pipe(stdoutPipe) != 0) {
            return false;
        }
        if (pipe(stdinPipe) != 0) {
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            close(stdinPipe[0]);
            close(stdinPipe[1]);
            return false;
        }

        if (pid == 0) {
            // Child.
            dup2(stdoutPipe[1], STDOUT_FILENO);
            dup2(stdinPipe[0], STDIN_FILENO);
            close(stdoutPipe[0]);
            close(stdoutPipe[1]);
            close(stdinPipe[0]);
            close(stdinPipe[1]);

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(exePath.c_str()));
            for (const std::string& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            execv(exePath.c_str(), argv.data());
            _exit(127); // execv only returns on failure
        }

        // Parent: these ends belong to the child now.
        close(stdoutPipe[1]);
        close(stdinPipe[0]);

        childPid_ = pid;
        stdoutReadFd_ = stdoutPipe[0];
        stdinWriteFd_ = stdinPipe[1];
        return true;
    }

    /// Blocking line read from the child's stdout. This IS the
    /// synchronization for "the helper is ready": nothing is returned
    /// until the child has actually written (and flushed) its line.
    /// Returns false on EOF with nothing buffered (child closed its
    /// stdout, e.g. because it exited, without ever writing a full line).
    bool ReadLine(std::string& outLine) {
        outLine.clear();
        char c = 0;
        while (true) {
            ssize_t n = read(stdoutReadFd_, &c, 1);
            if (n <= 0) {
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
        ssize_t ignored = write(stdinWriteFd_, message, sizeof(message) - 1);
        (void)ignored;
    }

    /// Sends one command line (e.g. "DAMAGE") and blocks until the
    /// helper's "ACK <command> ..." acknowledgement line comes back --
    /// real pipe synchronization for "the command has been applied," no
    /// sleep on either side. Returns false if the write failed or the
    /// expected "ACK <command>" prefix wasn't read back.
    bool SendCommandAndWaitForAck(const std::string& command) {
        std::string message = command + "\n";
        ssize_t written = write(stdinWriteFd_, message.data(), message.size());
        if (written != static_cast<ssize_t>(message.size())) {
            return false;
        }
        std::string ack;
        if (!ReadLine(ack)) {
            return false;
        }
        std::string expectedPrefix = "ACK " + command;
        return ack.compare(0, expectedPrefix.size(), expectedPrefix) == 0;
    }

    /// Blocking wait for the child to actually terminate -- real
    /// process-exit synchronization via waitpid(), not polling/sleep. Our
    /// own helper always exits promptly after EXIT, so no timeout
    /// enforcement is needed (mirrors HelperProcess.hpp's
    /// WaitForSingleObject, which also blocks without polling).
    bool WaitForExit(int* outExitCode = nullptr) {
        if (childPid_ <= 0) {
            return false;
        }
        int status = 0;
        pid_t result = waitpid(childPid_, &status, 0);
        if (result != childPid_) {
            return false;
        }
        reaped_ = true;
        if (outExitCode != nullptr) {
            *outExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return true;
    }

    pid_t Pid() const { return childPid_; }

private:
    pid_t childPid_ = -1;
    int stdoutReadFd_ = -1;
    int stdinWriteFd_ = -1;
    bool reaped_ = false;
};

inline std::uint64_t ParseLinuxHelperKeyValue(const std::string& token) {
    auto pos = token.find('=');
    return std::stoull(token.substr(pos + 1));
}

} // namespace sekiro_haptics::process
