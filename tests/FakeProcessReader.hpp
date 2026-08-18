#pragma once

// Test-only IProcessReader double for AobScanner/RipRelative tests. Lives
// under tests/, never included by production code. Simpler than routing
// through Win32ProcessReader + FakeWin32Api for these tests, since
// ScanProcessRange/ResolveRipRelativeFromProcess only need IProcessReader
// itself, not attach/module-enumeration semantics.

#include "sekiro_haptics/process/IProcessReader.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sekiro_haptics::process {

class FakeProcessReader final : public IProcessReader {
public:
    void SetAttached(bool attached) { attached_ = attached; }
    void SetAlive(bool alive) { alive_ = alive; }

    void PokeBytes(std::uintptr_t address, const void* data, std::size_t length) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < length; ++i) {
            memory_[address + i] = bytes[i];
        }
    }

    /// The Nth ReadBytes() call (0-based) reports a hard failure.
    void FailReadAtCall(int callIndex) { failReadAtCall_ = callIndex; }

    /// Like FailReadAtCall(), but for more than one call index at once --
    /// e.g. a coalesced read AND one specific per-address fallback read
    /// within the same test.
    void FailReadAtCalls(std::vector<int> callIndices) { failReadAtCalls_ = std::move(callIndices); }

    /// The Nth ReadBytes() call (0-based) reports ProcessExited, as if the
    /// process disappeared partway through a multi-call scan (distinct
    /// from SetAlive(false), which makes every call fail from the start).
    void FailAsProcessExitedAtCall(int callIndex) { processExitedAtCall_ = callIndex; }

    /// The Nth ReadBytes() call (0-based) reports success but only
    /// `actualBytes` bytes read.
    void ForcePartialReadAtCall(int callIndex, std::size_t actualBytes) {
        partialReadAtCall_ = {callIndex, actualBytes};
    }

    int ReadCalls() const { return readCalls_; }

    // --- IProcessReader ---

    ProcessReaderResult AttachByPid(std::uint32_t pid) override {
        pid_ = pid;
        attached_ = true;
        return ProcessReaderResult::Success;
    }

    ProcessReaderResult AttachByName(const std::string&) override {
        attached_ = true;
        return ProcessReaderResult::Success;
    }

    void Detach() override {
        attached_ = false;
        pid_ = 0;
    }

    bool IsAttached() const override { return attached_; }
    bool IsAlive() const override { return attached_ && alive_; }
    std::uint32_t Pid() const override { return pid_; }

    ProcessReaderResult ReadBytes(std::uintptr_t address, void* destination, std::size_t requestedSize) override {
        int callIndex = readCalls_++;
        if (!attached_) {
            return ProcessReaderResult::NotAttached;
        }
        if (!alive_) {
            return ProcessReaderResult::ProcessExited;
        }
        if (failReadAtCall_.has_value() && *failReadAtCall_ == callIndex) {
            return ProcessReaderResult::ReadFailed;
        }
        if (failReadAtCalls_.has_value() &&
            std::find(failReadAtCalls_->begin(), failReadAtCalls_->end(), callIndex) != failReadAtCalls_->end()) {
            return ProcessReaderResult::ReadFailed;
        }
        if (processExitedAtCall_.has_value() && *processExitedAtCall_ == callIndex) {
            return ProcessReaderResult::ProcessExited;
        }

        std::size_t toRead = requestedSize;
        bool partial = false;
        if (partialReadAtCall_.has_value() && partialReadAtCall_->first == callIndex) {
            toRead = partialReadAtCall_->second;
            partial = true;
        }

        auto* bytes = static_cast<std::uint8_t*>(destination);
        for (std::size_t i = 0; i < toRead; ++i) {
            auto it = memory_.find(address + i);
            bytes[i] = it != memory_.end() ? it->second : 0;
        }
        return partial ? ProcessReaderResult::PartialRead : ProcessReaderResult::Success;
    }

private:
    bool attached_ = true;
    bool alive_ = true;
    std::uint32_t pid_ = 1;
    std::unordered_map<std::uintptr_t, std::uint8_t> memory_;
    std::optional<int> failReadAtCall_;
    std::optional<std::vector<int>> failReadAtCalls_;
    std::optional<int> processExitedAtCall_;
    std::optional<std::pair<int, std::size_t>> partialReadAtCall_;
    int readCalls_ = 0;
};

} // namespace sekiro_haptics::process
