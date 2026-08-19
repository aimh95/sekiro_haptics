#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "LinuxChildProcessReader.hpp"

#include <signal.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace sekiro_haptics::process {

LinuxChildProcessReader::LinuxChildProcessReader(pid_t authorizedPid, std::uintptr_t arenaBase, std::size_t arenaSize)
    : authorizedPid_(authorizedPid), arenaBase_(arenaBase), arenaSize_(arenaSize) {}

ProcessReaderResult LinuxChildProcessReader::AttachByPid(std::uint32_t pid) {
    if (static_cast<pid_t>(pid) != authorizedPid_) {
        // Structural refusal -- this adapter only ever attaches to the
        // exact child it was constructed with, never an arbitrary PID.
        return ProcessReaderResult::ProcessNotFound;
    }
    attachedPid_ = authorizedPid_;
    return ProcessReaderResult::Success;
}

ProcessReaderResult LinuxChildProcessReader::AttachByName(const std::string&) {
    return ProcessReaderResult::ProcessNotFound;
}

void LinuxChildProcessReader::Detach() {
    attachedPid_ = -1;
}

bool LinuxChildProcessReader::IsAttached() const {
    return attachedPid_ > 0;
}

bool LinuxChildProcessReader::IsAlive() const {
    if (attachedPid_ <= 0) {
        return false;
    }
    return kill(attachedPid_, 0) == 0;
}

std::uint32_t LinuxChildProcessReader::Pid() const {
    return attachedPid_ > 0 ? static_cast<std::uint32_t>(attachedPid_) : 0;
}

ProcessReaderResult LinuxChildProcessReader::ReadBytes(std::uintptr_t address, void* destination,
                                                        std::size_t requestedSize) {
    if (attachedPid_ <= 0) {
        return ProcessReaderResult::NotAttached;
    }
    if (requestedSize == 0) {
        return ProcessReaderResult::Success;
    }
    if (destination == nullptr) {
        return ProcessReaderResult::InvalidArgument;
    }

    struct iovec local{};
    local.iov_base = destination;
    local.iov_len = requestedSize;
    struct iovec remote{};
    remote.iov_base = reinterpret_cast<void*>(address);
    remote.iov_len = requestedSize;

    errno = 0;
    ssize_t n = process_vm_readv(attachedPid_, &local, 1, &remote, 1, 0);
    if (n < 0) {
        lastErrno_ = errno;
        if (lastErrno_ == ESRCH) {
            return ProcessReaderResult::ProcessExited;
        }
        return ProcessReaderResult::ReadFailed;
    }
    lastErrno_ = 0;
    if (static_cast<std::size_t>(n) < requestedSize) {
        return ProcessReaderResult::PartialRead;
    }
    return ProcessReaderResult::Success;
}

ProcessInspectionResult LinuxChildProcessReader::GetImagePath(std::filesystem::path& outPath) {
    if (attachedPid_ <= 0) {
        return ProcessInspectionResult::NotAttached;
    }
    std::string exeLink = "/proc/" + std::to_string(attachedPid_) + "/exe";
    char buffer[4096];
    ssize_t len = readlink(exeLink.c_str(), buffer, sizeof(buffer) - 1);
    if (len < 0) {
        return ProcessInspectionResult::ImagePathQueryFailed;
    }
    buffer[len] = '\0';
    outPath = std::filesystem::path(buffer);
    return ProcessInspectionResult::Success;
}

ProcessInspectionResult LinuxChildProcessReader::GetMainModule(ModuleInfo&) {
    return ProcessInspectionResult::MainModuleNotFound;
}

ProcessInspectionResult LinuxChildProcessReader::FindModuleExact(const std::string&, ModuleInfo&) {
    return ProcessInspectionResult::ModuleNotFound;
}

namespace {

struct ParsedMapsLine {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    bool readable = false;
    bool isPrivate = false;
};

bool ParseMapsLine(const std::string& line, ParsedMapsLine& out) {
    // "start-end perms offset dev inode [pathname]"
    std::istringstream iss(line);
    std::string rangeToken, permsToken;
    if (!(iss >> rangeToken >> permsToken)) {
        return false;
    }
    auto dashPos = rangeToken.find('-');
    if (dashPos == std::string::npos) {
        return false;
    }
    try {
        out.start = static_cast<std::uintptr_t>(std::stoull(rangeToken.substr(0, dashPos), nullptr, 16));
        out.end = static_cast<std::uintptr_t>(std::stoull(rangeToken.substr(dashPos + 1), nullptr, 16));
    } catch (...) {
        return false;
    }
    if (permsToken.size() < 4) {
        return false;
    }
    out.readable = permsToken[0] == 'r';
    out.isPrivate = permsToken[3] == 'p';
    return true;
}

} // namespace

MemoryMapResult LinuxChildProcessReader::EnumerateReadableRegions(std::vector<ProcessMemoryRegion>& outRegions) const {
    if (attachedPid_ <= 0) {
        return MemoryMapResult::NotAttached;
    }

    std::uintptr_t arenaEnd = arenaBase_ + arenaSize_;
    if (arenaEnd < arenaBase_) {
        return MemoryMapResult::AddressOverflow;
    }

    std::string mapsPath = "/proc/" + std::to_string(attachedPid_) + "/maps";
    std::ifstream in(mapsPath);
    if (!in.is_open()) {
        // Most commonly: the process has already exited (its /proc entry
        // is gone) -- kill(pid,0) is the authoritative liveness check, but
        // an unopenable maps file for a still-numbered PID is itself
        // strong evidence of exit, not an unrelated enumeration failure.
        return kill(attachedPid_, 0) == 0 ? MemoryMapResult::EnumerationFailed : MemoryMapResult::ProcessExited;
    }

    std::string line;
    while (std::getline(in, line)) {
        ParsedMapsLine parsed;
        if (!ParseMapsLine(line, parsed)) {
            continue;
        }
        if (parsed.start <= arenaBase_ && parsed.end >= arenaEnd && parsed.readable && parsed.isPrivate) {
            ProcessMemoryRegion region;
            region.baseAddress = arenaBase_;
            region.sizeBytes = arenaSize_;
            region.kind = MemoryRegionKind::Private;
            region.committed = true;
            region.readable = true;
            outRegions.clear();
            outRegions.push_back(region);
            return MemoryMapResult::Success;
        }
    }

    return MemoryMapResult::EnumerationFailed;
}

} // namespace sekiro_haptics::process
