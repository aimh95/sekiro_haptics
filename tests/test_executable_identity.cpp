// Unit tests for SHA-256/ExecutableIdentity (ComputeExecutableIdentity).
// Pure file I/O -- no live process, IProcessReader, or IProcessInspector
// involved. All temp files are created under a dedicated test-only
// directory and cleaned up afterward; no user or real game files are ever
// touched.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/FileHashSeam.hpp"
#include "testing.hpp"

#include "FakeFileHashSeam.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace sekiro_haptics::process;

namespace {

std::filesystem::path TestDir() {
    return std::filesystem::temp_directory_path() / "sh_executable_identity_tests";
}

// Creates (or overwrites) a file with exactly `content` and returns its path.
std::filesystem::path WriteTempFile(const std::string& name, const std::string& content) {
    std::filesystem::create_directories(TestDir());
    std::filesystem::path path = TestDir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path;
}

// Known NIST SHA-256 test vectors.
constexpr const char* kSha256EmptyHex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kSha256AbcHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

} // namespace

SH_TEST(ComputeExecutableIdentity_EmptyFile_MatchesKnownSha256TestVector) {
    std::filesystem::path path = WriteTempFile("empty.bin", "");

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x400000, 0x1000, identity);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(ToHex(identity.sha256) == kSha256EmptyHex);
    SH_CHECK(identity.fileSizeBytes == 0);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_AbcFile_MatchesKnownSha256TestVector) {
    std::filesystem::path path = WriteTempFile("abc.bin", "abc");

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x400000, 0x1000, identity);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(ToHex(identity.sha256) == kSha256AbcHex);
    SH_CHECK(identity.fileSizeBytes == 3);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_SameFile_ProducesSameDigest) {
    std::filesystem::path path = WriteTempFile("repeat.bin", "the quick brown fox jumps over the lazy dog");

    ExecutableIdentity first;
    ExecutableIdentity second;
    SH_CHECK(ComputeExecutableIdentity(path, 0x1000, 0x100, first) == ProcessInspectionResult::Success);
    SH_CHECK(ComputeExecutableIdentity(path, 0x1000, 0x100, second) == ProcessInspectionResult::Success);

    SH_CHECK(first.sha256 == second.sha256);
    SH_CHECK(first.fileSizeBytes == second.fileSizeBytes);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_OneByteDifferentFile_ProducesDifferentDigest) {
    std::filesystem::path pathA = WriteTempFile("variant_a.bin", "AAAAAAAAAA");
    std::filesystem::path pathB = WriteTempFile("variant_b.bin", "AAAAAAAAAB"); // last byte differs

    ExecutableIdentity identityA;
    ExecutableIdentity identityB;
    SH_CHECK(ComputeExecutableIdentity(pathA, 0x1000, 0x100, identityA) == ProcessInspectionResult::Success);
    SH_CHECK(ComputeExecutableIdentity(pathB, 0x1000, 0x100, identityB) == ProcessInspectionResult::Success);

    SH_CHECK(identityA.sha256 != identityB.sha256);

    std::filesystem::remove(pathA);
    std::filesystem::remove(pathB);
}

SH_TEST(ToHex_ProducesLowercase64CharacterString) {
    std::filesystem::path path = WriteTempFile("hex_check.bin", "abc");

    ExecutableIdentity identity;
    SH_CHECK(ComputeExecutableIdentity(path, 0x1000, 0x100, identity) == ProcessInspectionResult::Success);

    std::string hex = ToHex(identity.sha256);
    SH_CHECK(hex.size() == 64);
    for (char c : hex) {
        SH_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_FileDoesNotExist_ReturnsFileOpenFailed) {
    std::filesystem::path path = TestDir() / "this_file_does_not_exist.bin";
    std::filesystem::remove(path); // ensure it really doesn't exist

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x1000, 0x100, identity);

    SH_CHECK(result == ProcessInspectionResult::FileOpenFailed);
}

SH_TEST(ComputeExecutableIdentity_PathIsADirectory_ReturnsFileOpenFailed) {
    std::filesystem::create_directories(TestDir());

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(TestDir(), 0x1000, 0x100, identity);

    SH_CHECK(result == ProcessInspectionResult::FileOpenFailed);
}

// NOTE: This is a best-effort CONCURRENCY SMOKE TEST, not a deterministic
// verification of the FileReadFailed path -- it does not reliably exercise
// any specific branch every run, and must never be relied on as the sole
// (or a counted) test for FileReadFailed. The deterministic,
// seam-injected FileReadFailed/HashFailed tests below this one are what
// actually verify those branches every run. Since ComputeExecutableIdentity
// now opens the file with FILE_SHARE_READ only (see
// docs/05-process-access.md), a concurrent resize_file() from another
// handle is *expected* to usually fail with a sharing violation while our
// handle is open -- this test's real remaining value is confirming that
// outIdentity is never left claiming a size larger than what was actually
// hashed, under either outcome.
SH_TEST(ComputeExecutableIdentity_ConcurrencySmokeTest_FileTruncatedDuringRead_HandlesRaceEitherWaySafely) {
    std::string content(8 * 1024 * 1024, 'A'); // 8 MB
    std::filesystem::path path = WriteTempFile("race_target.bin", content);

    std::thread truncator([&path] {
        std::error_code ec;
        std::filesystem::resize_file(path, 16, ec); // shrink drastically, as early as possible
    });

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x1000, 0x100, identity);
    truncator.join();

    if (result == ProcessInspectionResult::Success) {
        // The race was lost (read finished before truncation, or
        // truncation happened before the read even started) -- the
        // reported size must match what's actually on disk at the time
        // ComputeExecutableIdentity returned, never a stale larger value.
        SH_CHECK(identity.fileSizeBytes <= content.size());
    } else {
        SH_CHECK(result == ProcessInspectionResult::FileReadFailed);
    }

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_InvalidModuleBaseZero_ReturnsInvalidModuleRange) {
    std::filesystem::path path = WriteTempFile("valid_content.bin", "abc");

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, /*mainModuleBaseAddress=*/0, 0x100, identity);

    SH_CHECK(result == ProcessInspectionResult::InvalidModuleRange);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_InvalidModuleSizeZero_ReturnsInvalidModuleRange) {
    std::filesystem::path path = WriteTempFile("valid_content2.bin", "abc");

    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x1000, /*mainModuleImageSize=*/0, identity);

    SH_CHECK(result == ProcessInspectionResult::InvalidModuleRange);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_ASLRLikeDifferentBase_SameFile_SameBuildDigest) {
    // Simulates ASLR: the same file, "loaded" at two different module
    // base addresses across two calls, must still report the identical
    // sha256/fileSizeBytes (the stable build identity) -- only
    // mainModuleBaseAddress differs, which is expected and correct.
    std::filesystem::path path = WriteTempFile("aslr_sim.bin", "identical build, different load address");

    ExecutableIdentity runA;
    ExecutableIdentity runB;
    SH_CHECK(ComputeExecutableIdentity(path, 0x00400000, 0x9000, runA) == ProcessInspectionResult::Success);
    SH_CHECK(ComputeExecutableIdentity(path, 0x7FF612340000ULL, 0x9000, runB) == ProcessInspectionResult::Success);

    SH_CHECK(runA.sha256 == runB.sha256);
    SH_CHECK(runA.fileSizeBytes == runB.fileSizeBytes);
    SH_CHECK(runA.mainModuleBaseAddress != runB.mainModuleBaseAddress);

    std::filesystem::remove(path);
}

SH_TEST(ComputeExecutableIdentity_DifferentPathsSameContent_SameBuildIdentity) {
    // Build identity must not depend on where the file lives.
    std::string content = "same bytes, two different paths";
    std::filesystem::path pathA = WriteTempFile("path_a.bin", content);
    std::filesystem::path pathB = WriteTempFile("path_b.bin", content);

    ExecutableIdentity identityA;
    ExecutableIdentity identityB;
    SH_CHECK(ComputeExecutableIdentity(pathA, 0x1000, 0x100, identityA) == ProcessInspectionResult::Success);
    SH_CHECK(ComputeExecutableIdentity(pathB, 0x1000, 0x100, identityB) == ProcessInspectionResult::Success);

    SH_CHECK(identityA.sha256 == identityB.sha256);
    SH_CHECK(identityA.executablePath != identityB.executablePath);

    std::filesystem::remove(pathA);
    std::filesystem::remove(pathB);
}

SH_TEST(ComputeExecutableIdentity_DifferentContent_DifferentBuildIdentity) {
    std::filesystem::path pathA = WriteTempFile("content_a.bin", "version one of the build");
    std::filesystem::path pathB = WriteTempFile("content_b.bin", "version two of the build!");

    ExecutableIdentity identityA;
    ExecutableIdentity identityB;
    SH_CHECK(ComputeExecutableIdentity(pathA, 0x1000, 0x100, identityA) == ProcessInspectionResult::Success);
    SH_CHECK(ComputeExecutableIdentity(pathB, 0x1000, 0x100, identityB) == ProcessInspectionResult::Success);

    SH_CHECK(identityA.sha256 != identityB.sha256);

    std::filesystem::remove(pathA);
    std::filesystem::remove(pathB);
}

SH_TEST(ComputeExecutableIdentity_FailedCall_NeverLeavesPartiallyFilledIdentity) {
    ExecutableIdentity identity;
    identity.fileSizeBytes = 999999; // sentinel -- must not survive a failed call
    identity.mainModuleBaseAddress = 0xDEADBEEF;

    std::filesystem::path missing = TestDir() / "definitely_missing_for_this_test.bin";
    std::filesystem::remove(missing);

    ProcessInspectionResult result = ComputeExecutableIdentity(missing, 0x1000, 0x100, identity);

    SH_CHECK(result != ProcessInspectionResult::Success);
    // outIdentity is untouched on failure -- the sentinel values prove
    // ComputeExecutableIdentity() didn't partially overwrite it.
    SH_CHECK(identity.fileSizeBytes == 999999);
    SH_CHECK(identity.mainModuleBaseAddress == 0xDEADBEEF);
}

// ===========================================================================
// Deterministic FileReadFailed / HashFailed tests, via the IFileReader /
// ICryptoApi seam (FileHashSeam.hpp + FakeFileHashSeam.hpp). Every test
// below injects one specific, named failure point and re-runs
// deterministically every time -- none of them depend on timing or a race.
// ===========================================================================

namespace {
ExecutableIdentity SentinelIdentity() {
    ExecutableIdentity identity;
    identity.fileSizeBytes = 999999;
    identity.mainModuleBaseAddress = 0xDEADBEEF;
    identity.mainModuleImageSize = 0x7777;
    return identity;
}

void CheckIdentityUntouched(const ExecutableIdentity& identity) {
    SH_CHECK(identity.fileSizeBytes == 999999);
    SH_CHECK(identity.mainModuleBaseAddress == 0xDEADBEEF);
    SH_CHECK(identity.mainModuleImageSize == 0x7777);
}
} // namespace

// --- FileReadFailed: deterministic ---

SH_TEST(ComputeExecutableIdentityWithSeams_ExplicitMidReadFailure_ReturnsFileReadFailedDeterministically) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        FakeFileReader fileReader;
        fileReader.SetContent(std::vector<std::uint8_t>(200 * 1024, 0x41)); // several 64K chunks
        fileReader.FailReadAtCall(0);                                      // fails on the very first read
        FakeCryptoApi cryptoApi;
        ExecutableIdentity identity = SentinelIdentity();

        ProcessInspectionResult result = ComputeExecutableIdentityWithSeams(
            "seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

        SH_CHECK(result == ProcessInspectionResult::FileReadFailed);
        CheckIdentityUntouched(identity);
    }
}

SH_TEST(ComputeExecutableIdentityWithSeams_FailureBeforeLastChunk_StopsImmediatelyAndReturnsFileReadFailed) {
    FakeFileReader fileReader;
    // 200 KB of content against a 64 KB internal chunk size means 4 reads
    // would be needed for a full success (64+64+64+8). Failing at call
    // index 1 (the 2nd read) must stop the loop right there -- proven by
    // ReadCalls() never reaching 3 or 4.
    fileReader.SetContent(std::vector<std::uint8_t>(200 * 1024, 0x42));
    fileReader.FailReadAtCall(1);
    FakeCryptoApi cryptoApi;
    ExecutableIdentity identity = SentinelIdentity();

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::FileReadFailed);
    SH_CHECK(fileReader.ReadCalls() == 2); // stopped right after the failing call, never attempted a 3rd/4th
    CheckIdentityUntouched(identity);
}

SH_TEST(ComputeExecutableIdentityWithSeams_ShortRead_TotalBytesBelowReportedSize_ReturnsFileReadFailed) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        FakeFileReader fileReader;
        fileReader.SetContent(std::string("only one hundred bytes worth of real content-ish padding.........."));
        // Reports a size larger than what Read() will ever actually
        // deliver -- Read() hits real EOF (0 bytes) before totalBytesRead
        // reaches this reported size, deterministically every run.
        fileReader.SetReportedSize(10000);
        FakeCryptoApi cryptoApi;
        ExecutableIdentity identity = SentinelIdentity();

        ProcessInspectionResult result = ComputeExecutableIdentityWithSeams(
            "seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

        SH_CHECK(result == ProcessInspectionResult::FileReadFailed);
        CheckIdentityUntouched(identity);
    }
}

SH_TEST(ComputeExecutableIdentityWithSeams_FileSizeQueryFailure_ReturnsFileReadFailed) {
    FakeFileReader fileReader;
    fileReader.SetContent("abc");
    fileReader.SetSizeQueryShouldSucceed(false);
    FakeCryptoApi cryptoApi;
    ExecutableIdentity identity = SentinelIdentity();

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::FileReadFailed);
    CheckIdentityUntouched(identity);
    // The file handle must still be closed exactly once even though the
    // size query (not the open) is what failed.
    SH_CHECK(fileReader.CloseCalls() == 1);
    SH_CHECK(fileReader.IsCurrentlyOpen() == false);
}

SH_TEST(ComputeExecutableIdentityWithSeams_NormalRead_StillSucceeds) {
    // Positive control for the seam itself -- proves the seam-based path
    // isn't just failing everything by construction.
    FakeFileReader fileReader;
    fileReader.SetContent("abc");
    FakeCryptoApi cryptoApi;
    ExecutableIdentity identity;

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(identity.fileSizeBytes == 3);
    SH_CHECK(fileReader.CloseCalls() == 1);
    SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 1);
    SH_CHECK(cryptoApi.DestroyHashCalls() == 1);
}

// --- HashFailed: deterministic, one per distinct CNG step ---

SH_TEST(ComputeExecutableIdentityWithSeams_AlgorithmProviderOpenFailure_ReturnsHashFailed) {
    FakeFileReader fileReader;
    fileReader.SetContent("abc");
    FakeCryptoApi cryptoApi;
    cryptoApi.SetOpenAlgorithmProviderShouldSucceed(false);
    ExecutableIdentity identity = SentinelIdentity();

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::HashFailed);
    CheckIdentityUntouched(identity);
    // Nothing was ever created past the failed open, so nothing to clean up.
    SH_CHECK(cryptoApi.CreateHashCalls() == 0);
    SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 0);
    SH_CHECK(fileReader.CloseCalls() == 1); // the file handle is still cleaned up
}

SH_TEST(ComputeExecutableIdentityWithSeams_HashObjectPropertyPrepFailure_ReturnsHashFailed) {
    FakeFileReader fileReader;
    fileReader.SetContent("abc");
    FakeCryptoApi cryptoApi;
    cryptoApi.SetGetHashObjectLengthShouldSucceed(false);
    ExecutableIdentity identity = SentinelIdentity();

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::HashFailed);
    CheckIdentityUntouched(identity);
    SH_CHECK(cryptoApi.CreateHashCalls() == 0);
    SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 1); // the alg provider that WAS opened is still closed
    SH_CHECK(cryptoApi.AlgorithmProviderStillOpen() == false);
}

SH_TEST(ComputeExecutableIdentityWithSeams_HashCreationFailure_ReturnsHashFailed) {
    FakeFileReader fileReader;
    fileReader.SetContent("abc");
    FakeCryptoApi cryptoApi;
    cryptoApi.SetCreateHashShouldSucceed(false);
    ExecutableIdentity identity = SentinelIdentity();

    ProcessInspectionResult result =
        ComputeExecutableIdentityWithSeams("seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

    SH_CHECK(result == ProcessInspectionResult::HashFailed);
    CheckIdentityUntouched(identity);
    SH_CHECK(cryptoApi.DestroyHashCalls() == 0); // never created, nothing to destroy
    SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 1);
    SH_CHECK(cryptoApi.AlgorithmProviderStillOpen() == false);
}

SH_TEST(ComputeExecutableIdentityWithSeams_HashUpdateFailure_ReturnsHashFailedAndCleansUpFully) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        FakeFileReader fileReader;
        fileReader.SetContent("some bytes to hash");
        FakeCryptoApi cryptoApi;
        cryptoApi.FailHashDataAtCall(0);
        ExecutableIdentity identity = SentinelIdentity();

        ProcessInspectionResult result = ComputeExecutableIdentityWithSeams(
            "seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

        SH_CHECK(result == ProcessInspectionResult::HashFailed);
        CheckIdentityUntouched(identity);
        SH_CHECK(cryptoApi.DestroyHashCalls() == 1);
        SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 1);
        SH_CHECK(cryptoApi.AlgorithmProviderStillOpen() == false);
        SH_CHECK(cryptoApi.HashStillOpen() == false);
        SH_CHECK(fileReader.CloseCalls() == 1);
    }
}

SH_TEST(ComputeExecutableIdentityWithSeams_HashFinishFailure_ReturnsHashFailedAndCleansUpFully) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        FakeFileReader fileReader;
        fileReader.SetContent("some bytes to hash");
        FakeCryptoApi cryptoApi;
        cryptoApi.SetFinishHashShouldSucceed(false);
        ExecutableIdentity identity = SentinelIdentity();

        ProcessInspectionResult result = ComputeExecutableIdentityWithSeams(
            "seam_test.bin", 0x1000, 0x100, fileReader, cryptoApi, identity);

        SH_CHECK(result == ProcessInspectionResult::HashFailed);
        CheckIdentityUntouched(identity);
        SH_CHECK(cryptoApi.DestroyHashCalls() == 1);
        SH_CHECK(cryptoApi.CloseAlgorithmProviderCalls() == 1);
        SH_CHECK(cryptoApi.AlgorithmProviderStillOpen() == false);
        SH_CHECK(cryptoApi.HashStillOpen() == false);
        SH_CHECK(fileReader.CloseCalls() == 1);
    }
}

// A failure at any single CNG step must never corrupt state for a
// subsequent, separate successful call -- exercised against the REAL
// (non-fake) implementation for genuine end-to-end confidence, not just
// the fakes' own bookkeeping.
SH_TEST(ComputeExecutableIdentity_AfterFailedCall_SubsequentSuccessfulCallStillWorksNormally) {
    std::filesystem::path missing = TestDir() / "still_missing_for_this_test.bin";
    std::filesystem::remove(missing);
    ExecutableIdentity failedIdentity;
    SH_CHECK(ComputeExecutableIdentity(missing, 0x1000, 0x100, failedIdentity) != ProcessInspectionResult::Success);

    std::filesystem::path path = WriteTempFile("after_failure.bin", "abc");
    ExecutableIdentity identity;
    ProcessInspectionResult result = ComputeExecutableIdentity(path, 0x1000, 0x100, identity);

    SH_CHECK(result == ProcessInspectionResult::Success);
    SH_CHECK(ToHex(identity.sha256) == kSha256AbcHex);

    std::filesystem::remove(path);
}
