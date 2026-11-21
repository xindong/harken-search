#pragma once

#include <cstdint>
#include <string>

namespace Platform::FileSystem {

// @stage7-contract
// API contract: all paths are UTF-8. Operations are durable only on Success;
// every other status preserves the backend/system error and does not promise a
// completed replacement or deletion. Functions do not retain caller storage.

enum class DurableFileOpStatus {
    Success,
    InvalidArgument,
    EncodingFailed,
    OpenFailed,
    TargetExists,
    CopyFailed,
    FlushFailed,
    ReplaceFailed,
    DeleteFailed,
    Unsupported,
};

struct DurableFileOpResult {
    DurableFileOpStatus status = DurableFileOpStatus::Unsupported;
    uint32_t systemError = 0;
};

DurableFileOpResult FlushExistingFileToDisk(const std::string& pathUtf8);
DurableFileOpResult CopyFileExclusiveAndFlush(const std::string& sourcePathUtf8,
                                              const std::string& targetPathUtf8);
DurableFileOpResult ReplaceFileWriteThrough(const std::string& sourcePathUtf8,
                                            const std::string& targetPathUtf8,
                                            bool replaceExisting = true);
DurableFileOpResult DeleteFileIfPresent(const std::string& pathUtf8);

} // namespace Platform::FileSystem
