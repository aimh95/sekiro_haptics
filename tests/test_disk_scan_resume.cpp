// Tests for ResumeDiskCandidateSession(): identity-match success, every
// identity-mismatch dimension, process-liveness, and data-file validation.

#include "sekiro_haptics/process/CandidateStorage.hpp"
#include "sekiro_haptics/process/DiskCandidateScanner.hpp"
#include "sekiro_haptics/process/ScanManifest.hpp"
#include "testing.hpp"

#include "FakeProcessInspector.hpp"
#include "FakeProcessMemoryMap.hpp"
#include "FakeProcessReader.hpp"

#include <filesystem>
#include <fstream>

using namespace sekiro_haptics::process;

namespace {

ProcessMemoryRegion MakeRegion(std::uintptr_t base, std::size_t size, MemoryRegionKind kind = MemoryRegionKind::Private) {
    ProcessMemoryRegion r;
    r.baseAddress = base;
    r.sizeBytes = size;
    r.kind = kind;
    return r;
}

std::filesystem::path FreshSessionDir(const std::string& name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

ScanSessionIdentity MakeIdentity() {
    ScanSessionIdentity id;
    id.executableFileSizeBytes = 12345;
    id.sha256Hex = "deadbeef";
    id.pid = 999;
    id.mainModuleBaseAddress = 0x140000000;
    id.mainModuleImageSize = 0x1000;
    id.valueType = "u32";
    id.scope = "private-readable";
    return id;
}

std::filesystem::path SetUpCompleteBaselineSession(const std::string& dirName) {
    std::filesystem::path sessionDir = FreshSessionDir(dirName);
    FakeProcessReader reader;
    FakeProcessInspector inspector;
    FakeProcessMemoryMap memoryMap;
    memoryMap.SetRegions({MakeRegion(0x1000, 8)});
    std::uint32_t values[2] = {1, 2};
    reader.PokeBytes(0x1000, &values[0], 4);
    reader.PokeBytes(0x1004, &values[1], 4);

    DiskScanStats stats;
    DiskScanOutcome outcome = BeginDiskCandidateScan(reader, inspector, memoryMap, CandidateScanScope::PrivateReadable,
                                                       CandidateValueType::U32, sessionDir, MakeIdentity(),
                                                       kDefaultDiskScanMemoryBudgetBytes, stats);
    (void)outcome;
    return sessionDir;
}

} // namespace

SH_TEST(ResumeDiskCandidateSession_MatchingIdentityAndAliveProcess_Succeeds) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_ok");

    FakeProcessReader reader;
    reader.SetAlive(true);
    ScanManifest manifest;
    DiskScanOutcome outcome = ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, manifest);

    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(manifest.generation == 0);
    SH_CHECK(manifest.state == ScanManifestState::BaselineComplete);
    SH_CHECK(manifest.totalValueCount == 2);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ExecutableFileSizeMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_filesize");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.executableFileSizeBytes += 1;

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_Sha256Mismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_sha256");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.sha256Hex = "different_hash_entirely";

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_PidMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_pid");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.pid += 1;

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ModuleBaseMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_modulebase");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.mainModuleBaseAddress += 0x1000; // different ASLR placement -- a different launch

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ModuleImageSizeMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_modulesize");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.mainModuleImageSize += 1;

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ValueTypeMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_valuetype");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.valueType = "u16"; // scanned as u32 originally

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ScopeMismatch_ReturnsSessionMismatch) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_scope");

    ScanSessionIdentity mismatched = MakeIdentity();
    mismatched.scope = "all-readable"; // scanned as private-readable originally

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, mismatched, reader, manifest) == DiskScanOutcome::SessionMismatch);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_ProcessNotAlive_ReturnsProcessExited) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_notalive");

    FakeProcessReader reader;
    reader.SetAlive(false);
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, manifest) == DiskScanOutcome::ProcessExited);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_MissingManifest_ReturnsCorruptFile) {
    std::filesystem::path sessionDir = FreshSessionDir("sh_disk_resume_missing_manifest");
    std::filesystem::create_directories(sessionDir);

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, manifest) == DiskScanOutcome::CorruptFile);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_MissingBaselineDataFile_ReturnsCorruptFile) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_missing_baseline");

    // Corrupt the session by deleting the data file the manifest claims is complete.
    std::filesystem::remove(sessionDir / "baseline.bin");

    FakeProcessReader reader;
    ScanManifest manifest;
    SH_CHECK(ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, manifest) == DiskScanOutcome::CorruptFile);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_AfterFiltering_ValidatesLatestGenerationFile) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_after_filter");

    // Fresh reader: must replicate every original baseline value (an unset
    // address in FakeProcessReader reads back as 0, which would spuriously
    // look "decreased" from any non-zero baseline value).
    FakeProcessReader reader;
    std::uint32_t updated0 = 0; // decreased from 1
    std::uint32_t unchanged1 = 2;
    reader.PokeBytes(0x1000, &updated0, 4);
    reader.PokeBytes(0x1004, &unchanged1, 4);

    DiskScanStats filterStats;
    SH_CHECK(FilterDiskCandidates(reader, sessionDir, CandidateFilterKind::Decreased, nullptr, kDefaultDiskScanMemoryBudgetBytes,
                                   filterStats) == DiskScanOutcome::CompleteCoverage);

    FakeProcessReader resumeReader;
    ScanManifest manifest;
    DiskScanOutcome outcome = ResumeDiskCandidateSession(sessionDir, MakeIdentity(), resumeReader, manifest);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(manifest.generation == 1);
    SH_CHECK(manifest.candidateCount == 1);

    std::filesystem::remove_all(sessionDir);
}

SH_TEST(ResumeDiskCandidateSession_IncompleteTempNeverPromotedToSuccess) {
    std::filesystem::path sessionDir = SetUpCompleteBaselineSession("sh_disk_resume_stray_tmp");

    // Simulate an interrupted filter attempt leaving a stray .tmp behind.
    {
        std::ofstream stray(sessionDir / "candidates-0001.tmp", std::ios::binary);
        stray << "not a real generation file";
    }

    // Resume must still succeed off the last known-good complete file
    // (the baseline) and must not be confused by the stray .tmp.
    FakeProcessReader reader;
    ScanManifest manifest;
    DiskScanOutcome outcome = ResumeDiskCandidateSession(sessionDir, MakeIdentity(), reader, manifest);
    SH_CHECK(outcome == DiskScanOutcome::CompleteCoverage);
    SH_CHECK(manifest.generation == 0);

    std::filesystem::remove_all(sessionDir);
}
