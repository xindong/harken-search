#ifdef __APPLE__

#include "durable_file_ops.h"
#include "macos_file_io.h"

#include <array>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace Platform::FileSystem {
namespace {
namespace io = MacFileIO;
using Status = DurableFileOpStatus;
DurableFileOpResult Result(Status status, int error = 0) {
    return {status, static_cast<uint32_t>(error)};
}
}

DurableFileOpResult FlushExistingFileToDisk(const std::string& path) {
    if (!io::ValidPath(path)) return Result(Status::InvalidArgument, errno);
    std::string name;
    auto parent = io::OpenParent(path, name);
    if (!parent) return Result(Status::OpenFailed, errno);
    io::Descriptor file(::openat(parent.get(), name.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!file || !io::RegularFile(file.get())) return Result(Status::OpenFailed, errno);
    if (!io::FlushFile(file.get())) return Result(Status::FlushFailed, errno);
    if (!file.close()) return Result(Status::FlushFailed, errno);
    return Result(Status::Success);
}

DurableFileOpResult CopyFileExclusiveAndFlush(const std::string& source, const std::string& target) {
    if (!io::ValidPath(source) || !io::ValidPath(target)) return Result(Status::InvalidArgument, errno);
    std::string sourceName, targetName;
    auto sourceParent = io::OpenParent(source, sourceName);
    if (!sourceParent) return Result(Status::CopyFailed, errno);
    io::Descriptor input(::openat(sourceParent.get(), sourceName.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!input || !io::RegularFile(input.get())) return Result(Status::CopyFailed, errno);
    auto targetParent = io::OpenParent(target, targetName);
    if (!targetParent) return Result(Status::CopyFailed, errno);
    io::Descriptor output(::openat(targetParent.get(), targetName.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!output) return Result(errno == EEXIST ? Status::TargetExists : Status::CopyFailed, errno);
    std::array<char, 65536> buffer;
    for (;;) {
        const ssize_t count = ::read(input.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return Result(Status::CopyFailed, errno);
        if (!count) break;
        if (!io::WriteAll(output.get(), buffer.data(), static_cast<size_t>(count))) return Result(Status::CopyFailed, errno);
    }
    if (!io::FlushFile(output.get()) || !output.close() || ::fsync(targetParent.get()) != 0) return Result(Status::FlushFailed, errno);
    return Result(Status::Success);
}

DurableFileOpResult ReplaceFileWriteThrough(const std::string& source, const std::string& target) {
    if (!io::ValidPath(source) || !io::ValidPath(target)) return Result(Status::InvalidArgument, errno);
    std::string sourceName, targetName;
    auto sourceParent = io::OpenParent(source, sourceName);
    if (!sourceParent) return Result(Status::ReplaceFailed, errno);
    auto targetParent = io::OpenParent(target, targetName);
    if (!targetParent) return Result(Status::ReplaceFailed, errno);
    io::Descriptor input(::openat(sourceParent.get(), sourceName.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!input || !io::RegularFile(input.get()) || !io::RegularOrMissing(targetParent.get(), targetName)) return Result(Status::ReplaceFailed, errno);
    if (!io::FlushFile(input.get())) return Result(Status::FlushFailed, errno);
    if (::renameat(sourceParent.get(), sourceName.c_str(), targetParent.get(), targetName.c_str()) != 0) return Result(Status::ReplaceFailed, errno);
    if (::fsync(targetParent.get()) != 0 || ::fsync(sourceParent.get()) != 0) return Result(Status::FlushFailed, errno);
    return Result(Status::Success);
}

DurableFileOpResult DeleteFileIfPresent(const std::string& path) {
    if (!io::ValidPath(path)) return Result(Status::InvalidArgument, errno);
    std::string name;
    auto parent = io::OpenParent(path, name);
    if (!parent) return Result(Status::DeleteFailed, errno);
    if (::unlinkat(parent.get(), name.c_str(), 0) != 0 && errno != ENOENT) return Result(Status::DeleteFailed, errno);
    if (::fsync(parent.get()) != 0) return Result(Status::FlushFailed, errno);
    return Result(Status::Success);
}

} // namespace Platform::FileSystem

#endif // __APPLE__
