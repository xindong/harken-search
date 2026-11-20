#ifdef _WIN32

#include "durable_file_ops.h"

#include "text_encoding.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Platform::FileSystem {
namespace {

class ScopedFile {
public:
    explicit ScopedFile(HANDLE handle) : handle_(handle) {}
    ~ScopedFile() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

DurableFileOpResult Failure(DurableFileOpStatus status, DWORD error = ERROR_SUCCESS) {
    DurableFileOpResult result;
    result.status = status;
    result.systemError = static_cast<uint32_t>(error);
    return result;
}

DurableFileOpResult FlushExistingFileWide(const std::wstring& path) {
    ScopedFile file(CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return Failure(DurableFileOpStatus::OpenFailed, GetLastError());
    }
    if (!FlushFileBuffers(file.get())) {
        const DWORD error = GetLastError();
        return Failure(DurableFileOpStatus::FlushFailed, error);
    }
    return Failure(DurableFileOpStatus::Success);
}

} // namespace

DurableFileOpResult FlushExistingFileToDisk(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return Failure(DurableFileOpStatus::InvalidArgument);
    const std::wstring path = TextEncoding::Utf8ToWideStrict(pathUtf8);
    if (path.empty()) return Failure(DurableFileOpStatus::EncodingFailed);

    return FlushExistingFileWide(path);
}

DurableFileOpResult CopyFileExclusiveAndFlush(
    const std::string& sourcePathUtf8, const std::string& targetPathUtf8) {
    if (sourcePathUtf8.empty() || targetPathUtf8.empty()) {
        return Failure(DurableFileOpStatus::InvalidArgument);
    }
    const std::wstring sourcePath = TextEncoding::Utf8ToWideStrict(sourcePathUtf8);
    const std::wstring targetPath = TextEncoding::Utf8ToWideStrict(targetPathUtf8);
    if (sourcePath.empty() || targetPath.empty()) {
        return Failure(DurableFileOpStatus::EncodingFailed);
    }
    if (!CopyFileW(sourcePath.c_str(), targetPath.c_str(), TRUE)) {
        const DWORD error = GetLastError();
        const DurableFileOpStatus status =
            error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                ? DurableFileOpStatus::TargetExists
                : DurableFileOpStatus::CopyFailed;
        return Failure(status, error);
    }
    return FlushExistingFileWide(targetPath);
}

DurableFileOpResult ReplaceFileWriteThrough(
    const std::string& sourcePathUtf8, const std::string& targetPathUtf8, bool replaceExisting) {
    if (sourcePathUtf8.empty() || targetPathUtf8.empty()) {
        return Failure(DurableFileOpStatus::InvalidArgument);
    }
    const std::wstring sourcePath = TextEncoding::Utf8ToWideStrict(sourcePathUtf8);
    const std::wstring targetPath = TextEncoding::Utf8ToWideStrict(targetPathUtf8);
    if (sourcePath.empty() || targetPath.empty()) {
        return Failure(DurableFileOpStatus::EncodingFailed);
    }
    const DWORD flags = replaceExisting ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                        : MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(sourcePath.c_str(), targetPath.c_str(), flags)) {
        const DWORD error = GetLastError();
        const bool targetExists = !replaceExisting &&
            (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS);
        return Failure(targetExists ? DurableFileOpStatus::TargetExists
                                    : DurableFileOpStatus::ReplaceFailed, error);
    }
    return Failure(DurableFileOpStatus::Success);
}

DurableFileOpResult DeleteFileIfPresent(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return Failure(DurableFileOpStatus::InvalidArgument);
    const std::wstring path = TextEncoding::Utf8ToWideStrict(pathUtf8);
    if (path.empty()) return Failure(DurableFileOpStatus::EncodingFailed);
    if (DeleteFileW(path.c_str())) return Failure(DurableFileOpStatus::Success);
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return Failure(DurableFileOpStatus::Success);
    if (error == ERROR_PATH_NOT_FOUND) {
        return Failure(DurableFileOpStatus::DeleteFailed, error);
    }
    return Failure(DurableFileOpStatus::DeleteFailed, error);
}

} // namespace Platform::FileSystem

#endif // _WIN32
