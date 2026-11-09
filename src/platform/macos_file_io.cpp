#ifdef __APPLE__
#include "macos_file_io.h"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Platform::MacFileIO {
Descriptor::~Descriptor() {
    const int error = errno;
    if (value_ >= 0) ::close(value_);
    errno = error;
}
Descriptor::Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
Descriptor& Descriptor::operator=(Descriptor&& other) noexcept {
    if (this == &other) return *this;
    const int error = errno;
    if (value_ >= 0) ::close(value_);
    value_ = std::exchange(other.value_, -1);
    errno = error;
    return *this;
}
bool Descriptor::close() { return ::close(std::exchange(value_, -1)) == 0; }
bool ValidPath(const std::string& path) {
    if (path.empty() || path.find(char(0)) != std::string::npos) { errno = EINVAL; return false; }
    return true;
}
Descriptor OpenDirectory(const std::filesystem::path& path, bool create) {
    if (!path.is_absolute()) { errno = EINVAL; return {}; }
    Descriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!current) return {};
    for (const auto& component : path.relative_path()) {
        const auto name = component.string();
        if (name.empty() || name == ".") continue;
        if (name == ".." || name.find(char(0)) != std::string::npos) { errno = EINVAL; return {}; }
        if (create && ::mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST) return {};
        Descriptor next(::openat(current.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) return {};
        current = std::move(next);
    }
    return current;
}
Descriptor OpenParent(const std::string& path, std::string& name) {
    if (!ValidPath(path)) return {};
    std::error_code error;
    const auto absolute = std::filesystem::absolute(std::filesystem::path(path), error);
    if (error) { errno = error.value(); return {}; }
    name = absolute.filename().string();
    if (name.empty() || name == "." || name == "..") { errno = EINVAL; return {}; }
    return OpenDirectory(absolute.parent_path());
}
bool RegularFile(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) return false;
    if (!S_ISREG(info.st_mode)) { errno = EINVAL; return false; }
    return true;
}
bool WritableDirectory(int fd) { return ::faccessat(fd, ".", W_OK | X_OK, 0) == 0; }
bool RegularOrMissing(int directory, const std::string& name) {
    struct stat info {};
    if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    if (!S_ISREG(info.st_mode)) { errno = S_ISLNK(info.st_mode) ? ELOOP : EISDIR; return false; }
    return true;
}
bool FlushFile(int fd) {
    // fsync alone does not ask the device to flush its volatile write cache.
    // Do not report durable success if F_FULLFSYNC fails.
    return ::fsync(fd) == 0 && ::fcntl(fd, F_FULLFSYNC) == 0;
}
bool WriteAll(int fd, const char* bytes, size_t count) {
    while (count) {
        const ssize_t written = ::write(fd, bytes, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { if (written == 0) errno = EIO; return false; }
        bytes += written;
        count -= static_cast<size_t>(written);
    }
    return true;
}
}
#endif
