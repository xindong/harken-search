#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Platform::FileSystem {

// @stage7-contract
// API contract: path arguments are UTF-8. Free functions do not retain caller
// storage and report ordinary operation failure through false, status values,
// empty results, or the documented error output parameter. DirectoryIterator,
// StreamWriter, and StreamReader own their native handles and are move-only or
// non-copyable as declared. Callers must serialize operations on one instance;
// independent instances may be used from different threads.

enum class DirectoryReadStatus {
    Entry,
    End,
    Error,
};

struct DirectoryEntry {
    std::string pathUtf8;
    bool isDirectory = false;
    bool isRegularFile = false;
    uint64_t fileSize = 0;
};

struct RegularFileInfo {
    bool isRegularFile = false;
    bool sizeAvailable = false;
    uint64_t sizeBytes = 0;
};

struct PathAttributes {
    bool hidden = false;
    bool system = false;
    bool temporary = false;
};

struct CopyFileResult {
    bool success = false;
    std::uint32_t systemError = 0;
    std::string errorMessage;
};

class DirectoryIterator {
public:
    DirectoryIterator();
    ~DirectoryIterator();
    DirectoryIterator(const DirectoryIterator&) = delete;
    DirectoryIterator& operator=(const DirectoryIterator&) = delete;

    bool Open(const std::string& rootPathUtf8, bool recursive, bool skipPermissionDenied);
    DirectoryReadStatus Next(DirectoryEntry& outEntry,
                             std::string* errorMessage = nullptr);
    bool IsOpen() const;
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool CanOpenFileForRead(const std::string& pathUtf8);
bool Exists(const std::string& pathUtf8);
enum class PathStatus {
    Missing,
    Present,
    Error,
    Unreachable,
};
PathStatus ProbePath(const std::string& pathUtf8,
                     std::string* errorMessage = nullptr);
bool IsNetworkPath(const std::string& pathUtf8);
PathAttributes QueryPathAttributes(const std::string& pathUtf8);
bool IsDirectory(const std::string& pathUtf8);
bool CreateDirectories(const std::string& pathUtf8);
bool EnsureDirectoryExists(const std::string& pathUtf8);
RegularFileInfo ProbeRegularFile(const std::string& pathUtf8);
bool CopyFile(const std::string& sourcePathUtf8,
              const std::string& targetPathUtf8,
              bool overwrite,
              std::string* errorMessage = nullptr);
CopyFileResult CopyFileWithSystemError(const std::string& sourcePathUtf8,
                                       const std::string& targetPathUtf8,
                                       bool overwrite) noexcept;
bool Rename(const std::string& sourcePathUtf8,
            const std::string& targetPathUtf8,
            std::string* errorMessage = nullptr);
bool RemoveFile(const std::string& pathUtf8,
                std::string* errorMessage = nullptr);
bool RemoveAll(const std::string& pathUtf8,
               std::string* errorMessage = nullptr);
bool ResizeFile(const std::string& pathUtf8,
                uint64_t sizeBytes,
                std::string* errorMessage = nullptr);
bool Equivalent(const std::string& leftPathUtf8,
                const std::string& rightPathUtf8);
std::string CurrentDirectory();
std::string WeaklyCanonical(const std::string& pathUtf8);
std::string RelativePath(const std::string& targetPathUtf8,
                         const std::string& basePathUtf8);
enum class TemporaryDirectoryStatus {
    Success,
    QueryFailed,
};

struct TemporaryDirectoryResult {
    TemporaryDirectoryStatus status = TemporaryDirectoryStatus::QueryFailed;
    std::string pathUtf8;
    std::error_code error;
};

TemporaryDirectoryResult QueryTemporaryDirectory();
std::string TemporaryDirectory();
std::vector<uint8_t> ReadFileBytes(const std::string& pathUtf8);
std::vector<uint8_t> ReadFileBytes(const std::string& pathUtf8, size_t maxBytes);
int64_t LastWriteTimeUnixSeconds(const std::string& pathUtf8);
bool ReadFileText(const std::string& pathUtf8, std::string& outText);
bool WriteFileText(const std::string& pathUtf8, const std::string& text);
bool AppendFileText(const std::string& pathUtf8, const std::string& text);

class StreamWriter {
public:
    StreamWriter();
    ~StreamWriter();
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    // Precondition: pathUtf8 names a writable file. Postcondition: a successful
    // open owns one persistent stream until Close or destruction.
    bool Open(const std::string& pathUtf8, bool append, bool binary);
    bool Write(std::string_view bytes);
    bool Flush();
    std::int64_t Tell() const;
    void Close();
    // Closes and flushes pending bytes, reporting write/close failure. False if
    // no stream is open; the handle is released even when closing fails.
    bool CloseChecked();
    bool IsOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class StreamSeekOrigin {
    Begin,
    Current,
    End,
};

class StreamReader {
public:
    StreamReader();
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;
    StreamReader(StreamReader&&) noexcept;
    StreamReader& operator=(StreamReader&&) noexcept;

    // Precondition: pathUtf8 names a readable file. Postcondition: reads are
    // incremental and never materialize the whole file in the PAL.
    bool Open(const std::string& pathUtf8, bool binary);
    size_t Read(void* destination, size_t bytes);
    bool Seek(int64_t offset, StreamSeekOrigin origin);
    int64_t Tell() const;
    bool Good() const;
    void Clear();
    void Close();
    bool IsOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool AtomicFileReplace(const std::string& targetPathUtf8,
                       const std::string& sourcePathUtf8);

} // namespace Platform::FileSystem
