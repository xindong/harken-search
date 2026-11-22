#include "diagnostic_log_bundle_file.h"

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Platform::DiagnosticLogBundleFile {
namespace {
namespace fs = std::filesystem;
using Result = HarkenSearch::DiagnosticLogBundleResult;
using Status = HarkenSearch::DiagnosticLogBundleStatus;

struct TemporaryArchive {
    fs::path path;
    bool owned = false;

    ~TemporaryArchive() {
        if (!owned) return;
        std::error_code error;
        fs::remove(path, error);
    }
};

int openExclusive(const fs::path& path, std::FILE*& file) {
#if defined(_WIN32)
    return _wfopen_s(&file, path.c_str(), L"wbx");
#else
    file = std::fopen(path.c_str(), "wbx");
    return file ? 0 : errno;
#endif
}

std::error_code publishNoReplace(const fs::path& temporary, const fs::path& target) {
    std::error_code error;
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
        error.assign(static_cast<int>(GetLastError()), std::system_category());
#else
    fs::create_hard_link(temporary, target, error);
#endif
    return error;
}

Result writeArchive(const fs::path& directory, int64_t clickMs,
                    const void* bytes, size_t size, bool empty) {
    constexpr int kMaxNames = 100;
    const auto base = "harken-diagnostics-" + std::to_string(clickMs);
    const auto filename = [&](int index) {
        return base + (index == 0 ? std::string{} : "-" + std::to_string(index));
    };
    TemporaryArchive temporary;
    for (int index = 0; index < kMaxNames; ++index) {
        temporary.path = directory / (filename(index) + ".tmp");
        std::FILE* file = nullptr;
        const auto error = openExclusive(temporary.path, file);
        if (error == EEXIST) continue;
        if (error != 0 || !file) return {Status::WriteFailed, {}};
        temporary.owned = true;
        const bool written = std::fwrite(bytes, 1, size, file) == size;
        const bool closed = std::fclose(file) == 0;
        if (!written || !closed) return {Status::WriteFailed, {}};
        break;
    }
    if (!temporary.owned) return {Status::NameUnavailable, {}};

    for (int index = 0; index < kMaxNames; ++index) {
        auto target = directory / (filename(index) + ".zip");
        const auto error = publishNoReplace(temporary.path, target);
        if (!error) {
#if defined(_WIN32)
            temporary.owned = false;
#endif
            return {Status::Written, std::move(target), empty};
        }
        if (error != std::errc::file_exists) return {Status::PublishFailed, {}};
    }
    return {Status::NameUnavailable, {}};
}

} // namespace

Result Publish(const fs::path& root, int64_t clickMs, const void* bytes, size_t size, bool empty) noexcept {
    Status failure = Status::InvalidRoot;
    try {
        std::error_code error;
        const auto directory = fs::absolute(root, error);
        if (error) return {failure, {}};
        fs::create_directories(directory, error);
        if (error || !fs::is_directory(directory, error)) return {failure, {}};

        failure = Status::WriteFailed;
        return writeArchive(directory, clickMs, bytes, size, empty);
    } catch (...) {
        return {failure, {}};
    }
}

} // namespace Platform::DiagnosticLogBundleFile
