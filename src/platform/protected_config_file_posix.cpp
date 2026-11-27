#include "protected_config_file_posix.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/xattr.h>
#elif defined(__APPLE__)
#include <sys/acl.h>
#endif

namespace Platform::FileSystem::ProtectedConfig {

class Handle {
public:
    explicit Handle(int value) : value_(value) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    int Get() const { return value_; }
    bool Valid() const { return value_ >= 0; }
    bool Close() {
        if (!Valid()) return true;
        const bool closed = close(value_) == 0;
        value_ = -1;
        return closed;
    }
private:
    int value_;
};


#if defined(__linux__)
inline bool ReadAttribute(int file, const char* name, std::string& value) {
    const auto size = fgetxattr(file, name, nullptr, 0);
    if (size < 0) return errno == ENODATA || errno == ENOTSUP;
    value.resize(static_cast<std::size_t>(size));
    return fgetxattr(file, name, value.data(), value.size()) == size;
}
#elif defined(__APPLE__)
inline bool ReadAcl(int file, std::string& value) {
    const auto acl = acl_get_fd_np(file, ACL_TYPE_EXTENDED);
    if (!acl) return errno == ENOENT || errno == ENOTSUP;
    char* text = acl_to_text(acl, nullptr);
    if (text) value.assign(text);
    const bool read = text != nullptr;
    if (text) acl_free(text);
    acl_free(acl);
    return read;
}
#endif

inline bool Capture(const Handle& file, Access& access) {
    struct stat details{};
    if (fstat(file.Get(), &details) != 0 || !S_ISREG(details.st_mode) ||
        (details.st_mode & (S_ISUID | S_ISGID | S_ISVTX))) return false;
    access.mode = details.st_mode & 0777;
    access.owner = details.st_uid;
    access.group = details.st_gid;
#if defined(__linux__)
    return ReadAttribute(file.Get(), "system.posix_acl_access", access.acl) &&
        ReadAttribute(file.Get(), "security.selinux", access.label);
#elif defined(__APPLE__)
    return ReadAcl(file.Get(), access.acl);
#else
    return true;
#endif
}


State Inspect(const std::filesystem::path& path, Access& access, std::string* bytes) {
    const Handle file(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!file.Valid()) return errno == ENOENT ? State::Missing : State::Unverifiable;
    if (!Capture(file, access)) return State::Unverifiable;
    if (bytes) {
        char buffer[8192];
        for (;;) {
            const auto count = read(file.Get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return State::Unverifiable;
            if (count == 0) break;
            bytes->append(buffer, static_cast<std::size_t>(count));
        }
        Access after;
        if (!Capture(file, after) || !SameAccess(access, after)) return State::Unverifiable;
    }
    return State::Safe;
}

bool SameAccess(const Access& source, const Access& copy) {
    return source.mode == copy.mode && source.owner == copy.owner && source.group == copy.group &&
        source.acl == copy.acl && source.label == copy.label;
}

inline bool SetAccess(const Handle& file, const Access& original) {
    struct stat details{};
    if (fstat(file.Get(), &details) != 0) return false;
    if ((details.st_uid != original.owner || details.st_gid != original.group) &&
        fchown(file.Get(), original.owner, original.group) != 0) return false;
#if defined(__linux__)
    constexpr const char* name = "system.posix_acl_access";
    if (original.acl.empty()) {
        if (fremovexattr(file.Get(), name) != 0 && errno != ENODATA && errno != ENOTSUP) return false;
    } else if (fsetxattr(file.Get(), name, original.acl.data(), original.acl.size(), 0) != 0) {
        return false;
    }
#elif defined(__APPLE__)
    if (!original.acl.empty()) {
        const auto acl = acl_from_text(original.acl.c_str());
        if (!acl) return false;
        const bool set = acl_set_fd_np(file.Get(), acl, ACL_TYPE_EXTENDED) == 0;
        acl_free(acl);
        if (!set) return false;
    }
#endif
    return fchmod(file.Get(), original.mode) == 0;
}

bool WritePending(const std::filesystem::path& path, std::string_view content,
                         const Access* original, bool& created) {
    Handle file(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!file.Valid()) return false;
    created = true;
    if (original) {
        Access actual;
        if (!SetAccess(file, *original) || !Capture(file, actual) || !SameAccess(*original, actual)) return false;
    }
    while (!content.empty()) {
        const auto written = write(file.Get(), content.data(), std::min<std::size_t>(content.size(), 65536));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    return fsync(file.Get()) == 0 && file.Close();
}

bool Publish(const std::filesystem::path& pending, const std::filesystem::path& target, bool replace) {
    if (replace) return rename(pending.c_str(), target.c_str()) == 0;
    return link(pending.c_str(), target.c_str()) == 0 && unlink(pending.c_str()) == 0;
}

}
