#include "platform/update_persistence_port.h"
#include "platform/update_bootstrap_authority_port.h"
#include "update/UpdateReleaseConfig.h"

#ifdef ARIS_UPDATE_PERSISTENCE_TESTING
#include "platform/update_persistence_port_test_hooks.h"
#endif

#ifdef _WIN32

#include "platform/text_encoding.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winternl.h>

#include "nlohmann_json.hpp"
#if defined(ARIS_UPDATE_RELEASE_NOTES)
#include "platform/miniz_archive_reader.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OBJ_DONT_REPARSE
#define OBJ_DONT_REPARSE 0x00001000L
#endif

namespace Platform::UpdatePersistence {
namespace {

using aris::update::DownloadCleanupIntent;
using aris::update::DownloadCleanupIntentKind;
using aris::update::StagedIdentityObservation;
using aris::update::StagedIdentityStatus;
using aris::update::StoreIntent;
using aris::update::StoreIntentKind;
using aris::update::TransferAppendIntent;
using aris::update::ResumeMetadataIntent;
using aris::update::ResumeMetadataObservation;
using aris::update::ResumeMetadataStatus;
using aris::update::UpdateDownloadManager;
using aris::update::UpdatePersistenceError;
using aris::update::UpdatePersistenceResult;
using aris::update::UpdateVersionDirectoryListing;

constexpr ULONG kFileDirectoryFile = 0x00000001;
constexpr ULONG kFileWriteThrough = 0x00000002;
constexpr ULONG kFileSequentialOnly = 0x00000004;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020;
constexpr ULONG kFileNonDirectoryFile = 0x00000040;
constexpr ULONG kFileOpen = 0x00000001;
constexpr ULONG kFileOpenIf = 0x00000003;
constexpr ULONG kFileOverwriteIf = 0x00000005;
constexpr NTSTATUS kStatusObjectNameNotFound =
    static_cast<NTSTATUS>(0xC0000034L);
constexpr NTSTATUS kStatusObjectPathNotFound =
    static_cast<NTSTATUS>(0xC000003AL);
constexpr NTSTATUS kStatusNoMoreFiles = static_cast<NTSTATUS>(0x80000006L);
constexpr NTSTATUS kStatusBufferOverflow = static_cast<NTSTATUS>(0x80000005L);
constexpr NTSTATUS kStatusNotADirectory = static_cast<NTSTATUS>(0xC0000103L);

constexpr ULONG kMaximumRetentionDepth = 4;
constexpr std::size_t kMaximumRetentionEntriesPerDirectory = 4096;

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) Reset(other.release());
        return *this;
    }
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }
    void Reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (valid()) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class ScopedAlgorithmProvider {
public:
    explicit ScopedAlgorithmProvider(BCRYPT_ALG_HANDLE handle) : handle_(handle) {}
    ~ScopedAlgorithmProvider() {
        if (handle_) BCryptCloseAlgorithmProvider(handle_, 0);
    }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class ScopedHash {
public:
    explicit ScopedHash(BCRYPT_HASH_HANDLE handle) : handle_(handle) {}
    ~ScopedHash() {
        if (handle_) BCryptDestroyHash(handle_);
    }
    BCRYPT_HASH_HANDLE get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

class ScopedFileLock {
public:
    explicit ScopedFileLock(HANDLE handle) : handle_(handle) {
        locked_ = LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK |
                                          LOCKFILE_FAIL_IMMEDIATELY,
                             0, MAXDWORD, MAXDWORD, &overlapped_) != FALSE;
        if (!locked_) error_ = GetLastError();
    }
    ~ScopedFileLock() {
        if (locked_) UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
    }
    bool locked() const { return locked_; }
    DWORD error() const { return error_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_{};
    bool locked_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(
    HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
using NtQueryDirectoryFileFunction = NTSTATUS(NTAPI*)(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
    FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);

struct NativeFunctions {
    NtCreateFileFunction createFile = nullptr;
    NtSetInformationFileFunction setInformationFile = nullptr;
    NtQueryDirectoryFileFunction queryDirectoryFile = nullptr;
};

const NativeFunctions& NativeApi() {
    static const NativeFunctions functions = [] {
        NativeFunctions result;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return result;
        result.createFile = reinterpret_cast<NtCreateFileFunction>(
            GetProcAddress(ntdll, "NtCreateFile"));
        result.setInformationFile = reinterpret_cast<NtSetInformationFileFunction>(
            GetProcAddress(ntdll, "NtSetInformationFile"));
        result.queryDirectoryFile = reinterpret_cast<NtQueryDirectoryFileFunction>(
            GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        return result;
    }();
    return functions;
}

struct NativeFileRenameInformation {
    BOOLEAN replaceIfExists = FALSE;
    HANDLE rootDirectory = nullptr;
    ULONG fileNameLength = 0;
    WCHAR fileName[1]{};
};

// FileNamesInformation (class 12) is the stable enumeration reply: names
// only, with the kind of each entry classified afterwards through the same
// non-following relative-open guards used by every other port operation.
struct NativeFileNameInformation {
    ULONG nextEntryOffset = 0;
    ULONG fileIndex = 0;
    ULONG fileNameLength = 0;
    WCHAR fileName[1]{};
};

constexpr FILE_INFORMATION_CLASS kFileRenameInformation =
    static_cast<FILE_INFORMATION_CLASS>(10);
constexpr FILE_INFORMATION_CLASS kFileNamesInformation =
    static_cast<FILE_INFORMATION_CLASS>(12);

std::string NormalizeSeparators(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

bool IsSafeSegment(const std::string& value) {
    if (value.empty() || value.size() > 255 || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    });
}

bool IsSafeRelativePath(const std::string& input) {
    if (input.empty() || input.size() > 4096) return false;
    const std::string value = NormalizeSeparators(input);
    if (value.front() == '/' || value.find(':') != std::string::npos) return false;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string segment = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!IsSafeSegment(segment)) return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F');
           });
}

std::vector<std::string> Segments(const std::string& relativePath) {
    std::vector<std::string> result;
    const std::string normalized = NormalizeSeparators(relativePath);
    std::size_t begin = 0;
    while (begin < normalized.size()) {
        const std::size_t end = normalized.find('/', begin);
        result.push_back(normalized.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string JoinSegments(const std::vector<std::string>& segments,
                         std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        if (!result.empty()) result += '/';
        result += segments[index];
    }
    return result;
}

UpdatePersistenceResult Failure(UpdatePersistenceError error,
                                std::uint32_t systemError = 0,
                                std::size_t completed = 0) {
    UpdatePersistenceResult result;
    result.error = error;
    result.systemError = systemError;
    result.completedIntents = completed;
    return result;
}

bool IsSafeDirectoryHandle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &info, sizeof(info))) {
        return false;
    }
    return (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsSafeRegularFileHandle(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &info, sizeof(info))) {
        return false;
    }
    return (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsMissingStatus(NTSTATUS status) {
    return status == kStatusObjectNameNotFound ||
           status == kStatusObjectPathNotFound;
}

struct NativeOpenResult {
    NTSTATUS status = static_cast<NTSTATUS>(0xC0000001L);
    ScopedHandle handle;

    bool ok() const { return status >= 0 && handle.valid(); }
    bool missing() const { return IsMissingStatus(status); }
};

NativeOpenResult OpenRelative(HANDLE root,
                              const std::string& relativePath,
                              ACCESS_MASK access,
                              ULONG share,
                              ULONG disposition,
                              ULONG options,
                              ULONG attributes = FILE_ATTRIBUTE_NORMAL) {
    NativeOpenResult result;
    const auto& native = NativeApi();
    if (!native.createFile || !IsSafeRelativePath(relativePath)) {
        result.status = static_cast<NTSTATUS>(0xC000000DL);
        return result;
    }
    std::wstring wide = Platform::TextEncoding::Utf8ToWideStrict(
        NormalizeSeparators(relativePath));
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    if (wide.empty() || wide.size() >
                            (std::numeric_limits<USHORT>::max)() / sizeof(wchar_t)) {
        result.status = static_cast<NTSTATUS>(0xC000000DL);
        return result;
    }
    UNICODE_STRING name{};
    name.Buffer = wide.data();
    name.Length = static_cast<USHORT>(wide.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES object{};
    object.Length = sizeof(object);
    object.RootDirectory = root;
    object.ObjectName = &name;
    object.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
    IO_STATUS_BLOCK io{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    result.status = native.createFile(
        &raw, access | SYNCHRONIZE, &object, &io, nullptr, attributes,
        share, disposition, options | kFileSynchronousIoNonAlert,
        nullptr, 0);
    if (result.status >= 0) result.handle = ScopedHandle(raw);
    return result;
}

struct ParentHandleResult {
    bool success = false;
    bool invalid = false;
    NTSTATUS status = 0;
    ScopedHandle handle;
    std::string fileName;
};

ParentHandleResult OpenParent(HANDLE root,
                              const std::string& relativePath,
                              bool createParents) {
    ParentHandleResult result;
    if (!IsSafeRelativePath(relativePath)) {
        result.invalid = true;
        return result;
    }
    const auto segments = Segments(relativePath);
    if (segments.empty()) {
        result.invalid = true;
        return result;
    }
    result.fileName = segments.back();
    if (segments.size() == 1) {
        result.success = true;
        return result;
    }

    NativeOpenResult opened;
    for (std::size_t count = 1; count < segments.size(); ++count) {
        const std::string parent = JoinSegments(segments, count);
        opened = OpenRelative(
            root, parent,
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE |
                FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            createParents ? kFileOpenIf : kFileOpen,
            kFileDirectoryFile);
        if (!opened.ok()) {
            result.status = opened.status;
            return result;
        }
        if (!IsSafeDirectoryHandle(opened.handle.get())) {
            result.invalid = true;
            result.status = static_cast<NTSTATUS>(ERROR_REPARSE_TAG_INVALID);
            return result;
        }
    }
    result.handle = std::move(opened.handle);
    result.success = true;
    return result;
}

struct HashAndSizeResult {
    bool success = false;
    std::uint64_t size = 0;
    std::string sha256;
    DWORD systemError = ERROR_SUCCESS;
};

HashAndSizeResult HashAndSizeFromHandle(HANDLE file) {
    HashAndSizeResult result;
    ScopedFileLock lock(file);
    if (!lock.locked()) {
        result.systemError = lock.error();
        return result;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        result.systemError = GetLastError();
        return result;
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        result.systemError = GetLastError();
        return result;
    }

    BCRYPT_ALG_HANDLE rawProvider = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &rawProvider, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        result.systemError = static_cast<DWORD>(status);
        return result;
    }
    ScopedAlgorithmProvider provider(rawProvider);
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD copied = 0;
    status = BCryptGetProperty(provider.get(), BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectBytes),
                               sizeof(objectBytes), &copied, 0);
    if (status < 0) {
        result.systemError = static_cast<DWORD>(status);
        return result;
    }
    status = BCryptGetProperty(provider.get(), BCRYPT_HASH_LENGTH,
                               reinterpret_cast<PUCHAR>(&hashBytes),
                               sizeof(hashBytes), &copied, 0);
    if (status < 0 || hashBytes == 0) {
        result.systemError = static_cast<DWORD>(status);
        return result;
    }
    std::vector<unsigned char> object(objectBytes);
    std::vector<unsigned char> digest(hashBytes);
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    status = BCryptCreateHash(provider.get(), &rawHash, object.data(), objectBytes,
                              nullptr, 0, 0);
    if (status < 0) {
        result.systemError = static_cast<DWORD>(status);
        return result;
    }
    ScopedHash hash(rawHash);
    std::vector<unsigned char> buffer(64 * 1024);
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &bytesRead, nullptr)) {
            result.systemError = GetLastError();
            return result;
        }
        if (bytesRead == 0) break;
        status = BCryptHashData(hash.get(), buffer.data(), bytesRead, 0);
        if (status < 0) {
            result.systemError = static_cast<DWORD>(status);
            return result;
        }
    }
    status = BCryptFinishHash(hash.get(), digest.data(), hashBytes, 0);
    if (status < 0) {
        result.systemError = static_cast<DWORD>(status);
        return result;
    }
    static constexpr char hex[] = "0123456789abcdef";
    result.sha256.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result.sha256[index * 2] = hex[digest[index] >> 4];
        result.sha256[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    result.size = static_cast<std::uint64_t>(size.QuadPart);
    result.success = true;
    return result;
}

bool SameTransfer(const aris::update::HttpTransferIdentity& left,
                  const aris::update::HttpTransferIdentity& right) {
    return !left.flightKey.empty() && left.flightKey == right.flightKey &&
           left.generation != 0 && left.generation == right.generation &&
           left.partPath == right.partPath;
}

class SecureWindowsAdapter final : public aris::update::UpdatePersistencePort {
public:
    explicit SecureWindowsAdapter(ScopedHandle trustedRoot)
        : trustedRoot_(std::move(trustedRoot)) {}

    UpdatePersistenceResult ExecuteStoreIntents(
        const std::vector<StoreIntent>& intents) override {
        if (!ValidateStorePlan(intents)) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        std::size_t completed = 0;
        for (std::size_t index = 0; index < intents.size(); ++index) {
            const auto& intent = intents[index];
            if (intent.kind == StoreIntentKind::WriteTempFile) {
                const auto result = WriteAndRename(
                    intent, intents[index + 1], completed);
                if (!result.ok()) return result;
                completed += 2;
                ++index;
                continue;
            }
            const auto result = ExecuteStoreIntent(intent, completed);
            if (!result.ok()) return result;
            ++completed;
        }
        return Failure(UpdatePersistenceError::None, 0, completed);
    }

    UpdatePersistenceResult ExecuteDownloadIntents(
        const std::vector<DownloadCleanupIntent>& intents) override {
        for (const auto& intent : intents) {
            if (!ValidateDownloadIntent(intent)) {
                return Failure(UpdatePersistenceError::InvalidIntent);
            }
        }
        std::size_t completed = 0;
        for (const auto& intent : intents) {
            const auto result = ExecuteDownloadIntent(intent, completed);
            if (!result.ok()) return result;
            ++completed;
        }
        return Failure(UpdatePersistenceError::None, 0, completed);
    }

    StagedIdentityObservation ObserveStagedPatch(
        const std::string& relativePath) override {
        StagedIdentityObservation result;
        if (!IsSafeRelativePath(relativePath)) {
            result.status = StagedIdentityStatus::InvalidPath;
            return result;
        }
        const auto opened = OpenRelative(
            trustedRoot_.get(), relativePath,
            GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, kFileOpen,
            kFileNonDirectoryFile | kFileSequentialOnly);
        if (!opened.ok()) {
            if (opened.missing()) {
                result.status = StagedIdentityStatus::Missing;
                result.identity.relativePath = NormalizeSeparators(relativePath);
            } else {
                result.status = StagedIdentityStatus::ProbeFailed;
                result.systemError = static_cast<std::uint32_t>(opened.status);
            }
            return result;
        }
        if (!IsSafeRegularFileHandle(opened.handle.get())) {
            result.status = StagedIdentityStatus::NotRegularFile;
            return result;
        }
        const auto observed = HashAndSizeFromHandle(opened.handle.get());
        if (!observed.success) {
            result.status = StagedIdentityStatus::HashFailed;
            result.systemError = observed.systemError;
            return result;
        }
        result.status = StagedIdentityStatus::Present;
        result.identity.exists = true;
        result.identity.relativePath = NormalizeSeparators(relativePath);
        result.identity.size = observed.size;
        result.identity.sha256 = observed.sha256;
        return result;
    }

#if defined(ARIS_UPDATE_RELEASE_NOTES)
    bool ReadReadyReleaseNotes(const std::string& relativePath,
        std::uint64_t expectedSize, const std::string& expectedSha256,
        std::string& json) override {
        json.clear();
        if (!IsSafeRelativePath(relativePath) || !IsSha256(expectedSha256) ||
            expectedSize == 0 || !EndsWith(relativePath, ".zip")) return false;
        auto file = OpenRelative(trustedRoot_.get(), relativePath,
            GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, kFileOpen,
            kFileNonDirectoryFile | kFileSynchronousIoNonAlert);
        if (!file.ok() || !IsSafeRegularFileHandle(file.handle.get())) return false;
        // Verify and read through the SAME non-writable shared handle. The
        // description cannot race replacement of the archive we just checked.
        const auto observed = HashAndSizeFromHandle(file.handle.get());
        if (!observed.success || observed.size != expectedSize || observed.sha256 != expectedSha256)
            return false;
        struct Context {
            HANDLE handle;
            std::uint64_t size;
            std::string text;
        } context{file.handle.get(), expectedSize, {}};
        mz_zip_archive zip{};
        zip.m_pIO_opaque = &context;
        zip.m_pRead = [](void* opaque, mz_uint64 offset, void* buffer, size_t count) -> size_t {
            auto& input = *static_cast<Context*>(opaque);
            if (offset > input.size || count > input.size - offset || count > MAXDWORD) return 0;
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(input.handle, position, nullptr, FILE_BEGIN)) return 0;
            DWORD read = 0;
            if (!ReadFile(input.handle, buffer, static_cast<DWORD>(count), &read, nullptr)) return 0;
            return read;
        };
        if (!mz_zip_reader_init(&zip, expectedSize, 0)) return false;
        constexpr mz_uint caseSensitive = 0x0100;
        const int index = mz_zip_reader_locate_file(&zip, "aris_release_notes.json", nullptr, caseSensitive);
        bool ok = false;
        if (index >= 0 && zip.m_total_files <= 8192U) {
            ok = mz_zip_reader_extract_file_to_callback(&zip, "aris_release_notes.json",
                [](void* opaque, mz_uint64 offset, const void* bytes, size_t count) -> size_t {
                    auto& output = *static_cast<Context*>(opaque);
                    constexpr size_t limit = 8U * 1024U * 1024U;
                    if (offset != output.text.size() || count > limit - output.text.size()) return 0;
                    output.text.append(static_cast<const char*>(bytes), count);
                    return count;
                }, &context, caseSensitive) != 0;
        }
        mz_zip_reader_end(&zip);
        if (!ok || context.text.empty()) return false;
        json = std::move(context.text);
        return true;
    }
#endif

    UpdatePersistenceResult ActivateTransfer(
        const aris::update::DownloadFlightState& activeFlight,
        const aris::update::DownloadFlightToken& token,
        const aris::update::HttpTransferIdentity& identity) override {
        if (activeFlight.activeKey != token.key() ||
            activeFlight.generation != token.generation() ||
            identity.flightKey != token.key() ||
            identity.generation != token.generation() ||
            !IsSafeRelativePath(identity.partPath) ||
            !EndsWith(identity.partPath, ".part")) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        std::lock_guard<std::mutex> lock(transferMutex_);
        activeTransfer_ = identity;
        return Failure(UpdatePersistenceError::None, 0, 1);
    }

    UpdatePersistenceResult ExecuteTransferChunk(
        const TransferAppendIntent& append,
        const ResumeMetadataIntent& metadata) override {
        if (append.bytes.empty() ||
            append.offset > (std::numeric_limits<std::uint64_t>::max)() -
                                append.bytes.size() ||
            !metadata.metadata.expectedSize ||
            metadata.metadata.downloadedBytes !=
                append.offset + append.bytes.size()) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        std::lock_guard<std::mutex> lock(transferMutex_);
        if (!activeTransfer_ || append.partPath != append.identity.partPath ||
            !EndsWith(append.partPath, ".part") ||
            !SameTransfer(*activeTransfer_, append.identity) ||
            !SameTransfer(append.identity, metadata.identity) ||
            metadata.metaPath != append.partPath.substr(
                                     0, append.partPath.size() - 5) +
                                     ".meta.json") {
            return Failure(UpdatePersistenceError::StaleGeneration);
        }
        const auto appended = AppendTransferChunk(append);
        if (!appended.ok()) return appended;
        return PersistResumeMetadata(metadata);
    }

    UpdatePersistenceResult DeactivateTransfer(
        const aris::update::DownloadFlightToken& token,
        const aris::update::HttpTransferIdentity& identity) override {
        std::lock_guard<std::mutex> lock(transferMutex_);
        if (!activeTransfer_) {
            return Failure(UpdatePersistenceError::None, 0, 1);
        }
        if (token.key() != identity.flightKey ||
            token.generation() != identity.generation ||
            !SameTransfer(*activeTransfer_, identity)) {
            return Failure(UpdatePersistenceError::StaleGeneration);
        }
        activeTransfer_.reset();
        return Failure(UpdatePersistenceError::None, 0, 1);
    }

    ResumeMetadataObservation ObserveResumeMetadata(
        const std::string& relativePath) override {
        ResumeMetadataObservation result;
        if (!IsSafeRelativePath(relativePath) ||
            !EndsWith(relativePath, ".meta.json")) {
            result.status = ResumeMetadataStatus::InvalidPath;
            return result;
        }
        auto file = OpenRelative(
            trustedRoot_.get(), relativePath, GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, kFileOpen,
            kFileNonDirectoryFile | kFileSequentialOnly);
        if (!file.ok()) {
            if (file.missing()) {
                result.status = ResumeMetadataStatus::Missing;
            } else {
                result.status = ResumeMetadataStatus::ReadFailed;
                result.systemError = static_cast<std::uint32_t>(file.status);
            }
            return result;
        }
        if (!IsSafeRegularFileHandle(file.handle.get())) {
            result.status = ResumeMetadataStatus::InvalidPath;
            return result;
        }
        LARGE_INTEGER size{};
        constexpr std::uint64_t kMaximumMetadataBytes = 1024 * 1024;
        if (!GetFileSizeEx(file.handle.get(), &size) || size.QuadPart <= 0 ||
            static_cast<std::uint64_t>(size.QuadPart) > kMaximumMetadataBytes) {
            result.status = ResumeMetadataStatus::Malformed;
            return result;
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>((std::min)(
                bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            if (!ReadFile(file.handle.get(), bytes.data() + offset,
                          requested, &read, nullptr) || read == 0) {
                result.status = ResumeMetadataStatus::ReadFailed;
                result.systemError = GetLastError();
                return result;
            }
            offset += read;
        }
        const auto value = nlohmann::ordered_json::parse(
            bytes.begin(), bytes.end(), nullptr, false);
        if (value.is_discarded() || !value.is_object() || value.size() != 6 ||
            !value.contains("downloadedBytes") || !value.contains("etag") ||
            !value.contains("expectedSha256") || !value.contains("expectedSize") ||
            !value.contains("lastModified") || !value.contains("url") ||
            !value["downloadedBytes"].is_number_unsigned() ||
            !value["expectedSize"].is_number_unsigned() ||
            !value["etag"].is_string() ||
            !value["expectedSha256"].is_string() ||
            !value["lastModified"].is_string() || !value["url"].is_string()) {
            result.status = ResumeMetadataStatus::Malformed;
            return result;
        }
        result.metadata.downloadedBytes = value["downloadedBytes"].get<std::uint64_t>();
        result.metadata.expectedSize = value["expectedSize"].get<std::uint64_t>();
        result.metadata.etag = value["etag"].get<std::string>();
        result.metadata.expectedSha256 = value["expectedSha256"].get<std::string>();
        result.metadata.lastModified = value["lastModified"].get<std::string>();
        result.metadata.url = value["url"].get<std::string>();
        if (*result.metadata.expectedSize == 0 ||
            result.metadata.downloadedBytes > *result.metadata.expectedSize ||
            !IsSha256(result.metadata.expectedSha256) ||
            result.metadata.url.rfind(
                ARIS_UPDATE_GITHUB_RELEASE_DOWNLOAD_PREFIX, 0) != 0) {
            result.status = ResumeMetadataStatus::Malformed;
            result.metadata = {};
            return result;
        }
        result.status = ResumeMetadataStatus::Present;
        return result;
    }

    UpdateVersionDirectoryListing ObserveVersionDirectories() override {
        UpdateVersionDirectoryListing listing;
        std::vector<std::string> names;
        std::uint32_t systemError = 0;
        if (!ListImmediateNames(trustedRoot_.get(), names, systemError)) {
            listing.systemError = systemError;
            return listing;
        }
        listing.ok = true;
        for (const auto& name : names) {
            if (!IsSafeSegment(name)) continue;
            if (ProbeEntryUnder(trustedRoot_.get(), name) !=
                RetentionEntryKind::Directory) {
                continue;
            }
            listing.directoryNames.push_back(name);
        }
        std::sort(listing.directoryNames.begin(), listing.directoryNames.end());
        listing.directoryNames.erase(
            std::unique(listing.directoryNames.begin(),
                        listing.directoryNames.end()),
            listing.directoryNames.end());
        return listing;
    }

    UpdatePersistenceResult DeleteVersionDirectory(
        const std::string& directoryName) override {
        if (!IsSafeSegment(directoryName) ||
            !UpdateDownloadManager::IsVersionDirectoryName(directoryName)) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        return DeleteVersionDirectoryTree(directoryName);
    }

    UpdatePersistenceResult AppendTransferChunk(
        const TransferAppendIntent& intent) {
        if (!IsSafeRelativePath(intent.partPath) ||
            !EndsWith(intent.partPath, ".part") || intent.bytes.empty()) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        if (intent.identity.partPath != intent.partPath ||
            intent.identity.flightKey.empty() || intent.identity.generation == 0) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        const auto parent = OpenParent(trustedRoot_.get(), intent.partPath, true);
        if (!parent.success) {
            return Failure(parent.invalid ? UpdatePersistenceError::InvalidIntent
                                          : UpdatePersistenceError::DirectoryCreateFailed,
                           static_cast<std::uint32_t>(parent.status));
        }
        auto file = OpenRelative(
            trustedRoot_.get(), intent.partPath,
            GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, kFileOpenIf,
            kFileNonDirectoryFile | kFileWriteThrough);
        if (!file.ok()) {
            return Failure(UpdatePersistenceError::OpenFailed,
                           static_cast<std::uint32_t>(file.status));
        }
        if (!IsSafeRegularFileHandle(file.handle.get())) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_REPARSE_TAG_INVALID);
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.handle.get(), &size) || size.QuadPart < 0) {
            return Failure(UpdatePersistenceError::OpenFailed, GetLastError());
        }
        if (static_cast<std::uint64_t>(size.QuadPart) != intent.offset) {
            return Failure(UpdatePersistenceError::StaleOffset,
                           ERROR_INVALID_DATA);
        }
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(intent.offset);
        if (!SetFilePointerEx(file.handle.get(), offset, nullptr, FILE_BEGIN)) {
            return Failure(UpdatePersistenceError::WriteFailed, GetLastError());
        }
        std::size_t writtenTotal = 0;
        while (writtenTotal < intent.bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                intent.bytes.size() - writtenTotal,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file.handle.get(), intent.bytes.data() + writtenTotal,
                           chunk, &written, nullptr) || written != chunk) {
                return Failure(UpdatePersistenceError::WriteFailed, GetLastError());
            }
            writtenTotal += written;
        }
        if (!FlushFileBuffers(file.handle.get())) {
            return Failure(UpdatePersistenceError::FlushFailed, GetLastError());
        }
        return Failure(UpdatePersistenceError::None, 0, 1);
    }

    UpdatePersistenceResult PersistResumeMetadata(
        const ResumeMetadataIntent& intent) {
        const auto& metadata = intent.metadata;
        if (!IsSafeRelativePath(intent.metaPath) ||
            !EndsWith(intent.metaPath, ".meta.json") || metadata.url.empty() ||
            !metadata.expectedSize || *metadata.expectedSize == 0 ||
            !IsSha256(metadata.expectedSha256) ||
            metadata.downloadedBytes > *metadata.expectedSize) {
            return Failure(UpdatePersistenceError::InvalidIntent);
        }
        nlohmann::ordered_json value;
        value["downloadedBytes"] = metadata.downloadedBytes;
        value["etag"] = metadata.etag;
        value["expectedSha256"] = metadata.expectedSha256;
        value["expectedSize"] = *metadata.expectedSize;
        value["lastModified"] = metadata.lastModified;
        value["url"] = metadata.url;
        StoreIntent write;
        write.kind = StoreIntentKind::WriteTempFile;
        write.path = intent.metaPath + ".tmp";
        write.bytes = value.dump();
        StoreIntent replace;
        replace.kind = StoreIntentKind::AtomicReplace;
        replace.path = write.path;
        replace.targetPath = intent.metaPath;
        return ExecuteStoreIntents({write, replace});
    }

private:
    static bool ListImmediateNames(HANDLE directory,
                                   std::vector<std::string>& names,
                                   std::uint32_t& systemError) {
        const auto& native = NativeApi();
        if (!native.queryDirectoryFile) {
            systemError = ERROR_PROC_NOT_FOUND;
            return false;
        }
        std::vector<WCHAR> buffer(8192U);
        IO_STATUS_BLOCK io{};
        BOOLEAN restartScan = TRUE;
        for (;;) {
            const NTSTATUS status = native.queryDirectoryFile(
                directory, nullptr, nullptr, nullptr, &io, buffer.data(),
                static_cast<ULONG>(buffer.size() * sizeof(WCHAR)),
                kFileNamesInformation, FALSE, nullptr, restartScan);
            restartScan = FALSE;
            if (status == kStatusNoMoreFiles) return true;
            // STATUS_BUFFER_OVERFLOW only means the reply needed two calls.
            if (status < 0 && status != kStatusBufferOverflow) {
                systemError = static_cast<std::uint32_t>(status);
                return false;
            }
            ULONG offset = 0;
            for (;;) {
                const auto* info =
                    reinterpret_cast<const NativeFileNameInformation*>(
                        reinterpret_cast<const BYTE*>(buffer.data()) + offset);
                const std::wstring_view name(
                    info->fileName, info->fileNameLength / sizeof(WCHAR));
                if (name != L"." && name != L"..") {
                    names.push_back(
                        Platform::TextEncoding::WideToUtf8Strict(name));
                    if (names.size() > kMaximumRetentionEntriesPerDirectory) {
                        systemError = ERROR_BUFFER_OVERFLOW;
                        return false;
                    }
                }
                if (info->nextEntryOffset == 0) break;
                offset += info->nextEntryOffset;
            }
        }
    }

    enum class RetentionEntryKind {
        Missing,
        Directory,
        RegularFile,
        Rejected,
    };

    // Classify one child through a non-following relative open against the
    // caller-held parent handle, so a path swap cannot re-point the probe at a
    // different object.  Reparse points never classify as deletable.
    RetentionEntryKind ProbeEntryUnder(HANDLE parent, const std::string& name) const {
        if (!IsSafeSegment(name)) return RetentionEntryKind::Rejected;
        auto directory = OpenRelative(
            parent, name, FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            kFileOpen, kFileDirectoryFile);
        if (directory.ok()) {
            return IsSafeDirectoryHandle(directory.handle.get())
                       ? RetentionEntryKind::Directory
                       : RetentionEntryKind::Rejected;
        }
        if (!directory.missing() && directory.status != kStatusNotADirectory) {
            return RetentionEntryKind::Rejected;
        }
        auto file = OpenRelative(
            parent, name, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            kFileOpen, kFileNonDirectoryFile);
        if (file.ok()) {
            return IsSafeRegularFileHandle(file.handle.get())
                       ? RetentionEntryKind::RegularFile
                       : RetentionEntryKind::Rejected;
        }
        if (file.missing()) return RetentionEntryKind::Missing;
        return RetentionEntryKind::Rejected;
    }

    static bool MarkFileForDeletion(HANDLE file) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        return SetFileInformationByHandle(file, FileDispositionInfo,
                                         &disposition,
                                         sizeof(disposition)) != FALSE;
    }

    static bool MarkDirectoryForDeletion(HANDLE directory) {
        FILE_DISPOSITION_INFO_EX disposition{};
        disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
                            FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
        return SetFileInformationByHandle(directory, FileDispositionInfoEx,
                                         &disposition,
                                         sizeof(disposition)) != FALSE;
    }

    // Remove every child of one already-pinned directory handle.  All opens,
    // probes and deletions go through this held handle, never through a
    // re-opened root-relative path, so a concurrent rename+replace of the
    // walked directory cannot redirect the walk onto a retained package.
    // The walk handle is opened without share-delete, which also blocks other
    // processes from renaming or deleting the directory while it is drained.
    // Reparse points and unexpected entries abort the whole tree; retention
    // is best-effort and retries next update.
    UpdatePersistenceResult DrainPinnedDirectory(
        HANDLE directory, std::size_t depth, std::size_t& removed) const {
        if (depth > kMaximumRetentionDepth) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_INVALID_PARAMETER, removed);
        }
        std::vector<std::string> children;
        std::uint32_t systemError = 0;
        if (!ListImmediateNames(directory, children, systemError)) {
            return Failure(UpdatePersistenceError::DeleteFailed, systemError, removed);
        }
        for (const auto& childName : children) {
            const auto kind = ProbeEntryUnder(directory, childName);
            if (kind == RetentionEntryKind::Missing) continue;
            if (kind == RetentionEntryKind::Rejected) {
                return Failure(UpdatePersistenceError::InvalidIntent,
                               ERROR_REPARSE_TAG_INVALID, removed);
            }
            if (kind == RetentionEntryKind::RegularFile) {
                auto file = OpenRelative(
                    directory, childName, DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    kFileOpen, kFileNonDirectoryFile);
                if (!file.ok()) {
                    if (file.missing()) continue;
                    return Failure(UpdatePersistenceError::DeleteFailed,
                                   static_cast<std::uint32_t>(file.status), removed);
                }
                if (!IsSafeRegularFileHandle(file.handle.get()) ||
                    !MarkFileForDeletion(file.handle.get())) {
                    return Failure(UpdatePersistenceError::DeleteFailed,
                                   GetLastError(), removed);
                }
                ++removed;
                continue;
            }
            auto child = OpenRelative(
                directory, childName,
                DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                kFileOpen, kFileDirectoryFile, FILE_ATTRIBUTE_DIRECTORY);
            if (!child.ok()) {
                if (child.missing()) continue;
                return Failure(UpdatePersistenceError::DeleteFailed,
                               static_cast<std::uint32_t>(child.status), removed);
            }
            if (!IsSafeDirectoryHandle(child.handle.get())) {
                return Failure(UpdatePersistenceError::InvalidIntent,
                               ERROR_REPARSE_TAG_INVALID, removed);
            }
            const auto drained = DrainPinnedDirectory(child.handle.get(), depth + 1U, removed);
            if (!drained.ok()) return drained;
            if (!MarkDirectoryForDeletion(child.handle.get())) {
                return Failure(UpdatePersistenceError::DeleteFailed,
                               GetLastError(), removed);
            }
        }
        return Failure(UpdatePersistenceError::None, 0, removed);
    }

    UpdatePersistenceResult DeleteVersionDirectoryTree(
        const std::string& directoryName) const {
        std::size_t removed = 0;
        // Pinned first: everything below walks through this single handle.
        // Share mode excludes FILE_SHARE_DELETE, so while we walk this
        // directory nobody can rename or replace it.
        auto top = OpenRelative(
            trustedRoot_.get(), directoryName,
            DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            kFileOpen, kFileDirectoryFile, FILE_ATTRIBUTE_DIRECTORY);
        if (!top.ok()) {
            if (top.missing()) {
                return Failure(UpdatePersistenceError::None, 0, 1);
            }
            if (top.status == kStatusNotADirectory) {
                return Failure(UpdatePersistenceError::InvalidIntent,
                               ERROR_REPARSE_TAG_INVALID, removed);
            }
            return Failure(UpdatePersistenceError::OpenFailed,
                           static_cast<std::uint32_t>(top.status), removed);
        }
        if (!IsSafeDirectoryHandle(top.handle.get())) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_REPARSE_TAG_INVALID, removed);
        }
        const auto drained = DrainPinnedDirectory(top.handle.get(), 1U, removed);
        if (!drained.ok()) return drained;
        if (!MarkDirectoryForDeletion(top.handle.get())) {
            return Failure(UpdatePersistenceError::DeleteFailed,
                           GetLastError(), removed);
        }
        return Failure(UpdatePersistenceError::None, 0, 1);
    }

    bool ValidateStoreIntent(const StoreIntent& intent) const {
        if (!IsSafeRelativePath(intent.path)) return false;
        switch (intent.kind) {
        case StoreIntentKind::WriteTempFile:
            return EndsWith(intent.path, ".tmp") && intent.targetPath.empty() &&
                   !intent.bytes.empty();
        case StoreIntentKind::AtomicReplace:
            return intent.bytes.empty() && IsSafeRelativePath(intent.targetPath) &&
                   intent.path == intent.targetPath + ".tmp";
        case StoreIntentKind::DeleteStateFile:
            return intent.path == "patch-info.json" && intent.targetPath.empty() &&
                   intent.bytes.empty();
        case StoreIntentKind::DeleteStagedPatch:
            return intent.targetPath.empty() && intent.bytes.empty();
        case StoreIntentKind::QuarantineStagedPatch:
            return intent.bytes.empty() && IsSafeRelativePath(intent.targetPath) &&
                   intent.targetPath.rfind("quarantine/", 0) == 0 &&
                   intent.path != intent.targetPath;
        }
        return false;
    }

    bool ValidateStorePlan(const std::vector<StoreIntent>& intents) const {
        for (std::size_t index = 0; index < intents.size(); ++index) {
            const auto& intent = intents[index];
            if (!ValidateStoreIntent(intent)) return false;
            if (intent.kind == StoreIntentKind::WriteTempFile) {
                if (index + 1 >= intents.size() ||
                    intents[index + 1].kind != StoreIntentKind::AtomicReplace ||
                    intents[index + 1].path != intent.path) return false;
            } else if (intent.kind == StoreIntentKind::AtomicReplace) {
                if (index == 0 ||
                    intents[index - 1].kind != StoreIntentKind::WriteTempFile ||
                    intents[index - 1].path != intent.path) return false;
            }
        }
        return true;
    }

    bool ValidateDownloadIntent(const DownloadCleanupIntent& intent) const {
        if (!IsSafeRelativePath(intent.sourcePath)) return false;
        switch (intent.kind) {
        case DownloadCleanupIntentKind::DeletePart:
            return intent.targetPath.empty() && EndsWith(intent.sourcePath, ".part");
        case DownloadCleanupIntentKind::DeleteMeta:
            return intent.targetPath.empty() &&
                   EndsWith(intent.sourcePath, ".meta.json");
        case DownloadCleanupIntentKind::RenamePartToTarget:
            return IsSafeRelativePath(intent.targetPath) &&
                   intent.sourcePath == intent.targetPath + ".part";
        }
        return false;
    }

    UpdatePersistenceResult RenameOpenHandle(
        HANDLE file,
        const std::string& targetRelative,
        UpdatePersistenceError failureKind,
        std::size_t completed) const {
        const auto targetParent = OpenParent(
            trustedRoot_.get(), targetRelative, true);
        if (!targetParent.success) {
            return Failure(targetParent.invalid
                               ? UpdatePersistenceError::InvalidIntent
                               : UpdatePersistenceError::DirectoryCreateFailed,
                           static_cast<std::uint32_t>(targetParent.status), completed);
        }
        if (!FlushFileBuffers(file)) {
            return Failure(UpdatePersistenceError::FlushFailed,
                           GetLastError(), completed);
        }
        const std::wstring name = Platform::TextEncoding::Utf8ToWideStrict(
            targetParent.fileName);
        const DWORD nameBytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        const std::size_t infoBytes = sizeof(NativeFileRenameInformation) +
                                      nameBytes + sizeof(wchar_t);
        std::vector<unsigned char> buffer(infoBytes);
        auto* rename = reinterpret_cast<NativeFileRenameInformation*>(buffer.data());
        rename->replaceIfExists = TRUE;
        rename->rootDirectory = targetParent.handle.valid()
                                    ? targetParent.handle.get()
                                    : trustedRoot_.get();
        rename->fileNameLength = nameBytes;
        std::memcpy(rename->fileName, name.data(), nameBytes);
        const auto& native = NativeApi();
        if (!native.setInformationFile) {
            return Failure(failureKind, ERROR_PROC_NOT_FOUND, completed);
        }
        IO_STATUS_BLOCK io{};
        const NTSTATUS status = native.setInformationFile(
            file, &io, rename, static_cast<ULONG>(infoBytes),
            kFileRenameInformation);
        if (status < 0) {
            return Failure(failureKind, static_cast<std::uint32_t>(status), completed);
        }
        return Failure(UpdatePersistenceError::None, 0, completed);
    }

    UpdatePersistenceResult WriteAndRename(const StoreIntent& write,
                                           const StoreIntent& replace,
                                           std::size_t completed) const {
        const auto parent = OpenParent(trustedRoot_.get(), write.path, true);
        if (!parent.success) {
            return Failure(parent.invalid ? UpdatePersistenceError::InvalidIntent
                                          : UpdatePersistenceError::DirectoryCreateFailed,
                           static_cast<std::uint32_t>(parent.status), completed);
        }
        auto file = OpenRelative(
            trustedRoot_.get(), write.path,
            DELETE | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, kFileOverwriteIf,
            kFileNonDirectoryFile | kFileWriteThrough);
        if (!file.ok()) {
            return Failure(UpdatePersistenceError::OpenFailed,
                           static_cast<std::uint32_t>(file.status), completed);
        }
        if (!IsSafeRegularFileHandle(file.handle.get())) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_REPARSE_TAG_INVALID, completed);
        }
        std::size_t offset = 0;
        while (offset < write.bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                write.bytes.size() - offset,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file.handle.get(), write.bytes.data() + offset,
                           chunk, &written, nullptr) || written != chunk) {
                return Failure(UpdatePersistenceError::WriteFailed,
                               GetLastError(), completed);
            }
            offset += written;
        }
        if (!FlushFileBuffers(file.handle.get())) {
            return Failure(UpdatePersistenceError::FlushFailed,
                           GetLastError(), completed);
        }
        return RenameOpenHandle(file.handle.get(), replace.targetPath,
                                UpdatePersistenceError::ReplaceFailed,
                                completed + 1);
    }

    UpdatePersistenceResult Rename(const std::string& sourceRelative,
                                   const std::string& targetRelative,
                                   UpdatePersistenceError failureKind,
                                   std::size_t completed) const {
        auto file = OpenRelative(
            trustedRoot_.get(), sourceRelative,
            DELETE | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, kFileOpen, kFileNonDirectoryFile);
        if (!file.ok()) {
            return Failure(failureKind,
                           static_cast<std::uint32_t>(file.status), completed);
        }
        if (!IsSafeRegularFileHandle(file.handle.get())) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_REPARSE_TAG_INVALID, completed);
        }
        return RenameOpenHandle(file.handle.get(), targetRelative,
                                failureKind, completed);
    }

    UpdatePersistenceResult Delete(const std::string& relativePath,
                                   std::size_t completed) const {
        auto file = OpenRelative(
            trustedRoot_.get(), relativePath,
            DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            kFileOpen, kFileNonDirectoryFile);
        if (!file.ok()) {
            if (file.missing()) {
                return Failure(UpdatePersistenceError::None, 0, completed);
            }
            return Failure(UpdatePersistenceError::DeleteFailed,
                           static_cast<std::uint32_t>(file.status), completed);
        }
        if (!IsSafeRegularFileHandle(file.handle.get())) {
            return Failure(UpdatePersistenceError::InvalidIntent,
                           ERROR_REPARSE_TAG_INVALID, completed);
        }
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(file.handle.get(), FileDispositionInfo,
                                        &disposition, sizeof(disposition))) {
            return Failure(UpdatePersistenceError::DeleteFailed,
                           GetLastError(), completed);
        }
        return Failure(UpdatePersistenceError::None, 0, completed);
    }

    UpdatePersistenceResult ExecuteStoreIntent(const StoreIntent& intent,
                                               std::size_t completed) const {
        switch (intent.kind) {
        case StoreIntentKind::WriteTempFile:
        case StoreIntentKind::AtomicReplace:
            return Failure(UpdatePersistenceError::InvalidIntent, 0, completed);
        case StoreIntentKind::DeleteStateFile:
        case StoreIntentKind::DeleteStagedPatch:
            return Delete(intent.path, completed);
        case StoreIntentKind::QuarantineStagedPatch:
            return Rename(intent.path, intent.targetPath,
                          UpdatePersistenceError::QuarantineFailed, completed);
        }
        return Failure(UpdatePersistenceError::InvalidIntent, 0, completed);
    }

    UpdatePersistenceResult ExecuteDownloadIntent(
        const DownloadCleanupIntent& intent,
        std::size_t completed) const {
        switch (intent.kind) {
        case DownloadCleanupIntentKind::DeletePart:
        case DownloadCleanupIntentKind::DeleteMeta:
            return Delete(intent.sourcePath, completed);
        case DownloadCleanupIntentKind::RenamePartToTarget:
            return Rename(intent.sourcePath, intent.targetPath,
                          UpdatePersistenceError::ReplaceFailed, completed);
        }
        return Failure(UpdatePersistenceError::InvalidIntent, 0, completed);
    }

    ScopedHandle trustedRoot_;
    std::mutex transferMutex_;
    std::optional<aris::update::HttpTransferIdentity> activeTransfer_;
};

CreateResult CreateFromBaseRoot(const std::string& basePathUtf8,
                                const std::vector<std::string>& children) {
    CreateResult result;
    const std::wstring base =
        Platform::TextEncoding::Utf8ToWideStrict(basePathUtf8);
    if (base.empty()) {
        result.status = CreateStatus::EncodingFailed;
        return result;
    }
    // The install-volume updater root may not exist on a fresh machine.  Build
    // the chain first, then reopen every component with the existing reparse
    // checks below; the trusted-root policy is therefore unchanged.
    std::wstring createPath = base;
    std::replace(createPath.begin(), createPath.end(), L'/', L'\\');
    const int createStatus = SHCreateDirectoryExW(nullptr, createPath.c_str(), nullptr);
    if (createStatus != ERROR_SUCCESS && createStatus != ERROR_ALREADY_EXISTS) {
        result.status = CreateStatus::DirectoryCreateFailed;
        result.systemError = static_cast<std::uint32_t>(createStatus);
        return result;
    }
    ScopedHandle current(CreateFileW(
        base.c_str(),
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!current.valid() || !IsSafeDirectoryHandle(current.get())) {
        result.status = CreateStatus::RootUnavailable;
        result.systemError = GetLastError();
        return result;
    }
    for (const auto& child : children) {
        auto opened = OpenRelative(
            current.get(), child,
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE |
                FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            kFileOpenIf, kFileDirectoryFile,
            FILE_ATTRIBUTE_DIRECTORY);
        if (!opened.ok() || !IsSafeDirectoryHandle(opened.handle.get())) {
            result.status = CreateStatus::DirectoryCreateFailed;
            result.systemError = static_cast<std::uint32_t>(opened.status);
            return result;
        }
        current = std::move(opened.handle);
    }
    result.port = std::make_unique<SecureWindowsAdapter>(std::move(current));
    result.status = CreateStatus::Success;
    return result;
}

}  // namespace

CreateResult CreateUpdatePersistencePort() {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    return CreateFromBaseRoot(ARIS_UPDATE_LOCAL_DEMO_ROOT, {});
#else
    PWSTR localAppData = nullptr;
    const HRESULT status = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData);
    if (FAILED(status) || localAppData == nullptr) {
        if (localAppData) CoTaskMemFree(localAppData);
        CreateResult result;
        result.status = CreateStatus::RootUnavailable;
        result.systemError = static_cast<std::uint32_t>(status);
        return result;
    }
    const std::string base =
        Platform::TextEncoding::WideToUtf8Strict(localAppData);
    CoTaskMemFree(localAppData);
    if (base.empty()) {
        CreateResult result;
        result.status = CreateStatus::EncodingFailed;
        return result;
    }
    // The downloaded archive has to end up under the same root the Bootstrap
    // authority later reads from, and that root follows the install volume so
    // install/staging/transaction share one volume serial.  Resolving this
    // independently of the authority is what makes the two disagree: an
    // off-system-drive install would download to %LOCALAPPDATA% and then fail
    // admission with "verified update archive is missing" because the
    // authority looked on the install volume instead.
    std::string installDirectory;
    {
        std::wstring buffer(32768U, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length != 0U && length < buffer.size()) {
            buffer.resize(length);
            const auto slash = buffer.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                buffer.resize(slash);
                installDirectory =
                    Platform::TextEncoding::WideToUtf8Strict(buffer.c_str());
            }
        }
    }
    const std::string root =
        Platform::UpdateBootstrapAuthority::ResolveUpdaterOwnedBaseForInstall(
            installDirectory, base);
    if (root.empty()) return CreateFromBaseRoot(base, {"HarkenSearch", "updates"});
    return CreateFromBaseRoot(root, {});
#endif
}

namespace {

aris::update::UpdatePersistenceResult WriteBootstrapFailureDiagnosticAt(
    const std::string& installDirectory, const std::string& localAppData,
    const char* stageName, int exitCode, const char* extra) {
    using namespace aris::update;
    if (exitCode == 0) return {};
    const std::string bytes =
        "stage=" + std::string(stageName != nullptr ? stageName : "") +
        "\ncode=" + std::to_string(exitCode) +
        "\nextra=" + std::string(extra != nullptr ? extra : "") + "\n";
    const std::string root =
        Platform::UpdateBootstrapAuthority::ResolveUpdaterOwnedBaseForInstall(
            installDirectory, localAppData);
    if (root.empty()) {
        return {UpdatePersistenceError::OpenFailed, 0, 0};
    }
    auto created = CreateFromBaseRoot(root, {});
    if (!created.ok()) {
        return {created.status == CreateStatus::DirectoryCreateFailed
                    ? UpdatePersistenceError::DirectoryCreateFailed
                    : UpdatePersistenceError::OpenFailed,
                created.systemError, 0};
    }
    StoreIntent write;
    write.kind = StoreIntentKind::WriteTempFile;
    write.path = "bootstrap-last-failure.txt.tmp";
    write.bytes = bytes;
    StoreIntent replace;
    replace.kind = StoreIntentKind::AtomicReplace;
    replace.path = write.path;
    replace.targetPath = "bootstrap-last-failure.txt";
    return created.port->ExecuteStoreIntents({write, replace});
}

}  // namespace

void RecordBootstrapFailureDiagnostic(const char* stageName, int exitCode,
                                      const char* extra) {
    if (exitCode == 0) return;
    std::wstring module(32768U, L'\0');
    const DWORD moduleLength = GetModuleFileNameW(
        nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (moduleLength == 0U || moduleLength >= module.size()) return;
    module.resize(moduleLength);
    const auto slash = module.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    module.resize(slash);
    const std::string installDirectory =
        Platform::TextEncoding::WideToUtf8Strict(module.c_str());
    if (installDirectory.empty()) return;

    PWSTR localAppDataWide = nullptr;
    const HRESULT status = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppDataWide);
    if (FAILED(status) || localAppDataWide == nullptr) {
        if (localAppDataWide) CoTaskMemFree(localAppDataWide);
        return;
    }
    const std::string localAppData =
        Platform::TextEncoding::WideToUtf8Strict(localAppDataWide);
    CoTaskMemFree(localAppDataWide);
    if (localAppData.empty()) return;
    static_cast<void>(WriteBootstrapFailureDiagnosticAt(
        installDirectory, localAppData, stageName, exitCode, extra));
}

std::string DescribeCreateFailure(CreateStatus status, std::uint32_t systemError) {
    switch (status) {
        case CreateStatus::DirectoryCreateFailed:
            return "update persistence directory creation failed (error " +
                   std::to_string(systemError) + ")";
        case CreateStatus::RootUnavailable:
            return "update persistence root unavailable (error " +
                   std::to_string(systemError) + ")";
        case CreateStatus::EncodingFailed:
            return "update persistence path encoding failed";
        case CreateStatus::Unsupported:
            return "update persistence is unsupported on this platform";
        case CreateStatus::Success:
            return {};
    }
    return "update persistence initialization failed (error " +
           std::to_string(systemError) + ")";
}

#ifdef ARIS_UPDATE_PERSISTENCE_TESTING
namespace Testing {

CreateResult CreateForTrustedRoot(const std::string& rootPathUtf8) {
    return CreateFromBaseRoot(rootPathUtf8, {});
}

aris::update::UpdatePersistenceResult WriteBootstrapFailureDiagnosticForInstall(
    const std::string& installDirectory, const std::string& localAppData,
    const char* stageName, int exitCode, const char* extra) {
    return WriteBootstrapFailureDiagnosticAt(
        installDirectory, localAppData, stageName, exitCode, extra);
}

}  // namespace Testing
#endif

}  // namespace Platform::UpdatePersistence

#else

namespace Platform::UpdatePersistence {

CreateResult CreateUpdatePersistencePort() {
    CreateResult result;
    result.status = CreateStatus::Unsupported;
    return result;
}

void RecordBootstrapFailureDiagnostic(const char*, int, const char*) {
}

#ifdef ARIS_UPDATE_PERSISTENCE_TESTING
namespace Testing {

CreateResult CreateForTrustedRoot(const std::string&) {
    CreateResult result;
    result.status = CreateStatus::Unsupported;
    return result;
}

aris::update::UpdatePersistenceResult WriteBootstrapFailureDiagnosticForInstall(
    const std::string&, const std::string&, const char*, int, const char*) {
    return {aris::update::UpdatePersistenceError::OpenFailed, 0, 0};
}

}  // namespace Testing
#endif

std::string DescribeCreateFailure(CreateStatus status, std::uint32_t systemError) {
    switch (status) {
        case CreateStatus::DirectoryCreateFailed:
            return "update persistence directory creation failed (error " +
                   std::to_string(systemError) + ")";
        case CreateStatus::RootUnavailable:
            return "update persistence root unavailable (error " +
                   std::to_string(systemError) + ")";
        case CreateStatus::EncodingFailed:
            return "update persistence path encoding failed";
        case CreateStatus::Unsupported:
            return "update persistence is unsupported on this platform";
        case CreateStatus::Success:
            return {};
    }
    return "update persistence initialization failed (error " +
           std::to_string(systemError) + ")";
}

}

#endif
