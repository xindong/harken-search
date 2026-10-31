#include "platform/filesystem.h"

#ifdef _WIN32
#include "platform/filesystem_atomic_replace_test_hooks.h"
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef CopyFile
#undef CopyFile
#endif
#endif

int main() {
#ifdef _WIN32
    {
        Platform::FileSystem::Testing::AtomicReplaceScript script;
        script.moveFileExResult = false;
        script.deleteFileResult = true;
        script.moveFileResult = false;
        Platform::FileSystem::Testing::SetAtomicReplaceScript(&script);
        const bool replaced = Platform::FileSystem::AtomicFileReplace("original", "original.tmp");
        Platform::FileSystem::Testing::SetAtomicReplaceScript(nullptr);
        if (replaced || !script.targetPresent || !script.sourcePresent ||
            script.moveFileExCalls != 1 || script.deleteFileCalls != 0 ||
            script.moveFileCalls != 0) {
            std::cerr << "FAIL: failed atomic replace must preserve original and temp\n";
            return 1;
        }
    }
    {
        Platform::FileSystem::Testing::AtomicReplaceScript script;
        script.moveFileExResult = true;
        Platform::FileSystem::Testing::SetAtomicReplaceScript(&script);
        const bool replaced = Platform::FileSystem::AtomicFileReplace("original", "original.tmp");
        Platform::FileSystem::Testing::SetAtomicReplaceScript(nullptr);
        if (!replaced || !script.targetPresent || script.sourcePresent ||
            script.moveFileExCalls != 1 || script.deleteFileCalls != 0 ||
            script.moveFileCalls != 0) {
            std::cerr << "FAIL: successful atomic replace must consume temp\n";
            return 1;
        }
    }
#endif
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aris_stage6_filesystem_probe";
    const fs::path path = root / "readable.tmp";
    const fs::path nested = root / "nested" / "child";
    std::error_code ec;
    fs::remove_all(root, ec);

    if (Platform::FileSystem::CurrentDirectory().empty()) {
        std::cerr << "FAIL: current directory probe should return a UTF-8 path\n";
        return 1;
    }

    if (!Platform::FileSystem::CreateDirectories(nested.u8string()) ||
        !Platform::FileSystem::Exists(nested.u8string())) {
        std::cerr << "FAIL: nested directory should be created\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (Platform::FileSystem::CreateDirectories(nested.u8string())) {
        std::cerr << "FAIL: existing directory creation should be idempotent\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path ensuredNested = root / "ensured" / "nested";
    if (!Platform::FileSystem::EnsureDirectoryExists(ensuredNested.u8string()) ||
        !fs::is_directory(ensuredNested, ec)) {
        std::cerr << "FAIL: ensure directory should create a missing nested path\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (!Platform::FileSystem::EnsureDirectoryExists(ensuredNested.u8string())) {
        std::cerr << "FAIL: ensure directory should accept an existing directory\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path occupiedDirectoryPath = root / "occupied-directory";
    std::ofstream(occupiedDirectoryPath) << "file";
    if (Platform::FileSystem::EnsureDirectoryExists(occupiedDirectoryPath.u8string())) {
        std::cerr << "FAIL: ensure directory should reject a file-occupied path\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto missingInfo = Platform::FileSystem::ProbeRegularFile(
        (root / "missing.bin").u8string());
    if (missingInfo.isRegularFile || missingInfo.sizeAvailable || missingInfo.sizeBytes != 0) {
        std::cerr << "FAIL: missing path should report no regular file and no size\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto directoryInfo = Platform::FileSystem::ProbeRegularFile(nested.u8string());
    if (directoryInfo.isRegularFile || directoryInfo.sizeAvailable || directoryInfo.sizeBytes != 0) {
        std::cerr << "FAIL: directory should report no regular file and no size\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto occupiedInfo = Platform::FileSystem::ProbeRegularFile(
        occupiedDirectoryPath.u8string());
    if (!occupiedInfo.isRegularFile || !occupiedInfo.sizeAvailable || occupiedInfo.sizeBytes != 4) {
        std::cerr << "FAIL: non-empty regular file should report exact size\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path zeroBytePath = root / "zero-byte.bin";
    std::ofstream(zeroBytePath, std::ios::binary);
    const auto zeroByteInfo = Platform::FileSystem::ProbeRegularFile(zeroBytePath.u8string());
    if (!zeroByteInfo.isRegularFile || !zeroByteInfo.sizeAvailable || zeroByteInfo.sizeBytes != 0) {
        std::cerr << "FAIL: zero-byte regular file should report size zero\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path resizePath = root / "resize-test.bin";
    std::ofstream(resizePath, std::ios::binary) << "abcdefgh";
    std::string resizeError;
    if (!Platform::FileSystem::ResizeFile(resizePath.u8string(), 4, &resizeError) ||
        fs::file_size(resizePath, ec) != 4) {
        std::cerr << "FAIL: resize should shrink a file: " << resizeError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path unicodeResizePath = root / fs::u8path(u8"缩小-😀.bin");
    const std::vector<uint8_t> resizeBytes{0x00, 0xFF, 0x10, 0x20, 0x7F, 0x80, 0x42};
    {
        std::ofstream output(unicodeResizePath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(resizeBytes.data()),
                     static_cast<std::streamsize>(resizeBytes.size()));
    }
    resizeError.clear();
    if (!Platform::FileSystem::ResizeFile(unicodeResizePath.u8string(), 4, &resizeError)) {
        std::cerr << "FAIL: resize should accept a Unicode UTF-8 path: " << resizeError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto resizedBytes = Platform::FileSystem::ReadFileBytes(unicodeResizePath.u8string());
    if (resizedBytes.size() != 4 ||
        !std::equal(resizedBytes.begin(), resizedBytes.end(), resizeBytes.begin())) {
        std::cerr << "FAIL: resize should preserve the original byte prefix\n";
        fs::remove_all(root, ec);
        return 1;
    }
    resizeError.clear();
    if (Platform::FileSystem::ResizeFile(
            (root / "missing-resize.bin").u8string(), 4, &resizeError) ||
        resizeError.empty()) {
        std::cerr << "FAIL: resize should report a missing-path error\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const fs::path unicodeMtimePath = root / fs::u8path(u8"时间 😀.tmp");
    std::ofstream(unicodeMtimePath, std::ios::binary) << "mtime";
    const int64_t unicodeMtime =
        Platform::FileSystem::LastWriteTimeUnixSeconds(unicodeMtimePath.u8string());
    if (unicodeMtime <= 0) {
        std::cerr << "FAIL: Unicode regular file should have a positive last-write time\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (Platform::FileSystem::LastWriteTimeUnixSeconds(
            (root / "missing-mtime.tmp").u8string()) != 0) {
        std::cerr << "FAIL: missing path should report last-write time zero\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (Platform::FileSystem::LastWriteTimeUnixSeconds(root.u8string()) <= 0) {
        std::cerr << "FAIL: existing directory should have a positive last-write time\n";
        fs::remove_all(root, ec);
        return 1;
    }
    std::string embeddedNulMtimePath = unicodeMtimePath.u8string();
    embeddedNulMtimePath.append("\0suffix", 7);
    if (Platform::FileSystem::LastWriteTimeUnixSeconds(embeddedNulMtimePath) !=
        unicodeMtime) {
        std::cerr << "FAIL: embedded-NUL suffix should preserve prefix last-write time behavior\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto nonPositiveSystemTime =
        std::chrono::system_clock::time_point{} - std::chrono::seconds(10);
    const auto nonPositiveFileTime = fs::file_time_type::clock::now() +
        (nonPositiveSystemTime - std::chrono::system_clock::now());
    fs::last_write_time(unicodeMtimePath, nonPositiveFileTime, ec);
    if (ec) {
        std::cout << "SKIP: platform rejected non-positive epoch last-write time\n";
        ec.clear();
    } else if (Platform::FileSystem::LastWriteTimeUnixSeconds(
                   unicodeMtimePath.u8string()) > 0) {
        std::cerr << "FAIL: accepted non-positive epoch should remain non-positive\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const std::string canonicalRoot = Platform::FileSystem::WeaklyCanonical(root.u8string());
    const std::string relativeNested = Platform::FileSystem::RelativePath(nested.u8string(), root.u8string());
    if (canonicalRoot.empty() || relativeNested != "nested/child") {
        std::cerr << "FAIL: path canonicalization and relative lookup should preserve UTF-8 path semantics\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path enumRoot = root / "enum";
    const fs::path enumNested = enumRoot / "nested";
    fs::create_directories(enumNested, ec);
    std::ofstream(enumRoot / "top.wav") << "top";
    std::ofstream(enumNested / "child.wav") << "child";
    Platform::FileSystem::DirectoryIterator iterator;
    if (!iterator.Open(enumRoot.u8string(), true, true)) {
        std::cerr << "FAIL: directory iterator should open a recursive root\n";
        fs::remove_all(root, ec);
        return 1;
    }
    bool sawTop = false;
    bool sawChild = false;
    bool sawNestedDirectory = false;
    for (;;) {
        Platform::FileSystem::DirectoryEntry entry;
        std::string iteratorError;
        const auto status = iterator.Next(entry, &iteratorError);
        if (status == Platform::FileSystem::DirectoryReadStatus::End) break;
        if (status == Platform::FileSystem::DirectoryReadStatus::Error) {
            std::cerr << "FAIL: directory iterator reported an unexpected error: " << iteratorError << "\n";
            fs::remove_all(root, ec);
            return 1;
        }
        sawTop = sawTop || entry.pathUtf8.find("top.wav") != std::string::npos;
        sawChild = sawChild || entry.pathUtf8.find("child.wav") != std::string::npos;
        sawNestedDirectory = sawNestedDirectory || entry.isDirectory;
    }
    iterator.Close();
    if (!sawTop || !sawChild || !sawNestedDirectory) {
        std::cerr << "FAIL: recursive directory iterator should expose files and directories\n";
        fs::remove_all(root, ec);
        return 1;
    }

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "aris";
    }

    if (!Platform::FileSystem::CanOpenFileForRead(path.u8string())) {
        std::cerr << "FAIL: regular file should be readable\n";
        fs::remove(path, ec);
        return 1;
    }
    if (!Platform::FileSystem::Exists(path.u8string())) {
        std::cerr << "FAIL: existing regular file should exist\n";
        fs::remove_all(root, ec);
        return 1;
    }
    std::string probeError;
    if (Platform::FileSystem::ProbePath(path.u8string(), &probeError) !=
        Platform::FileSystem::PathStatus::Present) {
        std::cerr << "FAIL: existing regular file should probe as present: " << probeError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

#ifdef _WIN32
    const std::wstring widePath = path.wstring();
    if (!SetFileAttributesW(widePath.c_str(),
                            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                FILE_ATTRIBUTE_TEMPORARY)) {
        std::cerr << "FAIL: test fixture attributes should be set\n";
        fs::remove_all(root, ec);
        return 1;
    }
    const auto attributes = Platform::FileSystem::QueryPathAttributes(path.u8string());
    if (!attributes.hidden || !attributes.system || !attributes.temporary) {
        std::cerr << "FAIL: filesystem PAL should preserve Windows scan-filter attributes\n";
        SetFileAttributesW(widePath.c_str(), FILE_ATTRIBUTE_NORMAL);
        fs::remove_all(root, ec);
        return 1;
    }
    SetFileAttributesW(widePath.c_str(), FILE_ATTRIBUTE_NORMAL);
#endif

    const auto missingAttributes = Platform::FileSystem::QueryPathAttributes(
        (root / "missing-attributes.tmp").u8string());
    const auto emptyAttributes = Platform::FileSystem::QueryPathAttributes("");
    if (missingAttributes.hidden || missingAttributes.system ||
        missingAttributes.temporary || emptyAttributes.hidden ||
        emptyAttributes.system || emptyAttributes.temporary) {
        std::cerr << "FAIL: failed attribute queries should preserve false semantics\n";
        fs::remove_all(root, ec);
        return 1;
    }

    fs::remove(path, ec);
    if (Platform::FileSystem::CanOpenFileForRead(path.u8string())) {
        std::cerr << "FAIL: missing file should not be readable\n";
        return 1;
    }
    if (Platform::FileSystem::Exists(path.u8string())) {
        std::cerr << "FAIL: removed file should not exist\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (Platform::FileSystem::ProbePath(path.u8string(), &probeError) !=
        Platform::FileSystem::PathStatus::Missing) {
        std::cerr << "FAIL: removed file should probe as missing: " << probeError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path textPath = root / "text.txt";
    if (!Platform::FileSystem::WriteFileText(textPath.u8string(), "alpha") ||
        !Platform::FileSystem::AppendFileText(textPath.u8string(), "\nbeta")) {
        std::cerr << "FAIL: text write and append should succeed\n";
        fs::remove_all(root, ec);
        return 1;
    }
    std::string text;
    if (!Platform::FileSystem::ReadFileText(textPath.u8string(), text) ||
        text != "alpha\nbeta") {
        std::cerr << "FAIL: text roundtrip should preserve append order\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path copySource = root / "copy-source.txt";
    const fs::path copyTarget = root / "copy-target.txt";
    std::string copyError;
    if (!Platform::FileSystem::IsDirectory(root.u8string()) ||
        !Platform::FileSystem::WriteFileText(copySource.u8string(), "first") ||
        !Platform::FileSystem::CopyFile(copySource.u8string(), copyTarget.u8string(), false, &copyError)) {
        std::cerr << "FAIL: directory probe and initial copy should succeed: " << copyError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (!Platform::FileSystem::WriteFileText(copySource.u8string(), "second") ||
        !Platform::FileSystem::CopyFile(copySource.u8string(), copyTarget.u8string(), false, &copyError) ||
        !Platform::FileSystem::ReadFileText(copyTarget.u8string(), text) || text != "first") {
        std::cerr << "FAIL: non-overwrite copy should preserve the existing target\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (!Platform::FileSystem::CopyFile(copySource.u8string(), copyTarget.u8string(), true, &copyError) ||
        !Platform::FileSystem::ReadFileText(copyTarget.u8string(), text) || text != "second") {
        std::cerr << "FAIL: overwrite copy should replace the target: " << copyError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path renameSource = root / "rename-source.txt";
    const fs::path renameTarget = root / "rename-target.txt";
    if (!Platform::FileSystem::WriteFileText(renameSource.u8string(), "rename") ||
        !Platform::FileSystem::Rename(renameSource.u8string(), renameTarget.u8string(), &copyError) ||
        !Platform::FileSystem::Exists(renameTarget.u8string()) ||
        Platform::FileSystem::Exists(renameSource.u8string())) {
        std::cerr << "FAIL: rename should move the source to the target: " << copyError << "\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path bytesPath = root / "bytes.bin";
    const std::vector<uint8_t> expectedBytes{0x00, 0x0A, 0x0D, 0xFF};
    {
        std::ofstream output(bytesPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(expectedBytes.data()),
                     static_cast<std::streamsize>(expectedBytes.size()));
    }
    const auto bytes = Platform::FileSystem::ReadFileBytes(bytesPath.u8string());
    if (bytes != expectedBytes) {
        std::cerr << "FAIL: byte read should preserve file contents\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (Platform::FileSystem::ReadFileText((root / "missing.txt").u8string(), text)) {
        std::cerr << "FAIL: missing text file should fail to read\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path streamPath = root / "stream.txt";
    Platform::FileSystem::StreamWriter writer;
    if (!writer.Open(streamPath.u8string(), false, false) ||
        !writer.Write("first\n") || !writer.Flush()) {
        std::cerr << "FAIL: streaming writer should open, write, and flush\n";
        fs::remove_all(root, ec);
        return 1;
    }
    writer.Close();
    if (writer.IsOpen() || !writer.Open(streamPath.u8string(), true, false) ||
        !writer.Write("second\n")) {
        std::cerr << "FAIL: streaming writer should close and reopen for append\n";
        fs::remove_all(root, ec);
        return 1;
    }
    if (!writer.CloseChecked() || writer.IsOpen() || writer.CloseChecked()) {
        std::cerr << "FAIL: checked close should flush buffered bytes and report an already closed stream\n";
        return 1;
    }
    if (!Platform::FileSystem::ReadFileText(streamPath.u8string(), text) ||
        text != "first\nsecond\n") {
        std::cerr << "FAIL: streaming writer should preserve append order\n";
        fs::remove_all(root, ec);
        return 1;
    }

    const fs::path readerPath = root / "reader.bin";
    {
        std::ofstream output(readerPath, std::ios::binary | std::ios::trunc);
        output << "first\nsecond\n";
    }
    Platform::FileSystem::StreamReader reader;
    if (!reader.Open(readerPath.u8string(), true)) {
        std::cerr << "FAIL: streaming reader should open a binary path\n";
        fs::remove_all(root, ec);
        return 1;
    }
    char firstLine[6] = {};
    if (reader.Read(firstLine, sizeof(firstLine)) != sizeof(firstLine) ||
        std::string(firstLine, sizeof(firstLine)) != "first\n" ||
        reader.Tell() != 6 ||
        !reader.Seek(6, Platform::FileSystem::StreamSeekOrigin::Begin)) {
        std::cerr << "FAIL: streaming reader should read and seek incrementally\n";
        fs::remove_all(root, ec);
        return 1;
    }
    char secondLine[7] = {};
    if (reader.Read(secondLine, sizeof(secondLine)) != sizeof(secondLine) ||
        std::string(secondLine, sizeof(secondLine)) != "second\n") {
        std::cerr << "FAIL: streaming reader seek should preserve source bytes\n";
        fs::remove_all(root, ec);
        return 1;
    }
    reader.Close();

    const fs::path removePath = root / "remove.txt";
    if (!Platform::FileSystem::WriteFileText(removePath.u8string(), "remove") ||
        !Platform::FileSystem::RemoveFile(removePath.u8string()) ||
        Platform::FileSystem::Exists(removePath.u8string()) ||
        Platform::FileSystem::RemoveFile(removePath.u8string())) {
        std::cerr << "FAIL: file removal should succeed exactly once\n";
        fs::remove_all(root, ec);
        return 1;
    }

    fs::remove_all(root, ec);
    std::cout << "PASS: filesystem status and readable probes preserve semantics\n";
    return 0;
}
