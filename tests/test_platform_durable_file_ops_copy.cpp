#include "platform/durable_file_ops.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "aris-durable-copy-contract";
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path source = root / "source.bin";
    const fs::path target = root / "target.bin";
    const fs::path missing = root / "missing.bin";
    {
        std::ofstream output(source, std::ios::binary);
        output << "durable-copy";
    }

#ifdef __APPLE__
    const auto flushed = Platform::FileSystem::FlushExistingFileToDisk(source.u8string());
    const auto missingFlush = Platform::FileSystem::FlushExistingFileToDisk(missing.u8string());
    const auto invalidFlush = Platform::FileSystem::FlushExistingFileToDisk({});
    if (flushed.status != Platform::FileSystem::DurableFileOpStatus::Success ||
        flushed.systemError != 0 ||
        missingFlush.status != Platform::FileSystem::DurableFileOpStatus::OpenFailed ||
        missingFlush.systemError == 0 ||
        invalidFlush.status != Platform::FileSystem::DurableFileOpStatus::InvalidArgument) {
        std::cerr << "standalone flush did not report success, missing path and invalid input correctly\n";
        fs::remove_all(root);
        return 1;
    }
#endif

    const auto copied = Platform::FileSystem::CopyFileExclusiveAndFlush(
        source.u8string(), target.u8string());
    if (copied.status != Platform::FileSystem::DurableFileOpStatus::Success) {
        std::cerr << "copy + flush did not succeed\n";
        fs::remove_all(root);
        return 1;
    }
    std::ifstream input(target, std::ios::binary);
    const std::string payload((std::istreambuf_iterator<char>(input)), {});
    if (payload != "durable-copy") {
        std::cerr << "copied payload mismatch\n";
        fs::remove_all(root);
        return 1;
    }
    input.close();

    const auto existing = Platform::FileSystem::CopyFileExclusiveAndFlush(
        source.u8string(), target.u8string());
    if (existing.status != Platform::FileSystem::DurableFileOpStatus::TargetExists ||
        existing.systemError == 0) {
        std::cerr << "existing target was not rejected exclusively\n";
        fs::remove_all(root);
        return 1;
    }

    const auto missingResult = Platform::FileSystem::CopyFileExclusiveAndFlush(
        missing.u8string(), (root / "missing-target.bin").u8string());
    if (missingResult.status != Platform::FileSystem::DurableFileOpStatus::CopyFailed ||
        missingResult.systemError == 0) {
        std::cerr << "missing source error was not captured\n";
        fs::remove_all(root);
        return 1;
    }

    const auto invalid = Platform::FileSystem::CopyFileExclusiveAndFlush(
        std::string(), target.u8string());
    if (invalid.status != Platform::FileSystem::DurableFileOpStatus::InvalidArgument) {
        std::cerr << "empty source was not rejected\n";
        fs::remove_all(root);
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
