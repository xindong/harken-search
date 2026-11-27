// Fault-path contract for the macOS durable file ops.
//
// The happy paths are covered by test_platform_durable_file_ops_*.cpp. What
// those miss is the reason this implementation was reviewed twice: a real write
// error must not be turned into Success, and a directory sync failure must not
// be swallowed. Both need a filesystem that fails on demand, which is what the
// AURO_DURABLE_FILE_OPS_TESTING seam provides.

#include "platform/durable_file_ops.h"
#include "platform/durable_file_ops_macos_test_hooks.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using Platform::FileSystem::DurableFileOpStatus;
using Platform::FileSystem::Testing::DurableFileOpFaultScript;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void CheckStatus(const Platform::FileSystem::DurableFileOpResult& result,
                 DurableFileOpStatus expectedStatus, uint32_t expectedError,
                 const char* message) {
    if (result.status != expectedStatus || result.systemError != expectedError) {
        std::cerr << "FAIL: " << message << " (status " << static_cast<int>(result.status)
                  << " error " << result.systemError << ")\n";
        ++failures;
    }
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "aris-durable-faults";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path source = root / "source.bin";
    const fs::path target = root / "target.bin";
    const fs::path replaceSource = root / "replace-source.bin";
    const fs::path replaceTarget = root / "replace-target.bin";
    const fs::path deleted = root / "deleted.bin";
    {
        std::ofstream output(source, std::ios::binary);
        output << "durable-fault-source";
    }
    {
        std::ofstream output(replaceSource, std::ios::binary);
        output << "durable-fault-replace";
    }
    {
        std::ofstream output(deleted, std::ios::binary);
        output << "durable-fault-delete";
    }

    DurableFileOpFaultScript script;
    Platform::FileSystem::Testing::SetDurableFileOpFaultScript(&script);

    // 1. A real write error is reported, not masked by the fsync fallback.
    //    This is the exact regression the review found: F_FULLFSYNC returning
    //    EIO used to be cleared by a successful fsync.
    script = {};
    script.fullsyncError = EIO;
    script.fsyncError = 0;
    CheckStatus(Platform::FileSystem::FlushExistingFileToDisk(source.u8string()),
                DurableFileOpStatus::FlushFailed, EIO,
                "EIO from the write-through flush must surface as FlushFailed/EIO");

    // 2. A filesystem that rejects the command still gets the portable fallback.
    script = {};
    script.fullsyncError = ENOTSUP;
    script.fsyncError = 0;
    CheckStatus(Platform::FileSystem::FlushExistingFileToDisk(source.u8string()),
                DurableFileOpStatus::Success, 0,
                "an unsupported command must fall back to fsync and succeed");

    // 3. The fallback preserves the real error it then hits.
    script = {};
    script.fullsyncError = EINVAL;
    script.fsyncError = EIO;
    CheckStatus(Platform::FileSystem::FlushExistingFileToDisk(source.u8string()),
                DurableFileOpStatus::FlushFailed, EIO,
                "a failing fsync fallback must report its own error");

    // 4. A copy that cannot open the parent directory reports it instead of
    //    claiming a durable copy.
    script = {};
    script.directoryOpenError = EACCES;
    CheckStatus(Platform::FileSystem::CopyFileExclusiveAndFlush(source.u8string(), target.u8string()),
                DurableFileOpStatus::OpenFailed, EACCES,
                "an unopenable parent directory must surface as OpenFailed/EACCES");
    fs::remove(target);

    // 5. A delete whose directory sync fails reports FlushFailed: the entry may
    //    come back after a power failure, so Success would be a lie.
    script = {};
    script.fullsyncError = EIO;
    CheckStatus(Platform::FileSystem::DeleteFileIfPresent(deleted.u8string()),
                DurableFileOpStatus::FlushFailed, EIO,
                "a failed directory sync after delete must surface as FlushFailed/EIO");

    // 6. A replace whose parent directory cannot be opened reports it.
    script = {};
    script.directoryOpenError = ENOENT;
    CheckStatus(Platform::FileSystem::ReplaceFileWriteThrough(replaceSource.u8string(), replaceTarget.u8string()),
                DurableFileOpStatus::OpenFailed, ENOENT,
                "an unopenable parent directory must surface after a rename");

    // 7. With the seam idle everything still succeeds, so the seam itself does
    //    not change the shipped behaviour.
    Platform::FileSystem::Testing::SetDurableFileOpFaultScript(nullptr);
    CheckStatus(Platform::FileSystem::FlushExistingFileToDisk(source.u8string()),
                DurableFileOpStatus::Success, 0,
                "the real flush must still succeed with the seam detached");

    fs::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " durable file-op fault check(s) failed\n";
        return 1;
    }
    return 0;
}
