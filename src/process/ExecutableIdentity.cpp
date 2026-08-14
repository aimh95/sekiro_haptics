#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/FileHashSeam.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <vector>

namespace sekiro_haptics::process {

namespace {

constexpr std::size_t kReadChunkBytes = 64 * 1024;

// Real IFileReader: a single CreateFileW handle serves both the size
// query and the full read for one Open()/Close() pair, opened with
// FILE_SHARE_READ only -- see docs/05-process-access.md for the exact
// stability contract this sharing mode provides (and its limits).
class Win32FileReader final : public IFileReader {
public:
    void* Open(const std::filesystem::path& path) override {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle == INVALID_HANDLE_VALUE ? nullptr : static_cast<void*>(handle);
    }

    bool GetSize(void* handle, std::uint64_t& outSize) override {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(static_cast<HANDLE>(handle), &size)) {
            return false;
        }
        outSize = static_cast<std::uint64_t>(size.QuadPart);
        return true;
    }

    bool Read(void* handle, void* buffer, std::size_t bufferSize, std::size_t& outBytesRead) override {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(handle), buffer, static_cast<DWORD>(bufferSize), &bytesRead, nullptr);
        outBytesRead = static_cast<std::size_t>(bytesRead);
        return ok != FALSE;
    }

    void Close(void* handle) override {
        if (handle != nullptr) {
            CloseHandle(static_cast<HANDLE>(handle));
        }
    }
};

// Real ICryptoApi: a thin wrapper over BCrypt's SHA-256, split into its
// five distinct steps (open provider, query object length, create hash,
// update, finish) so each can be independently exercised/failed by a test
// through the seam -- see FileHashSeam.hpp.
class Win32CryptoApi final : public ICryptoApi {
public:
    void* OpenAlgorithmProvider() override {
        BCRYPT_ALG_HANDLE handle = nullptr;
        NTSTATUS status = BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        return status >= 0 ? static_cast<void*>(handle) : nullptr;
    }

    bool GetHashObjectLength(void* algHandle, std::size_t& outLength) override {
        DWORD objectLength = 0;
        ULONG resultSize = 0;
        NTSTATUS status = BCryptGetProperty(static_cast<BCRYPT_ALG_HANDLE>(algHandle), BCRYPT_OBJECT_LENGTH,
                                             reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                                             &resultSize, 0);
        if (status < 0) {
            return false;
        }
        outLength = static_cast<std::size_t>(objectLength);
        return true;
    }

    void* CreateHash(void* algHandle, void* hashObjectBuffer, std::size_t hashObjectBufferLength) override {
        BCRYPT_HASH_HANDLE handle = nullptr;
        NTSTATUS status = BCryptCreateHash(static_cast<BCRYPT_ALG_HANDLE>(algHandle), &handle,
                                            static_cast<PUCHAR>(hashObjectBuffer),
                                            static_cast<ULONG>(hashObjectBufferLength), nullptr, 0, 0);
        return status >= 0 ? static_cast<void*>(handle) : nullptr;
    }

    bool HashData(void* hashHandle, const void* data, std::size_t length) override {
        NTSTATUS status = BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hashHandle),
                                          static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(length),
                                          0);
        return status >= 0;
    }

    bool FinishHash(void* hashHandle, std::uint8_t* outDigest32) override {
        NTSTATUS status =
            BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hashHandle), outDigest32, 32, 0);
        return status >= 0;
    }

    void DestroyHash(void* hashHandle) override {
        if (hashHandle != nullptr) {
            BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hashHandle));
        }
    }

    void CloseAlgorithmProvider(void* algHandle) override {
        if (algHandle != nullptr) {
            BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algHandle), 0);
        }
    }
};

// Releases a file/crypto handle exactly once on every exit path, real or
// faked, via whichever seam owns it.
template <typename Seam, typename CloseFn>
class ScopedSeamHandle {
public:
    ScopedSeamHandle(Seam& seam, void* handle, CloseFn closeFn) : seam_(seam), handle_(handle), closeFn_(closeFn) {}
    ~ScopedSeamHandle() {
        if (handle_ != nullptr) {
            (seam_.*closeFn_)(handle_);
        }
    }
    ScopedSeamHandle(const ScopedSeamHandle&) = delete;
    ScopedSeamHandle& operator=(const ScopedSeamHandle&) = delete;

    void* get() const { return handle_; }
    void reset() { handle_ = nullptr; }

private:
    Seam& seam_;
    void* handle_;
    CloseFn closeFn_;
};

} // namespace

std::string ToHex(const Sha256Digest& digest) {
    static const char kHexChars[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.bytes.size() * 2);
    for (std::uint8_t byte : digest.bytes) {
        hex.push_back(kHexChars[(byte >> 4) & 0x0F]);
        hex.push_back(kHexChars[byte & 0x0F]);
    }
    return hex;
}

ProcessInspectionResult ComputeExecutableIdentityWithSeams(const std::filesystem::path& path,
                                                             std::uintptr_t mainModuleBaseAddress,
                                                             std::size_t mainModuleImageSize, IFileReader& fileReader,
                                                             ICryptoApi& cryptoApi, ExecutableIdentity& outIdentity) {
    if (mainModuleBaseAddress == 0 || mainModuleImageSize == 0) {
        return ProcessInspectionResult::InvalidModuleRange;
    }
    std::uintptr_t moduleEnd = mainModuleBaseAddress + mainModuleImageSize;
    if (moduleEnd < mainModuleBaseAddress) {
        return ProcessInspectionResult::InvalidModuleRange;
    }

    // One handle, opened once, used for both the size query and the full
    // read below -- see docs/05-process-access.md for why this replaces
    // the previous separate stat-then-open approach.
    void* fileHandle = fileReader.Open(path);
    if (fileHandle == nullptr) {
        return ProcessInspectionResult::FileOpenFailed;
    }
    ScopedSeamHandle<IFileReader, void (IFileReader::*)(void*)> fileGuard(fileReader, fileHandle, &IFileReader::Close);

    std::uint64_t expectedSize = 0;
    if (!fileReader.GetSize(fileHandle, expectedSize)) {
        return ProcessInspectionResult::FileReadFailed;
    }

    void* algHandle = cryptoApi.OpenAlgorithmProvider();
    if (algHandle == nullptr) {
        return ProcessInspectionResult::HashFailed;
    }
    ScopedSeamHandle<ICryptoApi, void (ICryptoApi::*)(void*)> algGuard(cryptoApi, algHandle,
                                                                        &ICryptoApi::CloseAlgorithmProvider);

    std::size_t hashObjectLength = 0;
    if (!cryptoApi.GetHashObjectLength(algHandle, hashObjectLength)) {
        return ProcessInspectionResult::HashFailed;
    }

    std::vector<std::uint8_t> hashObjectBuffer(hashObjectLength);
    void* hashHandle = cryptoApi.CreateHash(algHandle, hashObjectBuffer.data(), hashObjectBuffer.size());
    if (hashHandle == nullptr) {
        return ProcessInspectionResult::HashFailed;
    }
    ScopedSeamHandle<ICryptoApi, void (ICryptoApi::*)(void*)> hashGuard(cryptoApi, hashHandle,
                                                                         &ICryptoApi::DestroyHash);

    std::vector<std::uint8_t> readBuffer(kReadChunkBytes);
    std::uint64_t totalBytesRead = 0;
    bool readOk = true;
    bool hashOk = true;

    while (readOk && hashOk) {
        std::size_t bytesRead = 0;
        if (!fileReader.Read(fileHandle, readBuffer.data(), readBuffer.size(), bytesRead)) {
            readOk = false;
            break;
        }
        if (bytesRead == 0) {
            break; // end of file
        }
        if (!cryptoApi.HashData(hashHandle, readBuffer.data(), bytesRead)) {
            hashOk = false;
            break;
        }
        totalBytesRead += static_cast<std::uint64_t>(bytesRead);
    }

    if (!readOk) {
        return ProcessInspectionResult::FileReadFailed;
    }
    if (!hashOk) {
        return ProcessInspectionResult::HashFailed;
    }

    // The loop above only exits with readOk && hashOk both true via a
    // natural end-of-file break -- at that point, the file must have been
    // read in full, byte for byte, against this same handle's own size,
    // or this is a short read (e.g. content changed mid-hash despite the
    // sharing-mode protection) and must never be treated as success.
    if (totalBytesRead != expectedSize) {
        return ProcessInspectionResult::FileReadFailed;
    }

    Sha256Digest digest;
    if (!cryptoApi.FinishHash(hashHandle, digest.bytes.data())) {
        return ProcessInspectionResult::HashFailed;
    }

    ExecutableIdentity identity;
    identity.executablePath = path;
    identity.fileSizeBytes = totalBytesRead;
    identity.sha256 = digest;
    identity.mainModuleBaseAddress = mainModuleBaseAddress;
    identity.mainModuleImageSize = mainModuleImageSize;
    outIdentity = std::move(identity);
    return ProcessInspectionResult::Success;
}

ProcessInspectionResult ComputeExecutableIdentity(const std::filesystem::path& path,
                                                    std::uintptr_t mainModuleBaseAddress,
                                                    std::size_t mainModuleImageSize,
                                                    ExecutableIdentity& outIdentity) {
    Win32FileReader fileReader;
    Win32CryptoApi cryptoApi;
    return ComputeExecutableIdentityWithSeams(path, mainModuleBaseAddress, mainModuleImageSize, fileReader,
                                               cryptoApi, outIdentity);
}

ProcessInspectionResult BuildExecutableIdentity(IProcessInspector& inspector, ExecutableIdentity& outIdentity) {
    std::filesystem::path imagePath;
    ProcessInspectionResult pathResult = inspector.GetImagePath(imagePath);
    if (pathResult != ProcessInspectionResult::Success) {
        return pathResult;
    }

    ModuleInfo mainModule;
    ProcessInspectionResult moduleResult = inspector.GetMainModule(mainModule);
    if (moduleResult != ProcessInspectionResult::Success) {
        return moduleResult;
    }

    return ComputeExecutableIdentity(imagePath, mainModule.baseAddress, mainModule.imageSize, outIdentity);
}

} // namespace sekiro_haptics::process
