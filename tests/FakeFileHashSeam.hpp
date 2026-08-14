#pragma once

// Test-only IFileReader/ICryptoApi doubles for ExecutableIdentity's
// internal file-read/hash seam (FileHashSeam.hpp). Lives under tests/,
// never included by production code -- mirrors FakeWin32Api.hpp's role
// for Win32ProcessReader.

#include "sekiro_haptics/process/FileHashSeam.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace sekiro_haptics::process {

// A single in-memory "file" -- one Open()/Close() pair active at a time,
// which is all these tests need.
class FakeFileReader final : public IFileReader {
public:
    void SetContent(std::vector<std::uint8_t> content) { content_ = std::move(content); }
    void SetContent(const std::string& content) { content_.assign(content.begin(), content.end()); }

    void SetOpenShouldSucceed(bool succeed) { openShouldSucceed_ = succeed; }
    void SetSizeQueryShouldSucceed(bool succeed) { sizeQueryShouldSucceed_ = succeed; }

    /// Overrides the size GetSize() reports, independent of content_'s
    /// actual length -- used to deterministically simulate a short read
    /// (report a size larger than what Read() will ever actually deliver).
    void SetReportedSize(std::uint64_t size) { reportedSize_ = size; }

    /// The Nth Read() call (0-based) reports a hard failure.
    void FailReadAtCall(int callIndex) { failReadAtCall_ = callIndex; }

    int OpenCalls() const { return openCalls_; }
    int CloseCalls() const { return closeCalls_; }
    int ReadCalls() const { return readCalls_; }
    bool IsCurrentlyOpen() const { return isOpen_; }

    void* Open(const std::filesystem::path&) override {
        ++openCalls_;
        if (!openShouldSucceed_) {
            return nullptr;
        }
        position_ = 0;
        readCallIndexAtOpen_ = readCalls_;
        isOpen_ = true;
        return this;
    }

    bool GetSize(void*, std::uint64_t& outSize) override {
        if (!sizeQueryShouldSucceed_) {
            return false;
        }
        outSize = reportedSize_.value_or(static_cast<std::uint64_t>(content_.size()));
        return true;
    }

    bool Read(void*, void* buffer, std::size_t bufferSize, std::size_t& outBytesRead) override {
        int callIndex = readCalls_ - readCallIndexAtOpen_;
        ++readCalls_;
        if (failReadAtCall_.has_value() && *failReadAtCall_ == callIndex) {
            outBytesRead = 0;
            return false;
        }
        std::size_t remaining = position_ < content_.size() ? content_.size() - position_ : 0;
        std::size_t toCopy = std::min(bufferSize, remaining);
        if (toCopy > 0) {
            std::memcpy(buffer, content_.data() + position_, toCopy);
        }
        position_ += toCopy;
        outBytesRead = toCopy;
        return true;
    }

    void Close(void*) override {
        ++closeCalls_;
        isOpen_ = false;
    }

private:
    std::vector<std::uint8_t> content_;
    std::optional<std::uint64_t> reportedSize_;
    bool openShouldSucceed_ = true;
    bool sizeQueryShouldSucceed_ = true;
    std::optional<int> failReadAtCall_;

    std::size_t position_ = 0;
    int openCalls_ = 0;
    int closeCalls_ = 0;
    int readCalls_ = 0;
    int readCallIndexAtOpen_ = 0;
    bool isOpen_ = false;
};

// A single in-memory "hash session" -- one alg/hash pair active at a time.
class FakeCryptoApi final : public ICryptoApi {
public:
    void SetOpenAlgorithmProviderShouldSucceed(bool succeed) { openAlgShouldSucceed_ = succeed; }
    void SetGetHashObjectLengthShouldSucceed(bool succeed) { getLengthShouldSucceed_ = succeed; }
    void SetCreateHashShouldSucceed(bool succeed) { createHashShouldSucceed_ = succeed; }
    void SetFinishHashShouldSucceed(bool succeed) { finishShouldSucceed_ = succeed; }

    /// The Nth HashData() call (0-based, per hash session) reports failure.
    void FailHashDataAtCall(int callIndex) { failHashDataAtCall_ = callIndex; }

    int OpenAlgorithmProviderCalls() const { return openAlgCalls_; }
    int CloseAlgorithmProviderCalls() const { return closeAlgCalls_; }
    int CreateHashCalls() const { return createHashCalls_; }
    int DestroyHashCalls() const { return destroyHashCalls_; }
    bool AlgorithmProviderStillOpen() const { return algOpen_; }
    bool HashStillOpen() const { return hashOpen_; }

    void* OpenAlgorithmProvider() override {
        ++openAlgCalls_;
        if (!openAlgShouldSucceed_) {
            return nullptr;
        }
        algOpen_ = true;
        return this;
    }

    bool GetHashObjectLength(void*, std::size_t& outLength) override {
        if (!getLengthShouldSucceed_) {
            return false;
        }
        outLength = 128;
        return true;
    }

    void* CreateHash(void*, void*, std::size_t) override {
        ++createHashCalls_;
        if (!createHashShouldSucceed_) {
            return nullptr;
        }
        hashOpen_ = true;
        hashDataCallCount_ = 0;
        return this;
    }

    bool HashData(void*, const void*, std::size_t) override {
        int callIndex = hashDataCallCount_++;
        if (failHashDataAtCall_.has_value() && *failHashDataAtCall_ == callIndex) {
            return false;
        }
        return true;
    }

    bool FinishHash(void*, std::uint8_t* outDigest32) override {
        if (!finishShouldSucceed_) {
            return false;
        }
        std::memset(outDigest32, 0xAB, 32);
        return true;
    }

    void DestroyHash(void*) override {
        ++destroyHashCalls_;
        hashOpen_ = false;
    }

    void CloseAlgorithmProvider(void*) override {
        ++closeAlgCalls_;
        algOpen_ = false;
    }

private:
    bool openAlgShouldSucceed_ = true;
    bool getLengthShouldSucceed_ = true;
    bool createHashShouldSucceed_ = true;
    bool finishShouldSucceed_ = true;
    std::optional<int> failHashDataAtCall_;

    int openAlgCalls_ = 0;
    int closeAlgCalls_ = 0;
    int createHashCalls_ = 0;
    int destroyHashCalls_ = 0;
    int hashDataCallCount_ = 0;
    bool algOpen_ = false;
    bool hashOpen_ = false;
};

} // namespace sekiro_haptics::process
