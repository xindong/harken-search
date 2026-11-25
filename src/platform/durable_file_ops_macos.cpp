#ifdef __APPLE__

#include "durable_file_ops.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Platform::FileSystem {
namespace {

DurableFileOpResult Result(DurableFileOpStatus status, int error = 0) {
    DurableFileOpResult result;
    result.status = status;
    result.systemError = error > 0 ? static_cast<uint32_t>(error) : 0;
    return result;
}

// POSIX fsync only hands the bytes to the drive; macOS needs F_FULLFSYNC for the
// "on the platter" promise the Win32 implementation makes with FlushFileBuffers.
// Filesystems that reject the command still get the portable fsync fallback.
bool FlushDescriptor(int fd, int& error) {
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        error = 0;
        return true;
    }
    error = errno;
    if (::fsync(fd) == 0) {
        error = 0;
        return true;
    }
    error = errno;
    return false;
}

bool FlushPath(const std::string& path, int& openError, int& flushError) {
    openError = 0;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        openError = errno;
        return false;
    }
    const bool flushed = FlushDescriptor(fd, flushError);
    ::close(fd);
    return flushed;
}

// Durability of a rename needs the directory entry synced, not just the file.
void FlushParentDirectory(const std::string& path) {
    std::string directory = path;
    const std::string::size_type slash = directory.find_last_of('/');
    if (slash == std::string::npos) {
        directory = ".";
    } else if (slash == 0) {
        directory = "/";
    } else {
        directory.erase(slash);
    }
    const int fd = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void)::fsync(fd);
    ::close(fd);
}

bool CopyBytes(int source, int target, int& error) {
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t read = ::read(source, buffer, sizeof(buffer));
        if (read == 0) return true;
        if (read < 0) {
            if (errno == EINTR) continue;
            error = errno;
            return false;
        }
        ssize_t written = 0;
        while (written < read) {
            const ssize_t chunk = ::write(target, buffer + written, static_cast<size_t>(read - written));
            if (chunk < 0) {
                if (errno == EINTR) continue;
                error = errno;
                return false;
            }
            written += chunk;
        }
    }
}

} // namespace

DurableFileOpResult FlushExistingFileToDisk(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return Result(DurableFileOpStatus::InvalidArgument);
    int openError = 0;
    int flushError = 0;
    if (!FlushPath(pathUtf8, openError, flushError)) {
        if (openError != 0) return Result(DurableFileOpStatus::OpenFailed, openError);
        return Result(DurableFileOpStatus::FlushFailed, flushError);
    }
    return Result(DurableFileOpStatus::Success);
}

DurableFileOpResult CopyFileExclusiveAndFlush(const std::string& sourcePathUtf8,
                                              const std::string& targetPathUtf8) {
    if (sourcePathUtf8.empty() || targetPathUtf8.empty()) {
        return Result(DurableFileOpStatus::InvalidArgument);
    }
    const int source = ::open(sourcePathUtf8.c_str(), O_RDONLY | O_CLOEXEC);
    if (source < 0) return Result(DurableFileOpStatus::CopyFailed, errno);

    // O_EXCL without O_TRUNC is what makes the target exclusive; never overwrite here.
    const int target = ::open(targetPathUtf8.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (target < 0) {
        const int error = errno;
        ::close(source);
        const DurableFileOpStatus status =
            error == EEXIST ? DurableFileOpStatus::TargetExists : DurableFileOpStatus::CopyFailed;
        return Result(status, error);
    }

    int copyError = 0;
    if (!CopyBytes(source, target, copyError)) {
        ::close(source);
        ::close(target);
        ::unlink(targetPathUtf8.c_str());
        return Result(DurableFileOpStatus::CopyFailed, copyError);
    }
    ::close(source);

    int flushError = 0;
    const bool flushed = FlushDescriptor(target, flushError);
    ::close(target);
    if (!flushed) {
        ::unlink(targetPathUtf8.c_str());
        return Result(DurableFileOpStatus::FlushFailed, flushError);
    }
    return Result(DurableFileOpStatus::Success);
}

DurableFileOpResult ReplaceFileWriteThrough(const std::string& sourcePathUtf8,
                                            const std::string& targetPathUtf8,
                                            bool replaceExisting) {
    if (sourcePathUtf8.empty() || targetPathUtf8.empty()) {
        return Result(DurableFileOpStatus::InvalidArgument);
    }
    if (!replaceExisting && ::access(targetPathUtf8.c_str(), F_OK) == 0) {
        return Result(DurableFileOpStatus::TargetExists, EEXIST);
    }
    // rename(2) is atomic within a filesystem and replaces an existing target,
    // which is the macOS equivalent of MOVEFILE_REPLACE_EXISTING.
    if (::rename(sourcePathUtf8.c_str(), targetPathUtf8.c_str()) != 0) {
        return Result(DurableFileOpStatus::ReplaceFailed, errno);
    }
    int openError = 0;
    int flushError = 0;
    if (!FlushPath(targetPathUtf8, openError, flushError)) {
        if (openError != 0) return Result(DurableFileOpStatus::ReplaceFailed, openError);
        return Result(DurableFileOpStatus::FlushFailed, flushError);
    }
    FlushParentDirectory(targetPathUtf8);
    return Result(DurableFileOpStatus::Success);
}

DurableFileOpResult DeleteFileIfPresent(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return Result(DurableFileOpStatus::InvalidArgument);
    if (::unlink(pathUtf8.c_str()) == 0) return Result(DurableFileOpStatus::Success);
    const int error = errno;
    // Win32 separates "file not found" (success) from "path not found" (failure);
    // POSIX reports ENOENT for both, so check the parent to keep the same contract.
    std::string directory = pathUtf8;
    const std::string::size_type slash = directory.find_last_of('/');
    if (slash == std::string::npos) directory = ".";
    else if (slash == 0) directory = "/";
    else directory.erase(slash);
    struct stat info {};
    if (error == ENOENT && ::stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
        return Result(DurableFileOpStatus::Success);
    }
    return Result(DurableFileOpStatus::DeleteFailed, error);
}

} // namespace Platform::FileSystem

#endif // __APPLE__
