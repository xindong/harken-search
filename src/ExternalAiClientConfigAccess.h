#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/xattr.h>
#elif defined(__APPLE__)
#include <sys/acl.h>
#endif
#endif

namespace HarkenSearch::ClientConfigAccess {

enum class State { Missing, Safe, Unverifiable };

#ifdef _WIN32

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ != INVALID_HANDLE_VALUE; }
    bool Close() {
        if (!Valid()) return true;
        const bool closed = CloseHandle(value_) != 0;
        value_ = INVALID_HANDLE_VALUE;
        return closed;
    }
private:
    HANDLE value_;
};

struct Access {
    Access() = default;
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    ~Access() { if (descriptor) LocalFree(descriptor); }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
};

inline bool Capture(const Handle& file, Access& result) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file.Get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.Get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_READONLY))) return false;
    PACL special = nullptr;
    DWORD revision = 0;
    constexpr SECURITY_INFORMATION fields = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION |
        SCOPE_SECURITY_INFORMATION;
    return GetSecurityInfo(file.Get(), SE_FILE_OBJECT, fields, &result.owner, &result.group,
                           &result.dacl, &special, &result.descriptor) == ERROR_SUCCESS &&
        result.owner && result.group && result.dacl && (!special || special->AceCount == 0) &&
        GetSecurityDescriptorControl(result.descriptor, &result.control, &revision);
}

inline bool SameAccess(const Access& source, const Access& copy);

inline State Inspect(const std::filesystem::path& path, Access& access, std::string* bytes = nullptr) {
    const DWORD desired = bytes ? GENERIC_READ : READ_CONTROL | FILE_READ_ATTRIBUTES;
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_DELETE | (bytes ? 0 : FILE_SHARE_WRITE);
    const Handle file(CreateFileW(path.c_str(), desired, sharing, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.Valid()) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? State::Missing : State::Unverifiable;
    }
    if (!Capture(file, access)) return State::Unverifiable;
    if (bytes) {
        char buffer[8192];
        DWORD count = 0;
        do {
            if (!ReadFile(file.Get(), buffer, sizeof(buffer), &count, nullptr)) return State::Unverifiable;
            bytes->append(buffer, count);
        } while (count != 0);
        Access after;
        if (!Capture(file, after) || !SameAccess(access, after)) return State::Unverifiable;
    }
    return State::Safe;
}

inline bool SameAccess(const Access& source, const Access& copy) {
    if (!EqualSid(source.owner, copy.owner) || !EqualSid(source.group, copy.group) ||
        (source.control & SE_DACL_PROTECTED) != (copy.control & SE_DACL_PROTECTED) ||
        source.dacl->AceCount != copy.dacl->AceCount) return false;
    for (DWORD index = 0; index < source.dacl->AceCount; ++index) {
        void* sourceAce = nullptr;
        void* copyAce = nullptr;
        if (!GetAce(source.dacl, index, &sourceAce) || !GetAce(copy.dacl, index, &copyAce)) return false;
        const auto sourceSize = static_cast<const ACE_HEADER*>(sourceAce)->AceSize;
        const auto copySize = static_cast<const ACE_HEADER*>(copyAce)->AceSize;
        if (sourceSize != copySize || std::memcmp(sourceAce, copyAce, sourceSize) != 0) return false;
    }
    return true;
}

inline bool WritePending(const std::filesystem::path& path, std::string_view content,
                         const Access* original, bool& created) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), original ? original->descriptor : nullptr, FALSE};
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | READ_CONTROL, 0,
                           original ? &attributes : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) return false;
    created = true;
    Access actual;
    if (!Capture(file, actual) || (original && !SameAccess(*original, actual))) return false;
    while (!content.empty()) {
        const auto size = static_cast<DWORD>(std::min<std::size_t>(content.size(), 65536));
        DWORD written = 0;
        if (!WriteFile(file.Get(), content.data(), size, &written, nullptr) || written == 0) return false;
        content.remove_prefix(written);
    }
    return FlushFileBuffers(file.Get()) && file.Close();
}

inline bool Publish(const std::filesystem::path& pending, const std::filesystem::path& target, bool replace) {
    if (replace) return ReplaceFileW(target.c_str(), pending.c_str(), nullptr, 0, nullptr, nullptr) != 0;
    return MoveFileExW(pending.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}

#else

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

struct Access {
    mode_t mode = 0;
    uid_t owner = 0;
    gid_t group = 0;
    std::string acl;
    std::string label;
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

inline bool SameAccess(const Access& source, const Access& copy);

inline State Inspect(const std::filesystem::path& path, Access& access, std::string* bytes = nullptr) {
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

inline bool SameAccess(const Access& source, const Access& copy) {
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

inline bool WritePending(const std::filesystem::path& path, std::string_view content,
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

inline bool Publish(const std::filesystem::path& pending, const std::filesystem::path& target, bool replace) {
    if (replace) return rename(pending.c_str(), target.c_str()) == 0;
    return link(pending.c_str(), target.c_str()) == 0 && unlink(pending.c_str()) == 0;
}

#endif

struct Snapshot {
    Access access;
    State state = State::Unverifiable;
    std::string bytes;
};

inline bool ReadSnapshot(const std::filesystem::path& path, Snapshot& snapshot) {
    snapshot.state = Inspect(path, snapshot.access, &snapshot.bytes);
    return snapshot.state != State::Unverifiable;
}

inline bool BackupMatches(const Snapshot& source, const std::filesystem::path& backup) {
    Access copy;
    return source.state == State::Safe && Inspect(backup, copy) == State::Safe &&
        SameAccess(source.access, copy);
}

inline bool StillMatches(const std::filesystem::path& path, const Snapshot& source) {
    Snapshot current;
    return ReadSnapshot(path, current) && source.state == current.state &&
        (source.state == State::Missing ||
         (SameAccess(source.access, current.access) && source.bytes == current.bytes));
}

inline bool Write(const std::filesystem::path& target, const std::filesystem::path& pending,
                  std::string_view content, bool replace, const Snapshot* source = nullptr) {
    Access existing;
    const auto state = source ? source->state : Inspect(target, existing);
    if (state == State::Unverifiable || (!replace && state != State::Safe)) return false;
    if (source && replace && !StillMatches(target, *source)) return false;
    const auto* original = state == State::Safe ? (source ? &source->access : &existing) : nullptr;
    bool created = false;
    const bool written = WritePending(pending, content, original, created) &&
        (!source || !replace || StillMatches(target, *source)) &&
        Publish(pending, target, replace && state == State::Safe);
    if (created && !written) {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
    }
    return written;
}

}
