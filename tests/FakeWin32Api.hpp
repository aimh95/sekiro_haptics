#pragma once

// Test-only IWin32Api double. Lives under tests/, not
// include/sekiro_haptics/ -- production code (Win32ProcessReader) depends
// only on IWin32Api and never links or includes this type.

#include "sekiro_haptics/process/IWin32Api.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sekiro_haptics::process {

class FakeWin32Api final : public IWin32Api {
public:
    // --- configuration ---

    void SetProcessList(std::vector<ProcessEnumEntry> processes) { processes_ = std::move(processes); }

    void SetOpenShouldSucceed(bool succeed) { openShouldSucceed_ = succeed; }

    void SetProcessAlive(std::uint32_t pid, bool alive) { aliveOverride_[pid] = alive; }

    /// Sets the image path a subsequent GetProcessImagePath() call for
    /// `pid` reports. No entry for a pid means the call fails.
    void SetImagePath(std::uint32_t pid, std::wstring path) { imagePathsByPid_[pid] = std::move(path); }

    void SetModules(std::uint32_t pid, std::vector<ModuleEnumEntry> modules) { modulesByPid_[pid] = std::move(modules); }

    void SetModuleEnumerationShouldSucceed(bool succeed) { moduleEnumerationShouldSucceed_ = succeed; }
    void SetImagePathQueryShouldSucceed(bool succeed) { imagePathQueryShouldSucceed_ = succeed; }

    /// Writes raw bytes into the fake's flat address space (shared across
    /// every opened handle -- tests just use distinct addresses).
    void PokeBytes(std::uintptr_t address, const void* data, std::size_t length) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < length; ++i) {
            memory_[address + i] = bytes[i];
        }
    }

    /// The next ReadProcessMemory() call reports success (true) but only
    /// `actualBytes` bytes read, regardless of the requested size --
    /// simulates a real short/partial read.
    void ForcePartialReadNext(std::size_t actualBytes) { forcedPartialBytes_ = actualBytes; }

    /// The next ReadProcessMemory() call reports failure (false).
    void ForceNextReadToFail() { forceReadFailureNext_ = true; }

    // --- observers ---

    int OpenAttempts() const { return openAttempts_; }
    std::uint32_t LastRequestedAccessMask() const { return lastRequestedAccessMask_; }
    std::uint32_t LastOpenedPid() const { return lastOpenedPid_; }
    int CloseCount() const { return closeCount_; }
    int EnumerateCalls() const { return enumerateCalls_; }

    /// Handles opened but never closed -- should be 0 after a
    /// Win32ProcessReader detaches/is destroyed cleanly (no leak).
    std::size_t OpenHandleCount() const {
        std::size_t count = 0;
        for (const auto& h : handles_) {
            if (!h->closed) {
                ++count;
            }
        }
        return count;
    }

    // --- IWin32Api ---

    std::vector<ProcessEnumEntry> EnumerateProcesses() override {
        ++enumerateCalls_;
        return processes_;
    }

    void* OpenProcess(std::uint32_t desiredAccessMask, std::uint32_t pid) override {
        ++openAttempts_;
        lastRequestedAccessMask_ = desiredAccessMask;
        lastOpenedPid_ = pid;
        if (!openShouldSucceed_) {
            return nullptr;
        }

        auto handle = std::make_unique<FakeHandle>();
        handle->pid = pid;
        FakeHandle* raw = handle.get();
        handles_.push_back(std::move(handle));
        return raw;
    }

    bool ReadProcessMemory(void* handle, std::uintptr_t address, void* buffer, std::size_t size,
                            std::size_t& outBytesRead) override {
        (void)handle;
        if (forceReadFailureNext_) {
            forceReadFailureNext_ = false;
            outBytesRead = 0;
            return false;
        }

        std::size_t toRead = size;
        if (forcedPartialBytes_.has_value()) {
            toRead = *forcedPartialBytes_;
            forcedPartialBytes_.reset();
        }

        auto* bytes = static_cast<std::uint8_t*>(buffer);
        std::size_t actuallyRead = 0;
        for (std::size_t i = 0; i < toRead; ++i) {
            auto it = memory_.find(address + i);
            if (it == memory_.end()) {
                break;
            }
            bytes[i] = it->second;
            ++actuallyRead;
        }
        outBytesRead = actuallyRead;
        return true;
    }

    bool IsProcessAlive(void* handle) override {
        auto* h = static_cast<FakeHandle*>(handle);
        if (h == nullptr || h->closed) {
            return false;
        }
        // Looked up live by pid (not cached at OpenProcess() time) so
        // SetProcessAlive() takes effect even after a handle is already
        // open -- matching how a real process's liveness can change at any
        // time after attach.
        auto it = aliveOverride_.find(h->pid);
        return it == aliveOverride_.end() || it->second;
    }

    void CloseHandle(void* handle) override {
        ++closeCount_;
        auto* h = static_cast<FakeHandle*>(handle);
        if (h != nullptr) {
            h->closed = true;
        }
    }

    bool GetProcessImagePath(void* handle, std::wstring& outPath) override {
        ++imagePathQueries_;
        if (!imagePathQueryShouldSucceed_) {
            return false;
        }
        auto* h = static_cast<FakeHandle*>(handle);
        if (h == nullptr) {
            return false;
        }
        auto it = imagePathsByPid_.find(h->pid);
        if (it == imagePathsByPid_.end()) {
            return false;
        }
        outPath = it->second;
        return true;
    }

    bool EnumerateModules(std::uint32_t pid, std::vector<ModuleEnumEntry>& outModules) override {
        ++moduleEnumerationCalls_;
        if (!moduleEnumerationShouldSucceed_) {
            return false;
        }
        auto it = modulesByPid_.find(pid);
        outModules = it == modulesByPid_.end() ? std::vector<ModuleEnumEntry>{} : it->second;
        return true;
    }

    int ImagePathQueries() const { return imagePathQueries_; }
    int ModuleEnumerationCalls() const { return moduleEnumerationCalls_; }

private:
    struct FakeHandle {
        std::uint32_t pid = 0;
        bool closed = false;
    };

    std::vector<ProcessEnumEntry> processes_;
    bool openShouldSucceed_ = true;
    std::unordered_map<std::uint32_t, bool> aliveOverride_;
    std::unordered_map<std::uintptr_t, std::uint8_t> memory_;
    std::optional<std::size_t> forcedPartialBytes_;
    bool forceReadFailureNext_ = false;
    std::unordered_map<std::uint32_t, std::wstring> imagePathsByPid_;
    std::unordered_map<std::uint32_t, std::vector<ModuleEnumEntry>> modulesByPid_;
    bool moduleEnumerationShouldSucceed_ = true;
    bool imagePathQueryShouldSucceed_ = true;

    int openAttempts_ = 0;
    std::uint32_t lastRequestedAccessMask_ = 0;
    std::uint32_t lastOpenedPid_ = 0;
    int closeCount_ = 0;
    int enumerateCalls_ = 0;
    int imagePathQueries_ = 0;
    int moduleEnumerationCalls_ = 0;
    std::vector<std::unique_ptr<FakeHandle>> handles_;
};

} // namespace sekiro_haptics::process
