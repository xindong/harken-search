#ifdef __APPLE__

#include "platform/process_query.h"
#include "platform/system.h"
#include "platform/text_encoding.h"

#include <libproc.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Platform {
namespace {

bool ValidImageName(const std::string& name) {
    return !name.empty() && name.find('\0') == std::string::npos &&
           name.find_first_of("\\/") == std::string::npos &&
           !TextEncoding::Utf8ToWideStrict(name).empty();
}

bool MatchesImageName(std::string_view actual, std::string_view requested) {
    if (TextEncoding::EqualsAsciiCaseInsensitive(actual, requested)) return true;
    return requested.size() > 4 &&
           TextEncoding::EqualsAsciiCaseInsensitive(requested.substr(requested.size() - 4), ".exe") &&
           TextEncoding::EqualsAsciiCaseInsensitive(actual, requested.substr(0, requested.size() - 4));
}

bool ListProcessIds(std::vector<pid_t>& ids) {
    errno = 0;
    const int requiredBytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (requiredBytes <= 0) return false;
    std::size_t capacity = static_cast<std::size_t>(requiredBytes) / sizeof(pid_t) + 128;
    for (int attempt = 0; attempt < 4 && capacity <= INT_MAX / sizeof(pid_t); ++attempt) {
        ids.resize(capacity);
        const int usedBytes = proc_listpids(PROC_ALL_PIDS, 0, ids.data(),
                                           static_cast<int>(capacity * sizeof(pid_t)));
        if (usedBytes < 0) return false;
        if (static_cast<std::size_t>(usedBytes) < capacity * sizeof(pid_t)) {
            ids.resize(static_cast<std::size_t>(usedBytes) / sizeof(pid_t));
            return true;
        }
        capacity *= 2;
    }
    ids.clear();
    errno = EOVERFLOW;
    return false;
}

bool MatchesProcess(pid_t pid, const std::string& name) {
    const auto path = ReadProcessImagePath(static_cast<uint32_t>(pid));
    if (path.status == ProcessImagePathStatus::Success) {
        const std::string actual = std::filesystem::u8path(path.pathUtf8).filename().u8string();
        return MatchesImageName(actual, name);
    }
    // A process may be visible by name even when macOS refuses its full path.
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> processName{};
    if (proc_name(pid, processName.data(), processName.size()) <= 0) return false;
    return MatchesImageName(processName.data(), name);
}

bool GatherMatchingProcessIds(const std::string& name, std::vector<uint32_t>& matches) {
    std::vector<pid_t> ids;
    if (!ListProcessIds(ids)) return false;
    for (pid_t pid : ids) {
        if (pid > 0 && MatchesProcess(pid, name)) {
            matches.push_back(static_cast<uint32_t>(pid));
        }
    }
    return true;
}

} // namespace

ProcessImagePathResult ReadProcessImagePath(uint32_t processId) {
    ProcessImagePathResult result;
    if (processId == 0 || processId > INT_MAX) {
        result.status = ProcessImagePathStatus::InvalidArgument;
        return result;
    }
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    errno = 0;
    const int used = proc_pidpath(static_cast<int>(processId), path.data(), path.size());
    if (used <= 0 || static_cast<std::size_t>(used) >= path.size()) {
        const int error = errno;
        result.status = error == ESRCH || error == ENOENT
            ? ProcessImagePathStatus::NotFound : ProcessImagePathStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(error);
        return result;
    }
    result.pathUtf8 = path.data();
    if (result.pathUtf8.empty() || TextEncoding::Utf8ToWideStrict(result.pathUtf8).empty()) {
        result.pathUtf8.clear();
        result.status = ProcessImagePathStatus::EncodingFailed;
        return result;
    }
    result.status = ProcessImagePathStatus::Success;
    return result;
}

std::vector<uint32_t> FindProcessIdsByImageName(const std::string& imageName) {
    std::vector<uint32_t> matches;
    if (!ValidImageName(imageName)) return matches;
    GatherMatchingProcessIds(imageName, matches);
    return matches;
}

ProcessImageLookupResult FindProcessByImageName(
    const std::string& imageNameUtf8,
    const std::string& preferredExecutablePathUtf8) {
    ProcessImageLookupResult result;
    if (!ValidImageName(imageNameUtf8)) {
        result.status = ProcessImageLookupStatus::InvalidArgument;
        return result;
    }
    if (!preferredExecutablePathUtf8.empty() &&
        TextEncoding::Utf8ToWideStrict(preferredExecutablePathUtf8).empty()) {
        result.status = ProcessImageLookupStatus::EncodingFailed;
        return result;
    }
    std::vector<uint32_t> ids;
    if (!GatherMatchingProcessIds(imageNameUtf8, ids)) {
        result.status = ProcessImageLookupStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(errno ? errno : EIO);
        return result;
    }
    if (ids.empty()) {
        result.status = ProcessImageLookupStatus::NotFound;
        return result;
    }
    const auto preferred = std::filesystem::u8path(preferredExecutablePathUtf8).lexically_normal();
    std::string firstPath;
    for (uint32_t pid : ids) {
        const auto path = ReadProcessImagePath(pid);
        if (path.status != ProcessImagePathStatus::Success) {
            if (!result.systemError) result.systemError = path.systemError;
            continue;
        }
        if (firstPath.empty()) firstPath = path.pathUtf8;
        if (!preferredExecutablePathUtf8.empty() &&
            std::filesystem::u8path(path.pathUtf8).lexically_normal() == preferred) {
            result.status = ProcessImageLookupStatus::Found;
            result.executablePathUtf8 = path.pathUtf8;
            return result;
        }
    }
    result.status = firstPath.empty() ? ProcessImageLookupStatus::QueryFailed
                                      : ProcessImageLookupStatus::Found;
    result.executablePathUtf8 = std::move(firstPath);
    return result;
}

ProcessTerminationResult TerminateProcessById(uint32_t, uint32_t, uint32_t) {
    ProcessTerminationResult result;
    result.status = ProcessTerminationStatus::Unsupported;
    return result;
}

} // namespace Platform

#endif // __APPLE__
