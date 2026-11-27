#pragma once

#include "platform/filesystem.h"

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
#include "platform/protected_config_file_posix.h"
#endif

namespace HarkenSearch::ClientConfigAccess {

#ifdef _WIN32
enum class State { Missing, Safe, Unverifiable };

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

using Platform::FileSystem::ProtectedConfig::Access;
using Platform::FileSystem::ProtectedConfig::Inspect;
using Platform::FileSystem::ProtectedConfig::Publish;
using Platform::FileSystem::ProtectedConfig::SameAccess;
using Platform::FileSystem::ProtectedConfig::State;
using Platform::FileSystem::ProtectedConfig::WritePending;

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
        Platform::FileSystem::RemoveFile(pending.u8string());
    }
    return written;
}

}
