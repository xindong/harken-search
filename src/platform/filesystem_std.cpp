#include "filesystem.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "text_encoding.h"
#ifdef CopyFile
#undef CopyFile
#endif
#endif

namespace Platform::FileSystem {

struct DirectoryIterator::Impl {
    bool recursive = false;
    bool open = false;
    std::filesystem::directory_iterator directory;
    std::filesystem::recursive_directory_iterator recursiveDirectory;
    std::string pendingError;
};

namespace {

void fillDirectoryEntry(const std::filesystem::directory_entry& native,
                        DirectoryEntry& outEntry,
                        std::error_code& ec) {
    outEntry = {};
    outEntry.pathUtf8 = native.path().u8string();
    outEntry.isDirectory = native.is_directory(ec);
    if (ec) return;
    outEntry.isRegularFile = native.is_regular_file(ec);
    if (ec || !outEntry.isRegularFile) return;
    std::error_code sizeError;
    const auto size = native.file_size(sizeError);
    if (!sizeError) outEntry.fileSize = size;
}

void copyError(std::string* errorMessage, const std::string& error) {
    if (errorMessage) *errorMessage = error;
}

} // namespace

DirectoryIterator::DirectoryIterator() : impl_(std::make_unique<Impl>()) {}
DirectoryIterator::~DirectoryIterator() = default;

bool DirectoryIterator::Open(const std::string& rootPathUtf8,
                             bool recursive,
                             bool skipPermissionDenied) {
    Close();
    std::error_code ec;
    const auto options = skipPermissionDenied
        ? std::filesystem::directory_options::skip_permission_denied
        : std::filesystem::directory_options::none;
    try {
        impl_->recursive = recursive;
        if (recursive) {
            impl_->recursiveDirectory = std::filesystem::recursive_directory_iterator(
                std::filesystem::u8path(rootPathUtf8), options, ec);
        } else {
            impl_->directory = std::filesystem::directory_iterator(
                std::filesystem::u8path(rootPathUtf8), options, ec);
        }
    } catch (const std::exception& e) {
        impl_->pendingError = e.what();
        return false;
    }
    if (ec) {
        impl_->pendingError = ec.message();
        return false;
    }
    impl_->open = true;
    return true;
}

DirectoryReadStatus DirectoryIterator::Next(DirectoryEntry& outEntry,
                                             std::string* errorMessage) {
    outEntry = {};
    if (errorMessage) errorMessage->clear();
    if (!impl_->open) return DirectoryReadStatus::End;
    if (!impl_->pendingError.empty()) {
        copyError(errorMessage, impl_->pendingError);
        impl_->pendingError.clear();
        return DirectoryReadStatus::Error;
    }

    std::error_code entryError;
    std::error_code incrementError;
    if (impl_->recursive) {
        if (impl_->recursiveDirectory == std::filesystem::recursive_directory_iterator()) {
            impl_->open = false;
            return DirectoryReadStatus::End;
        }
        const auto native = *impl_->recursiveDirectory;
        fillDirectoryEntry(native, outEntry, entryError);
        impl_->recursiveDirectory.increment(incrementError);
    } else {
        if (impl_->directory == std::filesystem::directory_iterator()) {
            impl_->open = false;
            return DirectoryReadStatus::End;
        }
        const auto native = *impl_->directory;
        fillDirectoryEntry(native, outEntry, entryError);
        impl_->directory.increment(incrementError);
    }
    if (entryError) copyError(errorMessage, entryError.message());
    if (incrementError) impl_->pendingError = incrementError.message();
    if (entryError) return DirectoryReadStatus::Error;
    return DirectoryReadStatus::Entry;
}

bool DirectoryIterator::IsOpen() const {
    return impl_->open;
}

void DirectoryIterator::Close() {
    impl_->directory = std::filesystem::directory_iterator();
    impl_->recursiveDirectory = std::filesystem::recursive_directory_iterator();
    impl_->pendingError.clear();
    impl_->open = false;
}

struct StreamWriter::Impl {
    std::ofstream stream;
};

StreamWriter::StreamWriter() : impl_(std::make_unique<Impl>()) {}
StreamWriter::~StreamWriter() = default;

bool StreamWriter::Open(const std::string& pathUtf8, bool append, bool binary) {
    Close();
    std::ios::openmode mode = std::ios::out | (append ? std::ios::app : std::ios::trunc);
    if (binary) mode |= std::ios::binary;
    impl_->stream.open(std::filesystem::u8path(pathUtf8), mode);
    return impl_->stream.is_open();
}

bool StreamWriter::Write(std::string_view bytes) {
    if (!impl_->stream.is_open()) return false;
    impl_->stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return impl_->stream.good();
}

bool StreamWriter::Flush() {
    if (!impl_->stream.is_open()) return false;
    impl_->stream.flush();
    return impl_->stream.good();
}

std::int64_t StreamWriter::Tell() const {
    if (!impl_->stream.is_open()) return -1;
    auto& stream = impl_->stream;
    stream.clear();
    stream.seekp(0, std::ios::end);
    if (!stream.good()) return -1;
    const auto position = stream.tellp();
    if (position == std::streampos(std::streamoff(-1))) return -1;
    return static_cast<std::int64_t>(position);
}

void StreamWriter::Close() {
    (void)CloseChecked();
}

bool StreamWriter::CloseChecked() {
    if (!impl_->stream.is_open()) return false;
    impl_->stream.close();
    const bool succeeded = !impl_->stream.fail();
    impl_->stream.clear();
    return succeeded;
}

bool StreamWriter::IsOpen() const {
    return impl_->stream.is_open();
}

struct StreamReader::Impl {
    std::ifstream stream;
};

StreamReader::StreamReader() : impl_(std::make_unique<Impl>()) {}
StreamReader::~StreamReader() = default;
StreamReader::StreamReader(StreamReader&&) noexcept = default;
StreamReader& StreamReader::operator=(StreamReader&&) noexcept = default;

bool StreamReader::Open(const std::string& pathUtf8, bool binary) {
    Close();
    std::ios::openmode mode = std::ios::in;
    if (binary) mode |= std::ios::binary;
    try {
        impl_->stream.open(std::filesystem::u8path(pathUtf8), mode);
    } catch (...) {
        return false;
    }
    return impl_->stream.is_open();
}

size_t StreamReader::Read(void* destination, size_t bytes) {
    if (!impl_->stream.is_open() || !destination || bytes == 0) return 0;
    impl_->stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return static_cast<size_t>(impl_->stream.gcount());
}

bool StreamReader::Seek(int64_t offset, StreamSeekOrigin origin) {
    if (!impl_->stream.is_open()) return false;
    const auto direction = origin == StreamSeekOrigin::Begin
        ? std::ios::beg
        : (origin == StreamSeekOrigin::Current ? std::ios::cur : std::ios::end);
    impl_->stream.seekg(static_cast<std::streamoff>(offset), direction);
    return impl_->stream.good();
}

int64_t StreamReader::Tell() const {
    if (!impl_->stream.is_open()) return -1;
    const auto position = impl_->stream.tellg();
    return position < 0 ? -1 : static_cast<int64_t>(position);
}

bool StreamReader::Good() const {
    return impl_->stream.good();
}

void StreamReader::Clear() {
    impl_->stream.clear();
}

void StreamReader::Close() {
    if (impl_->stream.is_open()) impl_->stream.close();
    impl_->stream.clear();
}

bool StreamReader::IsOpen() const {
    return impl_->stream.is_open();
}

bool CanOpenFileForRead(const std::string& pathUtf8) {
    FILE* file = nullptr;
#ifdef _MSC_VER
    fopen_s(&file, pathUtf8.c_str(), "rb");
#else
    file = std::fopen(pathUtf8.c_str(), "rb");
#endif
    if (!file) return false;
    std::fclose(file);
    return true;
}

bool Exists(const std::string& pathUtf8) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(std::filesystem::u8path(pathUtf8), ec);
    return exists && !ec;
}

#ifndef _WIN32
bool IsNetworkPath(const std::string& pathUtf8) {
    if (pathUtf8.size() >= 2 && pathUtf8[0] == '/' && pathUtf8[1] == '/') {
        return true;
    }
    if (pathUtf8.size() >= 6) {
        const auto prefix = pathUtf8.substr(0, 6);
        if (prefix == "smb://" || prefix == "SMB://") {
            return true;
        }
    }
    return false;
}

PathStatus ProbePath(const std::string& pathUtf8, std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    try {
        std::error_code ec;
        const bool exists = std::filesystem::exists(std::filesystem::u8path(pathUtf8), ec);
        if (ec) {
            if (errorMessage) *errorMessage = ec.message();
            return PathStatus::Error;
        }
        return exists ? PathStatus::Present : PathStatus::Missing;
    } catch (const std::exception& error) {
        if (errorMessage) *errorMessage = error.what();
        return PathStatus::Error;
    }
}
#endif

#ifndef _WIN32
PathAttributes QueryPathAttributes(const std::string&) {
    return {};
}
#endif

bool IsDirectory(const std::string& pathUtf8) {
    std::error_code ec;
    const bool isDirectory = std::filesystem::is_directory(std::filesystem::u8path(pathUtf8), ec);
    return isDirectory && !ec;
}

bool CreateDirectories(const std::string& pathUtf8) {
    std::error_code ec;
    const bool created = std::filesystem::create_directories(std::filesystem::u8path(pathUtf8), ec);
    return created && !ec;
}

bool EnsureDirectoryExists(const std::string& pathUtf8) {
    const auto path = std::filesystem::u8path(pathUtf8);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return false;
    const bool isDirectory = std::filesystem::is_directory(path, ec);
    return isDirectory && !ec;
}

RegularFileInfo ProbeRegularFile(const std::string& pathUtf8) {
    RegularFileInfo info;
    const auto path = std::filesystem::u8path(pathUtf8);
    std::error_code ec;
    info.isRegularFile = std::filesystem::is_regular_file(path, ec);
    if (ec || !info.isRegularFile) return info;
    info.sizeBytes = std::filesystem::file_size(path, ec);
    if (ec) {
        info.sizeBytes = std::numeric_limits<uint64_t>::max();
        return info;
    }
    info.sizeAvailable = true;
    return info;
}

bool CopyFile(const std::string& sourcePathUtf8,
              const std::string& targetPathUtf8,
              bool overwrite,
              std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    std::error_code ec;
    const auto options = overwrite ? std::filesystem::copy_options::overwrite_existing
                                   : std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(std::filesystem::u8path(sourcePathUtf8),
                               std::filesystem::u8path(targetPathUtf8), options, ec);
    if (!ec) return true;
    if (errorMessage) *errorMessage = ec.message();
    return false;
}

bool Rename(const std::string& sourcePathUtf8,
            const std::string& targetPathUtf8,
            std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(sourcePathUtf8),
                            std::filesystem::u8path(targetPathUtf8), ec);
    if (!ec) return true;
    if (errorMessage) *errorMessage = ec.message();
    return false;
}

bool RemoveFile(const std::string& pathUtf8, std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    std::error_code ec;
    const bool removed = std::filesystem::remove(std::filesystem::u8path(pathUtf8), ec);
    if (removed && !ec) return true;
    if (errorMessage && ec) *errorMessage = ec.message();
    return false;
}

CopyFileResult CopyFileWithSystemError(const std::string& sourcePathUtf8,
                                       const std::string& targetPathUtf8,
                                       bool overwrite) noexcept {
    CopyFileResult result;
#ifdef _WIN32
    const std::wstring source = Platform::TextEncoding::Utf8ToWidePermissive(sourcePathUtf8);
    const std::wstring target = Platform::TextEncoding::Utf8ToWidePermissive(targetPathUtf8);
    if (::CopyFileW(source.c_str(), target.c_str(), overwrite ? FALSE : TRUE)) {
        result.success = true;
        return result;
    }
    result.systemError = static_cast<std::uint32_t>(::GetLastError());
    result.errorMessage = "CopyFileW failed";
    return result;
#else
    std::error_code ec;
    const auto options = overwrite ? std::filesystem::copy_options::overwrite_existing
                                   : std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(std::filesystem::u8path(sourcePathUtf8),
                               std::filesystem::u8path(targetPathUtf8), options, ec);
    result.success = !ec;
    if (ec) {
        result.systemError = static_cast<std::uint32_t>(ec.value());
        result.errorMessage = ec.message();
    }
    return result;
#endif
}

bool RemoveAll(const std::string& pathUtf8, std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(pathUtf8), ec);
    if (!ec) return true;
    if (errorMessage) *errorMessage = ec.message();
    return false;
}

bool ResizeFile(const std::string& pathUtf8,
                uint64_t sizeBytes,
                std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    std::error_code ec;
    std::filesystem::resize_file(std::filesystem::u8path(pathUtf8), sizeBytes, ec);
    if (!ec) return true;
    if (errorMessage) *errorMessage = ec.message();
    return false;
}

bool Equivalent(const std::string& leftPathUtf8,
                const std::string& rightPathUtf8) {
    try {
        std::error_code ec;
        const bool equivalent = std::filesystem::equivalent(
            std::filesystem::u8path(leftPathUtf8),
            std::filesystem::u8path(rightPathUtf8), ec);
        return equivalent && !ec;
    } catch (...) {
        return false;
    }
}

std::string CurrentDirectory() {
    std::error_code ec;
    const auto path = std::filesystem::current_path(ec);
    return ec ? std::string() : path.u8string();
}

std::string WeaklyCanonical(const std::string& pathUtf8) {
    std::error_code ec;
    const auto path = std::filesystem::weakly_canonical(std::filesystem::u8path(pathUtf8), ec);
    return ec ? std::string() : path.u8string();
}

std::string RelativePath(const std::string& targetPathUtf8,
                         const std::string& basePathUtf8) {
    std::error_code ec;
    const auto path = std::filesystem::relative(std::filesystem::u8path(targetPathUtf8),
                                                std::filesystem::u8path(basePathUtf8), ec);
    return ec ? std::string() : path.generic_string();
}

TemporaryDirectoryResult QueryTemporaryDirectory() {
    TemporaryDirectoryResult result;
    std::error_code ec;
    const auto path = std::filesystem::temp_directory_path(ec);
    if (ec || path.empty()) {
        result.error = ec;
        return result;
    }
    result.status = TemporaryDirectoryStatus::Success;
    result.pathUtf8 = path.u8string();
    result.error.clear();
    return result;
}

std::string TemporaryDirectory() {
    const TemporaryDirectoryResult result = QueryTemporaryDirectory();
    if (result.status != TemporaryDirectoryStatus::Success) return {};
    return result.pathUtf8;
}

std::vector<uint8_t> ReadFileBytes(const std::string& pathUtf8) {
    return ReadFileBytes(pathUtf8, static_cast<size_t>(-1));
}

std::vector<uint8_t> ReadFileBytes(const std::string& pathUtf8, size_t maxBytes) {
    std::ifstream input(std::filesystem::u8path(pathUtf8), std::ios::binary | std::ios::ate);
    if (!input.is_open()) return {};
    const auto size = static_cast<size_t>(input.tellg());
    if (size == 0 || size > maxBytes) return {};
    input.seekg(0);
    std::vector<uint8_t> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) return {};
    return bytes;
}

int64_t LastWriteTimeUnixSeconds(const std::string& pathUtf8) {
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(std::filesystem::u8path(pathUtf8), ec);
    if (ec) return 0;
    const auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return sctp.time_since_epoch().count();
}

bool ReadFileText(const std::string& pathUtf8, std::string& outText) {
    std::ifstream input(std::filesystem::u8path(pathUtf8));
    if (!input.is_open()) return false;
    outText.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileText(const std::string& pathUtf8, const std::string& text) {
    std::ofstream output(std::filesystem::u8path(pathUtf8), std::ios::out | std::ios::trunc);
    if (!output.is_open()) return false;
    output << text;
    return output.good();
}

bool AppendFileText(const std::string& pathUtf8, const std::string& text) {
    std::ofstream output(std::filesystem::u8path(pathUtf8), std::ios::out | std::ios::app);
    if (!output.is_open()) return false;
    output << text;
    return output.good();
}

#ifndef _WIN32
bool AtomicFileReplace(const std::string& targetPathUtf8, const std::string& sourcePathUtf8) {
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(sourcePathUtf8),
                            std::filesystem::u8path(targetPathUtf8), ec);
    return !ec;
}
#endif

} // namespace Platform::FileSystem
