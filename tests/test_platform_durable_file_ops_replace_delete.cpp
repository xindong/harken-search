#include "platform/durable_file_ops.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "aris-durable-replace-delete-contract";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path source = root / "source.bin";
    const fs::path target = root / "target.bin";
    {
        std::ofstream(source, std::ios::binary) << "new";
        std::ofstream(target, std::ios::binary) << "old";
    }

#ifdef __APPLE__
    const fs::path danglingTarget = root / "dangling.bin";
    fs::create_symlink("missing.bin", danglingTarget);
    const auto exclusiveRejected = Platform::FileSystem::ReplaceFileWriteThrough(
        source.u8string(), danglingTarget.u8string(), false);
    if (exclusiveRejected.status != Platform::FileSystem::DurableFileOpStatus::TargetExists ||
        exclusiveRejected.systemError == 0 || !fs::exists(source) ||
        !fs::is_symlink(danglingTarget)) {
        std::cerr << "exclusive rename did not preserve a dangling destination link and source\n";
        fs::remove_all(root);
        return 1;
    }
    const fs::path exclusiveTarget = root / "exclusive.bin";
    const auto exclusiveMoved = Platform::FileSystem::ReplaceFileWriteThrough(
        source.u8string(), exclusiveTarget.u8string(), false);
    if (exclusiveMoved.status != Platform::FileSystem::DurableFileOpStatus::Success ||
        exclusiveMoved.systemError != 0 || fs::exists(source) ||
        std::ifstream(exclusiveTarget, std::ios::binary).get() != 'n') {
        std::cerr << "exclusive rename did not move the source to a vacant destination\n";
        fs::remove_all(root);
        return 1;
    }
    fs::rename(exclusiveTarget, source);
#endif

    const auto replaced = Platform::FileSystem::ReplaceFileWriteThrough(
        source.u8string(), target.u8string());
    if (replaced.status != Platform::FileSystem::DurableFileOpStatus::Success) {
        std::cerr << "replace did not succeed\n";
        fs::remove_all(root);
        return 1;
    }
    {
        std::ifstream input(target, std::ios::binary);
        const std::string payload((std::istreambuf_iterator<char>(input)), {});
        if (payload != "new") {
            std::cerr << "replacement payload mismatch\n";
            fs::remove_all(root);
            return 1;
        }
    }

    const auto missingReplace = Platform::FileSystem::ReplaceFileWriteThrough(
        (root / "missing.bin").u8string(), target.u8string());
    if (missingReplace.status != Platform::FileSystem::DurableFileOpStatus::ReplaceFailed ||
        missingReplace.systemError == 0) {
        std::cerr << "replace failure did not preserve primary error\n";
        fs::remove_all(root);
        return 1;
    }

    const auto deleted = Platform::FileSystem::DeleteFileIfPresent(target.u8string());
    if (deleted.status != Platform::FileSystem::DurableFileOpStatus::Success ||
        fs::exists(target)) {
        std::cerr << "existing file was not deleted\n";
        fs::remove_all(root);
        return 1;
    }
    const auto missingDelete = Platform::FileSystem::DeleteFileIfPresent(target.u8string());
    if (missingDelete.status != Platform::FileSystem::DurableFileOpStatus::Success ||
        missingDelete.systemError != 0) {
        std::cerr << "missing file was not treated as success\n";
        fs::remove_all(root);
        return 1;
    }
    const auto missingParent = Platform::FileSystem::DeleteFileIfPresent(
        (root / "missing-dir" / "file.bin").u8string());
    if (missingParent.status != Platform::FileSystem::DurableFileOpStatus::DeleteFailed ||
        missingParent.systemError == 0) {
        std::cerr << "missing parent was not reported as delete failure\n";
        fs::remove_all(root);
        return 1;
    }
    const auto invalid = Platform::FileSystem::DeleteFileIfPresent({});
    if (invalid.status != Platform::FileSystem::DurableFileOpStatus::InvalidArgument) {
        std::cerr << "empty delete path was not rejected\n";
        fs::remove_all(root);
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
