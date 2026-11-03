#include "bootstrap/WindowsBootstrapFilesystem.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <bcrypt.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <cstring>
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
#include <cstdio>
#endif
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aris::bootstrap {
namespace {

constexpr ULONG kObjectDontReparse = 0x00001000UL;
constexpr ULONG kFileDirectoryFile = 0x00000001UL;
constexpr ULONG kFileWriteThrough = 0x00000002UL;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
constexpr ULONG kFileOpen = 1UL;
constexpr ULONG kFileCreate = 2UL;
constexpr ULONG kFileRenameInformationEx = 65UL;
constexpr ULONG_PTR kFileOpened = 1;

constexpr NTSTATUS kStatusNoSuchFile =
    static_cast<NTSTATUS>(0xC000000FUL);
constexpr NTSTATUS kStatusAccessDenied =
    static_cast<NTSTATUS>(0xC0000022UL);
constexpr NTSTATUS kStatusObjectNameNotFound =
    static_cast<NTSTATUS>(0xC0000034UL);
constexpr NTSTATUS kStatusObjectNameCollision =
    static_cast<NTSTATUS>(0xC0000035UL);
constexpr NTSTATUS kStatusObjectPathNotFound =
    static_cast<NTSTATUS>(0xC000003AUL);
constexpr NTSTATUS kStatusNotSameDevice =
    static_cast<NTSTATUS>(0xC00000D4UL);
constexpr NTSTATUS kStatusFileIsADirectory =
    static_cast<NTSTATUS>(0xC00000BAUL);
constexpr NTSTATUS kStatusDirectoryNotEmpty =
    static_cast<NTSTATUS>(0xC0000101UL);
constexpr NTSTATUS kStatusNotADirectory =
    static_cast<NTSTATUS>(0xC0000103UL);
constexpr NTSTATUS kStatusReparsePointEncountered =
    static_cast<NTSTATUS>(0xC000050BUL);
constexpr NTSTATUS kStatusNoMoreFiles =
    static_cast<NTSTATUS>(0x80000006UL);
constexpr FILE_INFORMATION_CLASS kFileIdBothDirectoryInformation =
    static_cast<FILE_INFORMATION_CLASS>(37);
constexpr std::size_t kMaximumDirectoryEnumerationPages = 128U;
constexpr std::size_t kMaximumDirectoryEnumerationEntries = 8192U;
constexpr std::size_t kMaximumDirectoryEnumerationNameBytes = 2U * 1024U * 1024U;
constexpr std::size_t kDirectoryInformationAlignment = 8U;

constexpr wchar_t kJournalSlotAName[] = L"aris-bootstrap.journal.a";
constexpr wchar_t kJournalSlotBName[] = L"aris-bootstrap.journal.b";
constexpr wchar_t kJournalSlotATempName[] = L"aris-bootstrap.journal.a.tmp";
constexpr wchar_t kJournalSlotBTempName[] = L"aris-bootstrap.journal.b.tmp";
constexpr wchar_t kLegacyJournalFileName[] = L"aris-bootstrap.journal";
constexpr wchar_t kLegacyJournalTempName[] = L"aris-bootstrap.journal.tmp";
constexpr wchar_t kRuntimePairDirectoryName[] = L"runtime-pair";
constexpr wchar_t kRuntimePairBootstrapName[] = L"HarkenSearchBootstrap.exe";
constexpr DWORD kMaxRollbackMetadataBytes = 256U * 1024U;

void WriteLocalDemoTrustedRootsDiagnostic(
    const char* stage,
    const WindowsFilesystemBackendResult& result = {}) {
#if defined(ARIS_UPDATE_LOCAL_DEMO) && ARIS_UPDATE_LOCAL_DEMO
    std::FILE* diagnostic = nullptr;
    const std::string path =
        std::string(ARIS_UPDATE_LOCAL_DEMO_ROOT) +
        "/bootstrap-demo-roots.txt";
    if (fopen_s(&diagnostic, path.c_str(), "wb") == 0 &&
        diagnostic != nullptr) {
        std::fprintf(diagnostic, "stage=%s status=%d native=%u\n", stage,
                     static_cast<int>(result.status), result.nativeError);
        std::fclose(diagnostic);
    }
#elif defined(ARIS_BOOTSTRAP_TEST_DIAGNOSTICS)
    std::fprintf(stderr, "trusted-roots:stage=%s status=%d native=%u\n",
                 stage, static_cast<int>(result.status), result.nativeError);
#else
    static_cast<void>(stage);
    static_cast<void>(result);
#endif
}

using NtCreateFileFunction = NTSTATUS(NTAPI*)(
    PHANDLE,
    ACCESS_MASK,
    POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK,
    PLARGE_INTEGER,
    ULONG,
    ULONG,
    ULONG,
    ULONG,
    PVOID,
    ULONG);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NTSTATUS);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(
    HANDLE,
    PIO_STATUS_BLOCK,
    PVOID,
    ULONG,
    ULONG);
using NtQueryInformationFileFunction = NTSTATUS(NTAPI*)(
    HANDLE,
    PIO_STATUS_BLOCK,
    PVOID,
    ULONG,
    FILE_INFORMATION_CLASS);
using NtQueryDirectoryFileFunction = NTSTATUS(NTAPI*)(
    HANDLE,
    HANDLE,
    PIO_APC_ROUTINE,
    PVOID,
    PIO_STATUS_BLOCK,
    PVOID,
    ULONG,
    FILE_INFORMATION_CLASS,
    BOOLEAN,
    PUNICODE_STRING,
    BOOLEAN);

struct NativeFunctions {
    NtCreateFileFunction createFile = nullptr;
    NtSetInformationFileFunction setInformationFile = nullptr;
    NtQueryInformationFileFunction queryInformationFile = nullptr;
    NtQueryDirectoryFileFunction queryDirectoryFile = nullptr;
    RtlNtStatusToDosErrorFunction statusToDosError = nullptr;
};

using BCryptOpenAlgorithmProviderFunction = decltype(&BCryptOpenAlgorithmProvider);
using BCryptGetPropertyFunction = decltype(&BCryptGetProperty);
using BCryptCreateHashFunction = decltype(&BCryptCreateHash);
using BCryptHashDataFunction = decltype(&BCryptHashData);
using BCryptFinishHashFunction = decltype(&BCryptFinishHash);
using BCryptHashFunction = decltype(&BCryptHash);
using BCryptDestroyHashFunction = decltype(&BCryptDestroyHash);
using BCryptCloseAlgorithmProviderFunction =
    decltype(&BCryptCloseAlgorithmProvider);

struct CryptoFunctions {
    HMODULE module = nullptr;
    BCryptOpenAlgorithmProviderFunction openAlgorithmProvider = nullptr;
    BCryptGetPropertyFunction getProperty = nullptr;
    BCryptCreateHashFunction createHash = nullptr;
    BCryptHashDataFunction hashData = nullptr;
    BCryptFinishHashFunction finishHash = nullptr;
    BCryptHashFunction hash = nullptr;
    BCryptDestroyHashFunction destroyHash = nullptr;
    BCryptCloseAlgorithmProviderFunction closeAlgorithmProvider = nullptr;

    bool available() const noexcept {
        return module != nullptr && openAlgorithmProvider != nullptr &&
               getProperty != nullptr && createHash != nullptr &&
               hashData != nullptr && finishHash != nullptr && hash != nullptr &&
               destroyHash != nullptr && closeAlgorithmProvider != nullptr;
    }
};

const CryptoFunctions& CryptoApi() {
    static const CryptoFunctions functions = [] {
        CryptoFunctions result;
        result.module = LoadLibraryExW(L"bcrypt.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (result.module == nullptr) return result;
        result.openAlgorithmProvider =
            reinterpret_cast<BCryptOpenAlgorithmProviderFunction>(
                GetProcAddress(result.module, "BCryptOpenAlgorithmProvider"));
        result.getProperty = reinterpret_cast<BCryptGetPropertyFunction>(
            GetProcAddress(result.module, "BCryptGetProperty"));
        result.createHash = reinterpret_cast<BCryptCreateHashFunction>(
            GetProcAddress(result.module, "BCryptCreateHash"));
        result.hashData = reinterpret_cast<BCryptHashDataFunction>(
            GetProcAddress(result.module, "BCryptHashData"));
        result.finishHash = reinterpret_cast<BCryptFinishHashFunction>(
            GetProcAddress(result.module, "BCryptFinishHash"));
        result.hash = reinterpret_cast<BCryptHashFunction>(
            GetProcAddress(result.module, "BCryptHash"));
        result.destroyHash = reinterpret_cast<BCryptDestroyHashFunction>(
            GetProcAddress(result.module, "BCryptDestroyHash"));
        result.closeAlgorithmProvider =
            reinterpret_cast<BCryptCloseAlgorithmProviderFunction>(
                GetProcAddress(result.module, "BCryptCloseAlgorithmProvider"));
        return result;
    }();
    return functions;
}

class ScopedAlgorithm {
public:
    ScopedAlgorithm(const CryptoFunctions& api,
                    BCRYPT_ALG_HANDLE handle) noexcept
        : api_(api), handle_(handle) {}

    ~ScopedAlgorithm() {
        if (handle_ != nullptr && api_.closeAlgorithmProvider != nullptr) {
            static_cast<void>(api_.closeAlgorithmProvider(handle_, 0));
        }
    }

    BCRYPT_ALG_HANDLE get() const noexcept {
        return handle_;
    }

private:
    const CryptoFunctions& api_;
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class ScopedHash {
public:
    ScopedHash(const CryptoFunctions& api, BCRYPT_HASH_HANDLE handle) noexcept
        : api_(api), handle_(handle) {}

    ~ScopedHash() {
        if (handle_ != nullptr && api_.destroyHash != nullptr) {
            static_cast<void>(api_.destroyHash(handle_));
        }
    }

    BCRYPT_HASH_HANDLE get() const noexcept {
        return handle_;
    }

private:
    const CryptoFunctions& api_;
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

const NativeFunctions& NativeApi() {
    static const NativeFunctions functions = [] {
        NativeFunctions result;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) return result;
        result.createFile = reinterpret_cast<NtCreateFileFunction>(
            GetProcAddress(ntdll, "NtCreateFile"));
        result.setInformationFile = reinterpret_cast<NtSetInformationFileFunction>(
            GetProcAddress(ntdll, "NtSetInformationFile"));
        result.queryInformationFile =
            reinterpret_cast<NtQueryInformationFileFunction>(
                GetProcAddress(ntdll, "NtQueryInformationFile"));
        result.queryDirectoryFile =
            reinterpret_cast<NtQueryDirectoryFileFunction>(
                GetProcAddress(ntdll, "NtQueryDirectoryFile"));
        result.statusToDosError = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
            GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        return result;
    }();
    return functions;
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedHandle() {
        reset();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const noexcept {
        return handle_;
    }

    HANDLE release() noexcept {
        const HANDLE result = handle_;
        handle_ = nullptr;
        return result;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class ScopedLocalFree {
public:
    ScopedLocalFree() = default;
    explicit ScopedLocalFree(PVOID value) noexcept : value_(value) {}
    ~ScopedLocalFree() {
        reset();
    }
    ScopedLocalFree(const ScopedLocalFree&) = delete;
    ScopedLocalFree& operator=(const ScopedLocalFree&) = delete;
    ScopedLocalFree(ScopedLocalFree&& other) noexcept
        : value_(other.release()) {}
    ScopedLocalFree& operator=(ScopedLocalFree&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    PVOID get() const noexcept { return value_; }
    PVOID release() noexcept {
        const PVOID value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(PVOID value = nullptr) noexcept {
        if (value_ != nullptr) static_cast<void>(LocalFree(value_));
        value_ = value;
    }
private:
    PVOID value_ = nullptr;
};

WindowsFilesystemBackendResult BackendFailure(
    WindowsFilesystemBackendStatus status,
    std::uint32_t nativeError) noexcept {
    return {status, nativeError};
}

WindowsFilesystemBackendResult Win32Failure(DWORD error) noexcept {
    WindowsFilesystemBackendStatus status = WindowsFilesystemBackendStatus::IoFailure;
    switch (error) {
        case ERROR_SUCCESS:
            status = WindowsFilesystemBackendStatus::Ok;
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            status = WindowsFilesystemBackendStatus::NotFound;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            status = WindowsFilesystemBackendStatus::AlreadyExists;
            break;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_DELETE_PENDING:
            status = WindowsFilesystemBackendStatus::AccessDenied;
            break;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_DRIVE:
        case ERROR_FILENAME_EXCED_RANGE:
            status = WindowsFilesystemBackendStatus::InvalidPath;
            break;
        case ERROR_CANT_ACCESS_FILE:
        case ERROR_CANT_RESOLVE_FILENAME:
        case ERROR_INVALID_REPARSE_DATA:
        case ERROR_REPARSE_TAG_INVALID:
        case ERROR_REPARSE_TAG_MISMATCH:
            status = WindowsFilesystemBackendStatus::ReparsePoint;
            break;
        case ERROR_DIRECTORY:
            status = WindowsFilesystemBackendStatus::NotDirectory;
            break;
        case ERROR_DIR_NOT_EMPTY:
            status = WindowsFilesystemBackendStatus::DirectoryNotEmpty;
            break;
        case ERROR_NOT_SAME_DEVICE:
            status = WindowsFilesystemBackendStatus::CrossVolume;
            break;
        case ERROR_INVALID_OWNER:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_INVALID_SECURITY_DESCR:
        case ERROR_NOT_SUPPORTED:
            status = WindowsFilesystemBackendStatus::UnsupportedMetadata;
            break;
        default:
            break;
    }
    return BackendFailure(status, static_cast<std::uint32_t>(error));
}

bool IsMissingStatus(NTSTATUS status) noexcept {
    return status == kStatusNoSuchFile ||
           status == kStatusObjectNameNotFound ||
           status == kStatusObjectPathNotFound;
}

WindowsFilesystemBackendResult NtFailure(NTSTATUS status) noexcept {
    if (IsMissingStatus(status)) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotFound,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusObjectNameCollision) {
        return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusAccessDenied) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusReparsePointEncountered) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusNotADirectory) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusFileIsADirectory) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotRegularFile,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusDirectoryNotEmpty) {
        return BackendFailure(WindowsFilesystemBackendStatus::DirectoryNotEmpty,
                              static_cast<std::uint32_t>(status));
    }
    if (status == kStatusNotSameDevice) {
        return BackendFailure(WindowsFilesystemBackendStatus::CrossVolume,
                              static_cast<std::uint32_t>(status));
    }

    const NativeFunctions& native = NativeApi();
    if (native.statusToDosError != nullptr) {
        const ULONG error = native.statusToDosError(status);
        if (error != ERROR_MR_MID_NOT_FOUND) {
            return Win32Failure(static_cast<DWORD>(error));
        }
    }
    return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                          static_cast<std::uint32_t>(status));
}

bool Utf8ToWideStrict(const std::string& value, std::wstring& output) {
    output.clear();
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        value.find('\0') != std::string::npos) {
        return false;
    }
    const int inputSize = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputSize, nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               inputSize, output.data(), required) == required;
}

bool IsAsciiPathValue(const std::string& value) {
    for (const unsigned char character : value) {
        if (character >= 0x80U) return false;
    }
    return true;
}

wchar_t AsciiUpper(wchar_t value) noexcept {
    if (value >= L'a' && value <= L'z') return value - (L'a' - L'A');
    return value;
}

bool IsReservedWindowsName(const std::wstring& component) {
    const std::size_t extension = component.find(L'.');
    std::wstring base = component.substr(0, extension);
    std::transform(base.begin(), base.end(), base.begin(), AsciiUpper);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    return base.size() == 4 &&
           (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
           base[3] >= L'1' && base[3] <= L'9';
}

bool IsSafeWindowsComponentBase(const std::wstring& component) {
    if (component.empty() || component.size() > 255 ||
        component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ') {
        return false;
    }
    for (const wchar_t character : component) {
        if (character < 0x20 || character == L'<' || character == L'>' ||
            character == L':' || character == L'"' || character == L'|' ||
            character == L'?' || character == L'*') {
            return false;
        }
    }
    return !IsReservedWindowsName(component);
}

bool IsSafeAbsoluteRootComponent(const std::wstring& component) {
    return IsSafeWindowsComponentBase(component);
}

bool IsSafeRelativeComponent(const std::wstring& component) {
    return component.find(L'~') == std::wstring::npos &&
           IsSafeWindowsComponentBase(component);
}

bool RelativePathToWide(const std::string& value, std::wstring& output) {
    if (value.empty() || value.size() > 4096 || !IsAsciiPathValue(value) ||
        !Utf8ToWideStrict(value, output)) {
        return false;
    }
    std::replace(output.begin(), output.end(), L'/', L'\\');
    if (output.empty() || output.front() == L'\\' || output.back() == L'\\' ||
        output.find(L':') != std::wstring::npos ||
        output.find(L"\\\\") != std::wstring::npos) {
        return false;
    }

    std::size_t begin = 0;
    while (begin < output.size()) {
        const std::size_t end = output.find(L'\\', begin);
        const std::wstring component = output.substr(
            begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (component.empty() || component.size() > 255U ||
            !IsSafeRelativeComponent(component)) return false;
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return output.size() <=
           static_cast<std::size_t>((std::numeric_limits<USHORT>::max)()) /
               sizeof(wchar_t);
}

bool EqualPathExact(const std::wstring& left, const std::wstring& right) {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), FALSE) ==
           CSTR_EQUAL;
}

bool EqualPathInsensitive(const std::wstring& left, const std::wstring& right) {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) ==
           CSTR_EQUAL;
}

bool IsSameOrDescendantPath(const std::wstring& candidate,
                            const std::wstring& ancestor) {
    if (EqualPathInsensitive(candidate, ancestor)) return true;
    if (candidate.size() <= ancestor.size() ||
        candidate[ancestor.size()] != L'\\') {
        return false;
    }
    return EqualPathInsensitive(candidate.substr(0, ancestor.size()), ancestor);
}

bool PathsOverlap(const std::wstring& left, const std::wstring& right) {
    return IsSameOrDescendantPath(left, right) ||
           IsSameOrDescendantPath(right, left);
}

void TrimTrailingSeparators(std::wstring& value) {
    while (value.size() > 3 && !value.empty() && value.back() == L'\\') {
        value.pop_back();
    }
}

std::wstring ComparableFinalPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    constexpr wchar_t uncPrefix[] = L"\\\\?\\UNC\\";
    constexpr wchar_t extendedPrefix[] = L"\\\\?\\";
    if (value.size() >= std::size(uncPrefix) - 1 &&
        EqualPathInsensitive(value.substr(0, std::size(uncPrefix) - 1),
                             std::wstring(uncPrefix))) {
        value = L"\\\\" + value.substr(std::size(uncPrefix) - 1);
    } else if (value.size() >= std::size(extendedPrefix) - 1 &&
               EqualPathInsensitive(
                   value.substr(0, std::size(extendedPrefix) - 1),
                   std::wstring(extendedPrefix))) {
        value.erase(0, std::size(extendedPrefix) - 1);
    }
    TrimTrailingSeparators(value);
    return value;
}

bool SameAuthenticatedParentProcess(
    const BootstrapProcessIdentity& left,
    const BootstrapProcessIdentity& right) {
    std::wstring leftPath;
    std::wstring rightPath;
    return left.valid && right.valid &&
           left.processId == right.processId &&
           left.creationTime == right.creationTime &&
           left.imageVolumeSerial == right.imageVolumeSerial &&
           left.imageFileId == right.imageFileId &&
           Utf8ToWideStrict(left.imagePath, leftPath) &&
           Utf8ToWideStrict(right.imagePath, rightPath) &&
           EqualPathInsensitive(
               ComparableFinalPath(std::move(leftPath)),
               ComparableFinalPath(std::move(rightPath)));
}

WindowsFilesystemBackendResult NormalizeAbsoluteRoot(
    const std::string& path,
    std::wstring& output) {
    std::wstring input;
    if (!Utf8ToWideStrict(path, input)) {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_INVALID_NAME);
    }
    std::replace(input.begin(), input.end(), L'/', L'\\');
    if (input.size() <= 3 || input.size() > 4096U ||
        !((input[0] >= L'A' && input[0] <= L'Z') ||
          (input[0] >= L'a' && input[0] <= L'z')) ||
        input[1] != L':' || input[2] != L'\\' || input.back() == L'\\' ||
        input.find(L':', 2) != std::wstring::npos ||
        input.find(L"\\\\", 2) != std::wstring::npos) {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_INVALID_NAME);
    }

    std::size_t begin = 3;
    while (begin < input.size()) {
        const std::size_t end = input.find(L'\\', begin);
        const std::wstring component = input.substr(
            begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (component.empty() || component.size() > 255U ||
            !IsSafeAbsoluteRootComponent(component)) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }

    const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (required == 0) return Win32Failure(GetLastError());
    std::wstring full(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), required, full.data(), nullptr);
    if (written == 0 || written >= required) return Win32Failure(GetLastError());
    full.resize(static_cast<std::size_t>(written));
    TrimTrailingSeparators(full);
    if (!EqualPathExact(input, full)) {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_INVALID_NAME);
    }
    output = std::move(full);
    return {};
}

WindowsFilesystemBackendResult FinalPathForHandle(
    HANDLE handle,
    std::wstring& output) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) return Win32Failure(GetLastError());
    std::wstring value(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, value.data(), static_cast<DWORD>(value.size()), flags);
    if (written == 0 || written >= value.size()) return Win32Failure(GetLastError());
    value.resize(static_cast<std::size_t>(written));
    output = ComparableFinalPath(std::move(value));
    return {};
}

WindowsFilesystemBackendResult IdentityForHandle(
    HANDLE handle,
    WindowsFileIdentity& output) {
    output = {};
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        return Win32Failure(GetLastError());
    }
    output.valid = true;
    output.volumeSerial = static_cast<std::uint64_t>(info.VolumeSerialNumber);
    static_assert(sizeof(output.fileId) == sizeof(info.FileId.Identifier));
    std::memcpy(output.fileId.data(), info.FileId.Identifier, output.fileId.size());
    return {};
}

WindowsFilesystemBackendResult CurrentProcessImageMatches(
    HANDLE candidate,
    bool& output) {
    output = false;
    if (candidate == nullptr || candidate == INVALID_HANDLE_VALUE) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_INVALID_HANDLE);
    }
    std::wstring candidatePath;
    const WindowsFilesystemBackendResult candidatePathResult =
        FinalPathForHandle(candidate, candidatePath);
    if (!candidatePathResult.ok()) return candidatePathResult;

    wchar_t imageBuffer[32768]{};
    const DWORD imageLength = GetModuleFileNameW(
        nullptr, imageBuffer, static_cast<DWORD>(std::size(imageBuffer)));
    if (imageLength == 0U || imageLength >= std::size(imageBuffer)) {
        return Win32Failure(imageLength == 0U ? GetLastError()
                                              : ERROR_INSUFFICIENT_BUFFER);
    }
    const std::wstring imagePath = ComparableFinalPath(
        std::wstring(imageBuffer, imageLength));
    if (!EqualPathExact(candidatePath, imagePath)) return {};

    ScopedHandle imageHandle(CreateFileW(
        imagePath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!imageHandle.valid()) return Win32Failure(GetLastError());
    WindowsFileIdentity candidateIdentity;
    const WindowsFilesystemBackendResult candidateIdentityResult =
        IdentityForHandle(candidate, candidateIdentity);
    if (!candidateIdentityResult.ok()) return candidateIdentityResult;
    WindowsFileIdentity imageIdentity;
    const WindowsFilesystemBackendResult imageIdentityResult =
        IdentityForHandle(imageHandle.get(), imageIdentity);
    if (!imageIdentityResult.ok()) return imageIdentityResult;
    output = candidateIdentity == imageIdentity;
    return {};
}

WindowsFilesystemBackendResult ObservationForHandle(
    HANDLE handle,
    WindowsFilesystemObservation& output) {
    output = {};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
        return Win32Failure(GetLastError());
    }

    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        output.kind = WindowsFilesystemEntryKind::ReparsePoint;
    } else if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        output.kind = WindowsFilesystemEntryKind::Directory;
    } else {
        SetLastError(ERROR_SUCCESS);
        const DWORD type = GetFileType(handle);
        if (type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS) {
            return Win32Failure(GetLastError());
        }
        output.kind = type == FILE_TYPE_DISK
            ? WindowsFilesystemEntryKind::RegularFile
            : WindowsFilesystemEntryKind::Other;
    }
    return IdentityForHandle(handle, output.identity);
}

struct NativeDirectoryEntrySnapshot {
    std::wstring name;
    DWORD attributes = 0U;
};

// Enumerate from a pinned directory handle.  A path-based FindFirstFileW on a
// final-path string can be redirected to a same-name replacement between the
// observation and the delete; FileIdBothDirectoryInfo keeps the namespace
// bound to the handle we already authenticated.
WindowsFilesystemBackendResult ReadDirectoryEntriesNoFollow(
    HANDLE directory,
    std::vector<NativeDirectoryEntrySnapshot>& output) {
    output.clear();
    if (directory == nullptr || directory == INVALID_HANDLE_VALUE) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_INVALID_HANDLE);
    }
    const NativeFunctions& native = NativeApi();
    if (native.queryDirectoryFile == nullptr) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_PROC_NOT_FOUND);
    }

    alignas(FILE_ID_BOTH_DIR_INFO)
        std::array<std::uint8_t, 64U * 1024U> buffer{};
    bool restartScan = true;
    std::size_t pagesRead = 0U;
    std::size_t entriesRead = 0U;
    std::size_t nameBytesRead = 0U;
    for (;;) {
        if (++pagesRead > kMaximumDirectoryEnumerationPages) {
            return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                  ERROR_INVALID_DATA);
        }
        IO_STATUS_BLOCK io{};
        const NTSTATUS status = native.queryDirectoryFile(
            directory, nullptr, nullptr, nullptr, &io, buffer.data(),
            static_cast<ULONG>(buffer.size()),
            kFileIdBothDirectoryInformation, FALSE, nullptr,
            restartScan ? TRUE : FALSE);
        restartScan = false;
        if (status == kStatusNoMoreFiles) return {};
        if (status != 0) {
            // A bounded buffer that cannot produce a complete record is not a
            // safe basis for destructive cleanup.  Fail closed rather than
            // guessing that the namespace is empty.
            return status < 0
                ? NtFailure(status)
                : BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                 static_cast<std::uint32_t>(status));
        }
        const std::size_t bytesReturned =
            static_cast<std::size_t>(io.Information);
        if (bytesReturned == 0U || bytesReturned > buffer.size()) {
            return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                  ERROR_INVALID_DATA);
        }
        std::size_t offset = 0U;
        for (;;) {
            constexpr std::size_t headerBytes =
                offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            static_assert(headerBytes % kDirectoryInformationAlignment == 0U);
            if (offset > bytesReturned ||
                bytesReturned - offset < headerBytes ||
                (offset % kDirectoryInformationAlignment) != 0U) {
                return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                      ERROR_INVALID_DATA);
            }
            const auto* entry = reinterpret_cast<
                const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
            if ((entry->FileNameLength % sizeof(wchar_t)) != 0U ||
                entry->FileNameLength >
                    bytesReturned - offset - headerBytes) {
                return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                      ERROR_INVALID_DATA);
            }
            const std::size_t recordBytes =
                headerBytes + static_cast<std::size_t>(entry->FileNameLength);
            const std::size_t entryNameBytes =
                static_cast<std::size_t>(entry->FileNameLength);
            if (++entriesRead > kMaximumDirectoryEnumerationEntries ||
                entryNameBytes > kMaximumDirectoryEnumerationNameBytes ||
                nameBytesRead >
                    kMaximumDirectoryEnumerationNameBytes - entryNameBytes) {
                return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                      ERROR_INVALID_DATA);
            }
            nameBytesRead += entryNameBytes;
            output.push_back({
                std::wstring(entry->FileName,
                             entry->FileNameLength / sizeof(wchar_t)),
                entry->FileAttributes});
            if (entry->NextEntryOffset == 0U) break;
            if (entry->NextEntryOffset < recordBytes ||
                (entry->NextEntryOffset % kDirectoryInformationAlignment) != 0U ||
                entry->NextEntryOffset > bytesReturned - offset) {
                return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                      ERROR_INVALID_DATA);
            }
            offset += entry->NextEntryOffset;
        }

        // Ask once more with the same handle and a non-restarting scan;
        // STATUS_NO_MORE_FILES ends the enumeration.
    }
}

struct NativeOpenResult {
    WindowsFilesystemBackendResult result;
    ScopedHandle handle;
    ULONG_PTR information = 0;
};

WindowsFilesystemBackendResult OpenObservedRelative(
    HANDLE root,
    const std::wstring& relativePath,
    ACCESS_MASK access,
    ULONG share,
    ULONG options,
    NativeOpenResult& opened,
    WindowsFilesystemObservation& observation);

NativeOpenResult OpenRelativeWide(
    HANDLE root,
    const std::wstring& relativePath,
    ACCESS_MASK access,
    ULONG share,
    ULONG disposition,
    ULONG options,
    ULONG attributes = FILE_ATTRIBUTE_NORMAL) {
    NativeOpenResult output;
    const NativeFunctions& native = NativeApi();
    if (native.createFile == nullptr || root == nullptr ||
        root == INVALID_HANDLE_VALUE || relativePath.empty() ||
        relativePath.size() >
            static_cast<std::size_t>((std::numeric_limits<USHORT>::max)()) /
                sizeof(wchar_t)) {
        output.result = BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                       native.createFile == nullptr
                                           ? ERROR_PROC_NOT_FOUND
                                           : ERROR_INVALID_NAME);
        return output;
    }

    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(relativePath.data());
    name.Length = static_cast<USHORT>(relativePath.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;

    OBJECT_ATTRIBUTES object{};
    object.Length = sizeof(object);
    object.RootDirectory = root;
    object.ObjectName = &name;
    object.Attributes = kObjectDontReparse;

    IO_STATUS_BLOCK io{};
    HANDLE opened = INVALID_HANDLE_VALUE;
    const NTSTATUS status = native.createFile(
        &opened, access | SYNCHRONIZE, &object, &io, nullptr, attributes,
        share, disposition,
        options | kFileSynchronousIoNonAlert | kFileOpenReparsePoint,
        nullptr, 0);
    if (status < 0) {
        output.result = NtFailure(status);
        return output;
    }
    output.handle = ScopedHandle(opened);
    output.information = io.Information;
    return output;
}

WindowsFilesystemBackendResult DuplicateNativeHandle(
    HANDLE source,
    ScopedHandle& output) {
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return Win32Failure(GetLastError());
    }
    output.reset(duplicate);
    return {};
}

struct ParentAndLeaf {
    ScopedHandle parent;
    std::vector<ScopedHandle> parentPins;
    std::wstring leaf;
};

struct PinnedRootOpen {
    ScopedHandle operation;
    std::vector<ScopedHandle> ancestorPins;
    WindowsFilesystemObservation observation;
    std::wstring finalPath;
};

WindowsFilesystemBackendResult OpenPinnedAbsoluteRoot(
    const std::wstring& normalized,
    ACCESS_MASK operationAccess,
    PinnedRootOpen& output) {
    output = {};
    const ACCESS_MASK pinAccess =
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
    const ULONG pinShare = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const std::wstring volumePath = normalized.substr(0, 3);
    const std::wstring extendedVolumePath = L"\\\\?\\" + volumePath;
    ScopedHandle volumePin(CreateFileW(
        extendedVolumePath.c_str(), pinAccess, pinShare, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!volumePin.valid()) return Win32Failure(GetLastError());
    WindowsFilesystemObservation volumeObservation;
    WindowsFilesystemBackendResult observed =
        ObservationForHandle(volumePin.get(), volumeObservation);
    if (!observed.ok()) return observed;
    if (volumeObservation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              ERROR_REPARSE_TAG_INVALID);
    }
    if (volumeObservation.kind != WindowsFilesystemEntryKind::Directory) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                              ERROR_DIRECTORY);
    }
    std::wstring volumeFinalPath;
    WindowsFilesystemBackendResult finalPathResult =
        FinalPathForHandle(volumePin.get(), volumeFinalPath);
    if (!finalPathResult.ok()) return finalPathResult;
    if (!EqualPathExact(volumeFinalPath, ComparableFinalPath(volumePath))) {
        return BackendFailure(
            WindowsFilesystemBackendStatus::ExactPathMismatch,
            ERROR_INVALID_DATA);
    }
    output.ancestorPins.push_back(std::move(volumePin));

    std::size_t begin = 3;
    while (begin < normalized.size()) {
        const std::size_t separator = normalized.find(L'\\', begin);
        const bool isRoot = separator == std::wstring::npos;
        const std::wstring component = normalized.substr(
            begin, isRoot ? std::wstring::npos : separator - begin);
        NativeOpenResult opened = OpenRelativeWide(
            output.ancestorPins.back().get(), component,
            isRoot ? operationAccess : pinAccess,
            isRoot
                ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
                : pinShare,
            kFileOpen, kFileDirectoryFile);
        if (!opened.result.ok()) return opened.result;
        WindowsFilesystemObservation componentObservation;
        observed = ObservationForHandle(opened.handle.get(), componentObservation);
        if (!observed.ok()) return observed;
        if (componentObservation.kind ==
            WindowsFilesystemEntryKind::ReparsePoint) {
            return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                  ERROR_REPARSE_TAG_INVALID);
        }
        if (componentObservation.kind != WindowsFilesystemEntryKind::Directory) {
            return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                                  ERROR_DIRECTORY);
        }
        if (isRoot) {
            output.observation = componentObservation;
            output.operation = std::move(opened.handle);
            break;
        }
        output.ancestorPins.push_back(std::move(opened.handle));
        begin = separator + 1U;
    }
    if (!output.operation.valid()) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                              ERROR_DIRECTORY);
    }
    finalPathResult = FinalPathForHandle(output.operation.get(), output.finalPath);
    if (!finalPathResult.ok()) return finalPathResult;
    if (!EqualPathExact(output.finalPath, ComparableFinalPath(normalized))) {
        return BackendFailure(
            WindowsFilesystemBackendStatus::ExactPathMismatch,
            ERROR_INVALID_DATA);
    }
    return {};
}

WindowsFilesystemBackendResult OpenContainingDirectory(
    HANDLE root,
    const std::wstring& relativePath,
    ParentAndLeaf& output) {
    output = {};
    const std::size_t separator = relativePath.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        output.leaf = relativePath;
        return DuplicateNativeHandle(root, output.parent);
    }

    output.leaf = relativePath.substr(separator + 1);
    const std::wstring parentPath = relativePath.substr(0, separator);
    const ACCESS_MASK pinAccess =
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES;
    const ACCESS_MASK operationAccess =
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE |
        FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
        READ_CONTROL;
    HANDLE current = root;
    std::size_t begin = 0;
    while (begin < parentPath.size()) {
        const std::size_t next = parentPath.find(L'\\', begin);
        const bool deepest = next == std::wstring::npos;
        const std::wstring component = parentPath.substr(
            begin, deepest ? std::wstring::npos : next - begin);
        NativeOpenResult pin = OpenRelativeWide(
            current, component, pinAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            kFileOpen, kFileDirectoryFile);
        if (!pin.result.ok()) return pin.result;
        WindowsFilesystemObservation pinObservation;
        WindowsFilesystemBackendResult observed =
            ObservationForHandle(pin.handle.get(), pinObservation);
        if (!observed.ok()) return observed;
        if (pinObservation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                  ERROR_REPARSE_TAG_INVALID);
        }
        if (pinObservation.kind != WindowsFilesystemEntryKind::Directory) {
            return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                                  ERROR_DIRECTORY);
        }
        if (deepest) {
            NativeOpenResult operation = OpenRelativeWide(
                current, component, operationAccess,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                kFileOpen, kFileDirectoryFile);
            if (!operation.result.ok()) return operation.result;
            WindowsFilesystemObservation operationObservation;
            observed = ObservationForHandle(operation.handle.get(),
                                            operationObservation);
            if (!observed.ok()) return observed;
            if (operationObservation.kind != WindowsFilesystemEntryKind::Directory ||
                operationObservation.identity != pinObservation.identity) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::IdentityMismatch,
                    ERROR_INVALID_DATA);
            }
            output.parentPins.push_back(std::move(pin.handle));
            output.parent = std::move(operation.handle);
            break;
        }
        output.parentPins.push_back(std::move(pin.handle));
        current = output.parentPins.back().get();
        begin = next + 1U;
    }
    return {};
}

WindowsFilesystemBackendResult FlushDirectory(HANDLE directory) {
    if (!FlushFileBuffers(directory)) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              GetLastError());
    }
    return {};
}

// The authority places its verified Bootstrap copy in this one adapter-owned
// subdirectory before launching it.  Keep that namespace narrow: the
// directory itself must be a real directory and its only child must be the
// published Bootstrap executable.  Everything else remains a journal-root
// namespace violation.
WindowsFilesystemBackendResult ValidateRuntimePairNamespaceHandle(
    HANDLE runtimePairRoot) {
    std::vector<NativeDirectoryEntrySnapshot> entries;
    const WindowsFilesystemBackendResult enumerated =
        ReadDirectoryEntriesNoFollow(runtimePairRoot, entries);
    if (!enumerated.ok()) return enumerated;

    bool bootstrapPresent = false;
    for (const NativeDirectoryEntrySnapshot& entry : entries) {
        const std::wstring& name = entry.name;
        if (name == L"." || name == L"..") continue;
        if (!EqualPathInsensitive(name, kRuntimePairBootstrapName) ||
            (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                  ERROR_FILE_EXISTS);
        }
        if (bootstrapPresent) {
            // A case-sensitive directory can contain two spellings that the
            // Windows path layer resolves as the same adapter-owned leaf.
            return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                  ERROR_FILE_EXISTS);
        }
        bootstrapPresent = true;
    }
    if (!bootstrapPresent) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotFound,
                              ERROR_FILE_NOT_FOUND);
    }
    return {};
}

WindowsFilesystemBackendResult ValidateRuntimePairNamespace(
    HANDLE transactionRoot) {
    NativeOpenResult runtimePair;
    WindowsFilesystemObservation runtimePairObservation;
    const WindowsFilesystemBackendResult opened = OpenObservedRelative(
        transactionRoot, kRuntimePairDirectoryName,
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        kFileDirectoryFile, runtimePair, runtimePairObservation);
    if (opened.status == WindowsFilesystemBackendStatus::NotFound) {
        return {};
    }
    if (!opened.ok()) return opened;
    if (runtimePairObservation.kind ==
        WindowsFilesystemEntryKind::ReparsePoint) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              ERROR_REPARSE_TAG_INVALID);
    }
    if (runtimePairObservation.kind != WindowsFilesystemEntryKind::Directory) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                              ERROR_DIRECTORY);
    }

    return ValidateRuntimePairNamespaceHandle(runtimePair.handle.get());
}

WindowsFilesystemBackendResult RequireRuntimePairMissing(HANDLE transactionRoot) {
    NativeOpenResult unexpected;
    WindowsFilesystemObservation observation;
    const WindowsFilesystemBackendResult observed = OpenObservedRelative(
        transactionRoot, kRuntimePairDirectoryName, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, unexpected,
        observation);
    if (observed.status == WindowsFilesystemBackendStatus::NotFound) {
        return {};
    }
    if (!observed.ok()) return observed;
    if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              ERROR_REPARSE_TAG_INVALID);
    }
    return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                          ERROR_FILE_EXISTS);
}

WindowsFilesystemBackendResult ValidateJournalNamespace(HANDLE directory) {
    std::vector<NativeDirectoryEntrySnapshot> entries;
    const WindowsFilesystemBackendResult enumerated =
        ReadDirectoryEntriesNoFollow(directory, entries);
    if (!enumerated.ok()) return enumerated;

    WindowsFilesystemBackendResult result;
    bool runtimePairPresent = false;
    std::set<std::wstring> observedRuntimePairNames;
    for (const NativeDirectoryEntrySnapshot& entry : entries) {
        const std::wstring& name = entry.name;
        if (name != L"." && name != L".." && name != L"backup" &&
            name != kJournalSlotAName && name != kJournalSlotBName &&
            name != kJournalSlotATempName && name != kJournalSlotBTempName &&
            !EqualPathInsensitive(name, kRuntimePairDirectoryName)) {
            result = BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                    ERROR_FILE_EXISTS);
            break;
        }
        if (EqualPathInsensitive(name, kRuntimePairDirectoryName)) {
            std::wstring folded = name;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](wchar_t value) {
                               return static_cast<wchar_t>(std::towlower(value));
                           });
            if (!observedRuntimePairNames.insert(std::move(folded)).second) {
                result = BackendFailure(
                    WindowsFilesystemBackendStatus::AlreadyExists,
                    ERROR_FILE_EXISTS);
                break;
            }
            runtimePairPresent = true;
        }
        if (!result.ok()) {
            break;
        }
    }
    if (result.ok() && runtimePairPresent) {
        result = ValidateRuntimePairNamespace(directory);
    }
    return result;
}

// The backup directory is a separate adapter-owned namespace.  Validate its
// complete child set before recovery promotes a journal or opens a rollback
// object.  Enumeration is performed from the already authenticated directory
// handle, so a rename plus same-name replacement cannot redirect the check.
// Unknown names, temporary leaves, reparse points, and directories are all
// fail-closed.
WindowsFilesystemBackendResult ValidateBackupNamespace(
    HANDLE transactionRoot,
    const std::vector<std::string>& expectedEntries,
    const std::vector<std::string>& allowedMissingEntries,
    const bool expectDirectory,
    const WindowsFileIdentity& expectedDirectoryIdentity) {
    std::set<std::wstring> expected;
    for (const std::string& entry : expectedEntries) {
        std::wstring wide;
        if (!RelativePathToWide(entry, wide) || wide.find(L'\\') !=
                                                   std::wstring::npos) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        std::transform(wide.begin(), wide.end(), wide.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (!expected.insert(std::move(wide)).second) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_DUP_NAME);
        }
    }

    NativeOpenResult backup;
    WindowsFilesystemObservation observation;
    const WindowsFilesystemBackendResult opened = OpenObservedRelative(
        transactionRoot, L"backup",
        FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        kFileDirectoryFile, backup, observation);
    if (opened.status == WindowsFilesystemBackendStatus::NotFound) {
        return expectDirectory
            ? BackendFailure(WindowsFilesystemBackendStatus::NotFound,
                             ERROR_FILE_NOT_FOUND)
            : WindowsFilesystemBackendResult{};
    }
    if (!opened.ok()) return opened;
    if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              ERROR_REPARSE_TAG_INVALID);
    }
    if (observation.kind != WindowsFilesystemEntryKind::Directory) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                              ERROR_DIRECTORY);
    }
    if (expectDirectory && expectedDirectoryIdentity.valid &&
        observation.identity != expectedDirectoryIdentity) {
        return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                              ERROR_INVALID_DATA);
    }
    if (!expectDirectory) {
        return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                              ERROR_FILE_EXISTS);
    }

    std::vector<NativeDirectoryEntrySnapshot> entries;
    const WindowsFilesystemBackendResult enumerated =
        ReadDirectoryEntriesNoFollow(backup.handle.get(), entries);
    if (!enumerated.ok()) return enumerated;

    WindowsFilesystemBackendResult result;
    std::set<std::wstring> observed;
    for (const NativeDirectoryEntrySnapshot& entry : entries) {
        const std::wstring& name = entry.name;
        if (name == L"." || name == L"..") continue;
        std::wstring folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (expected.find(folded) == expected.end() ||
            !observed.insert(folded).second ||
            (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            result = BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                    ERROR_FILE_EXISTS);
            break;
        }
    }
    std::set<std::wstring> allowedMissing;
    for (const std::string& entry : allowedMissingEntries) {
        std::wstring wide;
        if (!RelativePathToWide(entry, wide) ||
            wide.find(L'\\') != std::wstring::npos) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        std::transform(wide.begin(), wide.end(), wide.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (expected.find(wide) == expected.end() ||
            !allowedMissing.insert(std::move(wide)).second) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::IdentityMismatch,
                ERROR_DUP_NAME);
        }
    }
    if (result.ok()) {
        for (const std::wstring& expectedEntry : expected) {
            if (observed.find(expectedEntry) == observed.end() &&
                allowedMissing.find(expectedEntry) == allowedMissing.end()) {
                // A missing required leaf is as ambiguous as an unknown extra
                // child and must stop recovery before promotion/mutation.
                result = BackendFailure(
                    WindowsFilesystemBackendStatus::NotFound,
                    ERROR_FILE_NOT_FOUND);
                break;
            }
        }
    }
    return result;
}

// Validate the children of an already-bound backup directory handle.  The
// caller has separately authenticated the handle's identity and role; this
// helper deliberately never reopens `transactionRoot\\backup` by name.
WindowsFilesystemBackendResult ValidateBackupNamespaceBound(
    HANDLE backupDirectory,
    const std::vector<std::string>& expectedEntries,
    const std::vector<std::string>& allowedMissingEntries) {
    std::set<std::wstring> expected;
    for (const std::string& entry : expectedEntries) {
        std::wstring wide;
        if (!RelativePathToWide(entry, wide) || wide.find(L'\\') !=
                                                   std::wstring::npos) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        std::transform(wide.begin(), wide.end(), wide.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (!expected.insert(std::move(wide)).second) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_DUP_NAME);
        }
    }

    std::set<std::wstring> allowedMissing;
    for (const std::string& entry : allowedMissingEntries) {
        std::wstring wide;
        if (!RelativePathToWide(entry, wide) ||
            wide.find(L'\\') != std::wstring::npos) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        std::transform(wide.begin(), wide.end(), wide.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (expected.find(wide) == expected.end() ||
            !allowedMissing.insert(std::move(wide)).second) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_DUP_NAME);
        }
    }

    std::vector<NativeDirectoryEntrySnapshot> entries;
    const WindowsFilesystemBackendResult enumerated =
        ReadDirectoryEntriesNoFollow(backupDirectory, entries);
    if (!enumerated.ok()) return enumerated;

    std::set<std::wstring> observed;
    for (const NativeDirectoryEntrySnapshot& entry : entries) {
        const std::wstring& name = entry.name;
        if (name == L"." || name == L"..") continue;
        std::wstring folded = name;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        if (expected.find(folded) == expected.end() ||
            !observed.insert(folded).second ||
            (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                  ERROR_FILE_EXISTS);
        }
    }
    for (const std::wstring& expectedEntry : expected) {
        if (observed.find(expectedEntry) == observed.end() &&
            allowedMissing.find(expectedEntry) == allowedMissing.end()) {
            return BackendFailure(WindowsFilesystemBackendStatus::NotFound,
                                  ERROR_FILE_NOT_FOUND);
        }
    }
    return {};
}

WindowsFilesystemBackendResult WriteBytes(
    HANDLE file,
    const std::uint8_t* bytes,
    std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            size - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, nullptr)) {
            return Win32Failure(GetLastError());
        }
        if (written == 0) {
            return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                  ERROR_WRITE_FAULT);
        }
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

WindowsFilesystemBackendResult ReadBytesBounded(
    HANDLE file,
    const std::size_t maximumBytes,
    std::vector<std::uint8_t>& output) {
    output.clear();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return Win32Failure(GetLastError());
    if (size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_FILE_TOO_LARGE);
    }
    output.resize(static_cast<std::size_t>(size.QuadPart));
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        output.clear();
        return Win32Failure(GetLastError());
    }
    std::size_t offset = 0;
    while (offset < output.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(
            output.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(file, output.data() + offset, chunk, &read, nullptr)) {
            output.clear();
            return Win32Failure(GetLastError());
        }
        if (read == 0U) {
            output.clear();
            return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                  ERROR_HANDLE_EOF);
        }
        offset += static_cast<std::size_t>(read);
    }
    return {};
}

WindowsFilesystemBackendResult CopyHandleContents(HANDLE source, HANDLE target) {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN)) {
        return Win32Failure(GetLastError());
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            return Win32Failure(GetLastError());
        }
        if (read == 0) break;
        const WindowsFilesystemBackendResult written =
            WriteBytes(target, buffer.data(), static_cast<std::size_t>(read));
        if (!written.ok()) return written;
    }
    return {};
}

WindowsFilesystemBackendResult FingerprintHandle(
    HANDLE file,
    WindowsFileContentFingerprint& output) {
    output = {};
    const CryptoFunctions& crypto = CryptoApi();
    if (!crypto.available()) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_PROC_NOT_FOUND);
    }

    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    NTSTATUS status = crypto.openAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) return NtFailure(status);
    ScopedAlgorithm algorithm(crypto, rawAlgorithm);

    DWORD objectSize = 0;
    DWORD returned = 0;
    status = crypto.getProperty(
        algorithm.get(), BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0);
    if (status < 0 || returned != sizeof(objectSize) || objectSize == 0) {
        return status < 0
            ? NtFailure(status)
            : BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                             ERROR_INVALID_DATA);
    }

    DWORD hashSize = 0;
    returned = 0;
    status = crypto.getProperty(
        algorithm.get(), BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &returned, 0);
    if (status < 0 || returned != sizeof(hashSize) ||
        hashSize != output.sha256.size()) {
        return status < 0
            ? NtFailure(status)
            : BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                             ERROR_INVALID_DATA);
    }

    std::vector<std::uint8_t> hashObject(static_cast<std::size_t>(objectSize));
    BCRYPT_HASH_HANDLE rawHash = nullptr;
    status = crypto.createHash(
        algorithm.get(), &rawHash, hashObject.data(), objectSize,
        nullptr, 0, 0);
    if (status < 0) return NtFailure(status);
    ScopedHash hash(crypto, rawHash);

    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return Win32Failure(GetLastError());
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t byteSize = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr)) {
            return Win32Failure(GetLastError());
        }
        if (read == 0) break;
        status = crypto.hashData(hash.get(), buffer.data(), read, 0);
        if (status < 0) return NtFailure(status);
        if (byteSize > (std::numeric_limits<std::uint64_t>::max)() - read) {
            return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                                  ERROR_ARITHMETIC_OVERFLOW);
        }
        byteSize += static_cast<std::uint64_t>(read);
    }

    status = crypto.finishHash(hash.get(), output.sha256.data(), hashSize, 0);
    if (status < 0) {
        output = {};
        return NtFailure(status);
    }
    output.valid = true;
    output.byteSize = byteSize;
    return {};
}

WindowsFilesystemBackendResult FingerprintBytes(
    const std::vector<std::uint8_t>& bytes,
    WindowsFileContentFingerprint& output) {
    output = {};
    if (bytes.size() >
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_ARITHMETIC_OVERFLOW);
    }
    const CryptoFunctions& crypto = CryptoApi();
    if (!crypto.available()) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_PROC_NOT_FOUND);
    }

    BCRYPT_ALG_HANDLE rawAlgorithm = nullptr;
    const NTSTATUS opened = crypto.openAlgorithmProvider(
        &rawAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (opened < 0) return NtFailure(opened);
    ScopedAlgorithm algorithm(crypto, rawAlgorithm);

    const ULONG inputSize = static_cast<ULONG>(bytes.size());
    const NTSTATUS hashed = crypto.hash(
        algorithm.get(), nullptr, 0,
        bytes.empty() ? nullptr : const_cast<PUCHAR>(bytes.data()), inputSize,
        output.sha256.data(), static_cast<ULONG>(output.sha256.size()));
    if (hashed < 0) return NtFailure(hashed);
    output.valid = true;
    output.byteSize = static_cast<std::uint64_t>(bytes.size());
    return {};
}

bool ContentMatches(const WindowsFileContentFingerprint& left,
                    const WindowsFileContentFingerprint& right) noexcept {
    if (!left.valid || !right.valid || left.byteSize != right.byteSize) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.sha256.size(); ++index) {
        difference = static_cast<std::uint8_t>(
            difference | (left.sha256[index] ^ right.sha256[index]));
    }
    return difference == 0;
}

WindowsFilesystemBackendResult ContentMismatch() noexcept {
    return BackendFailure(WindowsFilesystemBackendStatus::ContentMismatch,
                          ERROR_CRC);
}

WindowsFilesystemBackendResult UnsupportedMetadata(
    const DWORD nativeError = ERROR_NOT_SUPPORTED) noexcept {
    return BackendFailure(WindowsFilesystemBackendStatus::UnsupportedMetadata,
                          nativeError);
}

WindowsFilesystemBackendResult MetadataApplyFailure(const DWORD nativeError) {
    // Preserve access failures while the temporary is unpublished. The
    // caller maps any failure after publication to DurabilityFailed.
    if (nativeError == ERROR_ACCESS_DENIED ||
        nativeError == ERROR_INVALID_OWNER ||
        nativeError == ERROR_PRIVILEGE_NOT_HELD) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              nativeError);
    }
    return Win32Failure(nativeError);
}

void AppendMetadataU32(std::vector<std::uint8_t>& bytes,
                       const std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendMetadataI64(std::vector<std::uint8_t>& bytes,
                       const std::int64_t value) {
    const std::uint64_t encoded = static_cast<std::uint64_t>(value);
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(encoded >> shift));
    }
}

bool ReadMetadataU32(const std::vector<std::uint8_t>& bytes,
                     std::size_t& offset,
                     std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = 0;
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return true;
}

bool ReadMetadataI64(const std::vector<std::uint8_t>& bytes,
                     std::size_t& offset,
                     std::int64_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) return false;
    std::uint64_t encoded = 0;
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        encoded |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    value = static_cast<std::int64_t>(encoded);
    return true;
}

WindowsFilesystemBackendResult ValidateSupportedMetadata(HANDLE file) {
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                      sizeof(standard))) {
        return Win32Failure(GetLastError());
    }
    if (standard.NumberOfLinks != 1U) {
        return UnsupportedMetadata(ERROR_TOO_MANY_LINKS);
    }

    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                      sizeof(basic))) {
        return Win32Failure(GetLastError());
    }
    constexpr DWORD supportedAttributes =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if ((basic.FileAttributes & ~supportedAttributes) != 0U) {
        return UnsupportedMetadata(ERROR_INVALID_FLAGS);
    }

    std::vector<std::uint8_t> streamBuffer(64U * 1024U);
    if (!GetFileInformationByHandleEx(file, FileStreamInfo,
                                      streamBuffer.data(),
                                      static_cast<DWORD>(streamBuffer.size()))) {
        const DWORD error = GetLastError();
        return error == ERROR_MORE_DATA ? UnsupportedMetadata(ERROR_MORE_DATA)
                                        : Win32Failure(error);
    }
    std::size_t offset = 0;
    for (;;) {
        if (offset > streamBuffer.size() ||
            streamBuffer.size() - offset < offsetof(FILE_STREAM_INFO, StreamName)) {
            return UnsupportedMetadata(ERROR_INVALID_DATA);
        }
        const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(
            streamBuffer.data() + offset);
        if ((stream->StreamNameLength % sizeof(wchar_t)) != 0U ||
            stream->StreamNameLength > streamBuffer.size() - offset -
                offsetof(FILE_STREAM_INFO, StreamName)) {
            return UnsupportedMetadata(ERROR_INVALID_DATA);
        }
        const std::wstring name(
            stream->StreamName,
            stream->StreamNameLength / sizeof(wchar_t));
        if (name != L"::$DATA") {
            return UnsupportedMetadata(ERROR_NOT_SUPPORTED);
        }
        if (stream->NextEntryOffset == 0U) break;
        if (stream->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) ||
            stream->NextEntryOffset > streamBuffer.size() - offset) {
            return UnsupportedMetadata(ERROR_INVALID_DATA);
        }
        offset += stream->NextEntryOffset;
    }

    struct NativeEaInformation {
        ULONG size = 0;
    } ea;
    const NativeFunctions& native = NativeApi();
    if (native.queryInformationFile == nullptr) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_PROC_NOT_FOUND);
    }
    IO_STATUS_BLOCK io{};
    constexpr FILE_INFORMATION_CLASS kFileEaInformationClass =
        static_cast<FILE_INFORMATION_CLASS>(7);
    const NTSTATUS queried = native.queryInformationFile(
        file, &io, &ea, sizeof(ea), kFileEaInformationClass);
    if (queried < 0) return NtFailure(queried);
    return WindowsFilesystemBackendResult{};
}

WindowsFilesystemBackendResult CaptureSecurityMetadata(
    HANDLE file,
    WindowsFileMetadataSnapshot& output) {
    output = {};
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    const DWORD securityResult = GetSecurityInfo(
        file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &rawDescriptor);
    ScopedLocalFree descriptorOwner(rawDescriptor);
    PSECURITY_DESCRIPTOR descriptor =
        static_cast<PSECURITY_DESCRIPTOR>(descriptorOwner.get());
    if (securityResult != ERROR_SUCCESS || descriptor == nullptr) {
        return Win32Failure(securityResult == ERROR_SUCCESS
                                ? ERROR_INVALID_SECURITY_DESCR
                                : securityResult);
    }
    const DWORD descriptorLength = GetSecurityDescriptorLength(descriptor);
    if (descriptorLength == 0U || descriptorLength > kMaxRollbackMetadataBytes ||
        !IsValidSecurityDescriptor(descriptor)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }

    PSECURITY_DESCRIPTOR rawLabelDescriptor = nullptr;
    const DWORD labelResult = GetSecurityInfo(
        file, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &rawLabelDescriptor);
    ScopedLocalFree labelOwner(rawLabelDescriptor);
    PSECURITY_DESCRIPTOR labelDescriptor =
        static_cast<PSECURITY_DESCRIPTOR>(labelOwner.get());
    if (labelResult != ERROR_SUCCESS || labelDescriptor == nullptr) {
        return Win32Failure(labelResult == ERROR_SUCCESS
                                ? ERROR_INVALID_SECURITY_DESCR
                                : labelResult);
    }
    const DWORD labelLength = GetSecurityDescriptorLength(labelDescriptor);
    if (labelLength == 0U || labelLength > kMaxRollbackMetadataBytes ||
        !IsValidSecurityDescriptor(labelDescriptor)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }

    // Keep the same self-contained envelope as CaptureMetadata, with zeroed
    // basic fields. SecurityMetadataMatches intentionally ignores those
    // fields when comparing a parent/default descriptor.
    output.bytes.reserve(48U + descriptorLength + labelLength);
    AppendMetadataU32(output.bytes, UINT32_C(0x324D4241));
    AppendMetadataI64(output.bytes, 0);
    AppendMetadataI64(output.bytes, 0);
    AppendMetadataI64(output.bytes, 0);
    AppendMetadataI64(output.bytes, 0);
    AppendMetadataU32(output.bytes, 0);
    AppendMetadataU32(output.bytes, descriptorLength);
    output.bytes.insert(output.bytes.end(),
                        reinterpret_cast<const std::uint8_t*>(descriptor),
                        reinterpret_cast<const std::uint8_t*>(descriptor) +
                            descriptorLength);
    AppendMetadataU32(output.bytes, labelLength);
    output.bytes.insert(output.bytes.end(),
                        reinterpret_cast<const std::uint8_t*>(labelDescriptor),
                        reinterpret_cast<const std::uint8_t*>(labelDescriptor) +
                            labelLength);
    output.valid = true;
    return {};
}

bool QueryCurrentUserSid(std::vector<std::uint8_t>& storage) {
    HANDLE rawToken = nullptr;
    const HANDLE currentProcess =
        reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-1));
    if (!OpenProcessToken(currentProcess, TOKEN_QUERY, &rawToken)) return false;
    ScopedHandle token(rawToken);
    DWORD required = 0U;
    static_cast<void>(GetTokenInformation(token.get(), TokenUser, nullptr, 0U,
                                          &required));
    if (required == 0U || required > 64U * 1024U) {
        return false;
    }
    storage.resize(required);
    const bool queried = GetTokenInformation(
        token.get(), TokenUser, storage.data(), required, &required) != FALSE;
    return queried && storage.size() >= sizeof(TOKEN_USER) &&
        IsValidSid(reinterpret_cast<const TOKEN_USER*>(storage.data())
                       ->User.Sid) != FALSE;
}

bool TrustedStorageWriterSid(PSID sid, PSID currentUser) {
    return sid != nullptr && IsValidSid(sid) != FALSE &&
        (EqualSid(sid, currentUser) != FALSE ||
         IsWellKnownSid(sid, WinLocalSystemSid) != FALSE ||
         IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != FALSE ||
         IsWellKnownSid(sid, WinCreatorOwnerSid) != FALSE);
}

bool AllowedAceSidAndMask(const ACE_HEADER* header, PSID& sid, ACCESS_MASK& mask) {
    sid = nullptr;
    mask = 0U;
    if (header == nullptr) return false;
    switch (header->AceType) {
        case ACCESS_ALLOWED_ACE_TYPE: {
            const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
            return true;
        }
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE: {
            const auto* ace =
                reinterpret_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(header);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
            return true;
        }
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE: {
            const auto* ace =
                reinterpret_cast<const ACCESS_ALLOWED_CALLBACK_ACE*>(header);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
            return true;
        }
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE: {
            const auto* ace = reinterpret_cast<
                const ACCESS_ALLOWED_CALLBACK_OBJECT_ACE*>(header);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
            return true;
        }
        default:
            return false;
    }
}

WindowsFilesystemBackendResult ValidateTrustedStorageSecurity(HANDLE directory) {
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const DWORD securityResult = GetSecurityInfo(
        directory, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &rawDescriptor);
    ScopedLocalFree descriptorOwner(rawDescriptor);
    std::vector<std::uint8_t> currentUserStorage;
    if (securityResult != ERROR_SUCCESS || rawDescriptor == nullptr ||
        owner == nullptr || dacl == nullptr || IsValidAcl(dacl) == FALSE ||
        !QueryCurrentUserSid(currentUserStorage)) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_ACCESS_DENIED);
    }
    PSID currentUser = reinterpret_cast<TOKEN_USER*>(currentUserStorage.data())
                           ->User.Sid;
    if (EqualSid(owner, currentUser) == FALSE) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_ACCESS_DENIED);
    }
    constexpr ACCESS_MASK kMutationRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC |
        WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
    for (DWORD index = 0U; index < dacl->AceCount; ++index) {
        ACE_HEADER* header = nullptr;
        if (GetAce(dacl, index, reinterpret_cast<void**>(&header)) == FALSE ||
            header == nullptr) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
        PSID sid = nullptr;
        ACCESS_MASK mask = 0U;
        if (!AllowedAceSidAndMask(header, sid, mask)) continue;
        if ((mask & kMutationRights) != 0U &&
            !TrustedStorageWriterSid(sid, currentUser)) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
    }

    PSECURITY_DESCRIPTOR rawLabel = nullptr;
    PACL labelAcl = nullptr;
    const DWORD labelResult = GetSecurityInfo(
        directory, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, &labelAcl, &rawLabel);
    ScopedLocalFree labelOwner(rawLabel);
    if (labelResult != ERROR_SUCCESS || rawLabel == nullptr ||
        (labelAcl != nullptr && IsValidAcl(labelAcl) == FALSE)) {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_ACCESS_DENIED);
    }
    if (labelAcl != nullptr) {
        for (DWORD index = 0U; index < labelAcl->AceCount; ++index) {
            ACE_HEADER* header = nullptr;
            if (GetAce(labelAcl, index, reinterpret_cast<void**>(&header)) == FALSE ||
                header == nullptr) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::AccessDenied,
                    ERROR_ACCESS_DENIED);
            }
            if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) continue;
            const auto* label =
                reinterpret_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(header);
            PSID labelSid = const_cast<DWORD*>(&label->SidStart);
            const PUCHAR subAuthorityCount =
                IsValidSid(labelSid) != FALSE
                    ? GetSidSubAuthorityCount(labelSid)
                    : nullptr;
            const PDWORD integrityRid =
                subAuthorityCount != nullptr && *subAuthorityCount != 0U
                    ? GetSidSubAuthority(
                          labelSid,
                          static_cast<DWORD>(*subAuthorityCount - 1U))
                    : nullptr;
            if (subAuthorityCount == nullptr || *subAuthorityCount == 0U ||
                integrityRid == nullptr ||
                *integrityRid < SECURITY_MANDATORY_MEDIUM_RID) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::AccessDenied,
                    ERROR_ACCESS_DENIED);
            }
        }
    }
    return {};
}

WindowsFilesystemBackendResult CaptureMetadata(
    HANDLE file,
    WindowsFileMetadataSnapshot& output) {
    output = {};
    WindowsFilesystemBackendResult validated = ValidateSupportedMetadata(file);
    if (!validated.ok()) return validated;
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic,
                                      sizeof(basic))) {
        return Win32Failure(GetLastError());
    }
    WindowsFilesystemBackendResult captured =
        CaptureSecurityMetadata(file, output);
    if (!captured.ok()) return captured;
    // Replace the zeroed basic portion with the observed file attributes.
    std::size_t offset = 4U;
    auto writeI64 = [&output, &offset](const std::int64_t value) {
        for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
            output.bytes[offset++] = static_cast<std::uint8_t>(
                static_cast<std::uint64_t>(value) >> shift);
        }
    };
    writeI64(basic.CreationTime.QuadPart);
    writeI64(basic.LastAccessTime.QuadPart);
    writeI64(basic.LastWriteTime.QuadPart);
    writeI64(basic.ChangeTime.QuadPart);
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        output.bytes[offset++] = static_cast<std::uint8_t>(
            basic.FileAttributes >> shift);
    }
    return {};
}

bool DecodeBasicMetadata(const WindowsFileMetadataSnapshot& metadata,
                         FILE_BASIC_INFO& basic) {
    basic = {};
    if (!metadata.valid) return false;
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::int64_t creation = 0;
    std::int64_t access = 0;
    std::int64_t write = 0;
    std::int64_t change = 0;
    std::uint32_t attributes = 0;
    if (!ReadMetadataU32(metadata.bytes, offset, magic) ||
        magic != UINT32_C(0x324D4241) ||
        !ReadMetadataI64(metadata.bytes, offset, creation) ||
        !ReadMetadataI64(metadata.bytes, offset, access) ||
        !ReadMetadataI64(metadata.bytes, offset, write) ||
        !ReadMetadataI64(metadata.bytes, offset, change) ||
        !ReadMetadataU32(metadata.bytes, offset, attributes) ||
        offset > metadata.bytes.size()) {
        return false;
    }
    basic.CreationTime.QuadPart = creation;
    basic.LastAccessTime.QuadPart = access;
    basic.LastWriteTime.QuadPart = write;
    basic.ChangeTime.QuadPart = change;
    basic.FileAttributes = attributes;
    return true;
}

bool IsBoundedSelfRelativeSid(const std::uint8_t* bytes,
                              const std::size_t length) {
    if (bytes == nullptr || length < 8U || bytes[0] != SID_REVISION) {
        return false;
    }
    const std::size_t subAuthorityCount = bytes[1];
    const std::size_t sidLength = 8U +
        subAuthorityCount * sizeof(DWORD);
    if (sidLength > length) return false;
    const PSID sid = reinterpret_cast<PSID>(
        const_cast<std::uint8_t*>(bytes));
    return IsValidSid(sid) != FALSE && GetLengthSid(sid) == sidLength;
}

bool IsBoundedSelfRelativeAcl(const std::uint8_t* bytes,
                              const std::size_t length) {
    if (bytes == nullptr || length < sizeof(ACL)) return false;
    ACL header{};
    std::memcpy(&header, bytes, sizeof(header));
    if ((header.AclRevision != ACL_REVISION &&
         header.AclRevision != ACL_REVISION_DS) ||
        header.AclSize < sizeof(ACL) || header.AclSize > length ||
        (header.AclSize % sizeof(DWORD)) != 0U) {
        return false;
    }
    std::size_t offset = sizeof(ACL);
    for (WORD index = 0; index < header.AceCount; ++index) {
        if (header.AclSize - offset < sizeof(ACE_HEADER)) return false;
        ACE_HEADER ace{};
        std::memcpy(&ace, bytes + offset, sizeof(ace));
        if (ace.AceSize < sizeof(ACE_HEADER) ||
            (ace.AceSize % sizeof(DWORD)) != 0U ||
            ace.AceSize > header.AclSize - offset) {
            return false;
        }
        offset += ace.AceSize;
    }
    return offset <= header.AclSize && IsValidAcl(
        reinterpret_cast<PACL>(const_cast<std::uint8_t*>(bytes))) != FALSE;
}

bool IsBoundedSelfRelativeDescriptor(const std::uint8_t* bytes,
                                     const std::size_t length) {
    constexpr std::size_t headerLength = 20U;
    if (bytes == nullptr || length < headerLength ||
        (length % sizeof(DWORD)) != 0U) {
        return false;
    }
    BYTE revision = 0;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD ownerOffset = 0;
    DWORD groupOffset = 0;
    DWORD saclOffset = 0;
    DWORD daclOffset = 0;
    std::memcpy(&revision, bytes, sizeof(revision));
    std::memcpy(&control, bytes + 2U, sizeof(control));
    std::memcpy(&ownerOffset, bytes + 4U, sizeof(ownerOffset));
    std::memcpy(&groupOffset, bytes + 8U, sizeof(groupOffset));
    std::memcpy(&saclOffset, bytes + 12U, sizeof(saclOffset));
    std::memcpy(&daclOffset, bytes + 16U, sizeof(daclOffset));
    if (revision != SECURITY_DESCRIPTOR_REVISION ||
        (control & SE_SELF_RELATIVE) == 0) {
        return false;
    }
    const auto validOffset = [length, headerLength](
                                 const DWORD offset,
                                 const std::size_t minimum) {
        return offset == 0U ||
            (offset % sizeof(DWORD) == 0U &&
             static_cast<std::size_t>(offset) >= headerLength &&
             static_cast<std::size_t>(offset) <= length &&
             length - static_cast<std::size_t>(offset) >= minimum);
    };
    if (!validOffset(ownerOffset, 8U) || !validOffset(groupOffset, 8U) ||
        !validOffset(saclOffset, sizeof(ACL)) ||
        !validOffset(daclOffset, sizeof(ACL))) {
        return false;
    }
    if (ownerOffset != 0U &&
        !IsBoundedSelfRelativeSid(bytes + ownerOffset,
                                  length - ownerOffset)) {
        return false;
    }
    if (groupOffset != 0U &&
        !IsBoundedSelfRelativeSid(bytes + groupOffset,
                                  length - groupOffset)) {
        return false;
    }
    if (saclOffset != 0U &&
        !IsBoundedSelfRelativeAcl(bytes + saclOffset,
                                  length - saclOffset)) {
        return false;
    }
    if (daclOffset != 0U &&
        !IsBoundedSelfRelativeAcl(bytes + daclOffset,
                                  length - daclOffset)) {
        return false;
    }
    return IsValidSecurityDescriptor(
        reinterpret_cast<PSECURITY_DESCRIPTOR>(
            const_cast<std::uint8_t*>(bytes))) != FALSE;
}

bool DecodeSecurityMetadata(const WindowsFileMetadataSnapshot& metadata,
                            PSECURITY_DESCRIPTOR& descriptor) {
    descriptor = nullptr;
    constexpr std::size_t maximumMetadataBytes =
        48U + 2U * static_cast<std::size_t>(kMaxRollbackMetadataBytes);
    if (!metadata.valid || metadata.bytes.size() < 44U ||
        metadata.bytes.size() > maximumMetadataBytes) {
        return false;
    }
    std::size_t offset = 40U;
    std::uint32_t descriptorLength = 0;
    if (!ReadMetadataU32(metadata.bytes, offset, descriptorLength) ||
        descriptorLength == 0U || descriptorLength > kMaxRollbackMetadataBytes ||
        (descriptorLength % sizeof(DWORD)) != 0U ||
        descriptorLength > metadata.bytes.size() - offset) {
        return false;
    }
    descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::uint8_t*>(metadata.bytes.data() + offset));
    return IsBoundedSelfRelativeDescriptor(metadata.bytes.data() + offset,
                                           descriptorLength) &&
           IsValidSecurityDescriptor(descriptor) != FALSE &&
           GetSecurityDescriptorLength(descriptor) == descriptorLength;
}

bool DecodeLabelMetadata(const WindowsFileMetadataSnapshot& metadata,
                         PSECURITY_DESCRIPTOR& descriptor) {
    descriptor = nullptr;
    constexpr std::size_t maximumMetadataBytes =
        48U + 2U * static_cast<std::size_t>(kMaxRollbackMetadataBytes);
    if (!metadata.valid || metadata.bytes.size() < 48U ||
        metadata.bytes.size() > maximumMetadataBytes) {
        return false;
    }
    std::size_t offset = 40U;
    std::uint32_t discretionaryLength = 0;
    if (!ReadMetadataU32(metadata.bytes, offset, discretionaryLength) ||
        discretionaryLength == 0U ||
        discretionaryLength > kMaxRollbackMetadataBytes ||
        (discretionaryLength % sizeof(DWORD)) != 0U ||
        discretionaryLength > metadata.bytes.size() - offset) {
        return false;
    }
    offset += discretionaryLength;
    std::uint32_t labelLength = 0;
    if (!ReadMetadataU32(metadata.bytes, offset, labelLength) ||
        labelLength == 0U || labelLength > kMaxRollbackMetadataBytes ||
        (labelLength % sizeof(DWORD)) != 0U ||
        labelLength != metadata.bytes.size() - offset) {
        return false;
    }
    descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::uint8_t*>(metadata.bytes.data() + offset));
    return IsBoundedSelfRelativeDescriptor(metadata.bytes.data() + offset,
                                           labelLength) &&
           IsValidSecurityDescriptor(descriptor) != FALSE &&
           GetSecurityDescriptorLength(descriptor) == labelLength;
}

// Validate an opaque metadata value before any object is opened.  Recovery
// receives this value from a durable journal, so it must never be allowed to
// influence a native open/rename merely because a later decode happened to
// fail.  Keep this check independent of the handle-bound comparison helpers:
// it validates the complete envelope (basic fields, security descriptor, and
// mandatory integrity label) while intentionally doing no I/O.
WindowsFilesystemBackendResult ValidateMetadataSnapshotStructure(
    const WindowsFileMetadataSnapshot& metadata) {
    if (!metadata.valid) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    FILE_BASIC_INFO basic{};
    PSECURITY_DESCRIPTOR security = nullptr;
    PSECURITY_DESCRIPTOR label = nullptr;
    if (!DecodeBasicMetadata(metadata, basic)) {
        // Preserve the adapter's historical generic error for an invalid
        // envelope/magic while still failing before any native object I/O.
        return UnsupportedMetadata();
    }
    if (!DecodeSecurityMetadata(metadata, security) ||
        !DecodeLabelMetadata(metadata, label)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    constexpr DWORD supportedAttributes =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
        FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
        FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if ((basic.FileAttributes & ~supportedAttributes) != 0U) {
        return UnsupportedMetadata(ERROR_INVALID_FLAGS);
    }
    return {};
}

bool AclEquivalent(PACL left, PACL right) {
    if (left == nullptr || right == nullptr) return left == right;
    ACL_SIZE_INFORMATION leftInfo{};
    ACL_SIZE_INFORMATION rightInfo{};
    if (GetAclInformation(left, &leftInfo, sizeof(leftInfo),
                          AclSizeInformation) == FALSE ||
        GetAclInformation(right, &rightInfo, sizeof(rightInfo),
                          AclSizeInformation) == FALSE ||
        leftInfo.AceCount != rightInfo.AceCount) {
        return false;
    }
    for (DWORD index = 0; index < leftInfo.AceCount; ++index) {
        void* leftAceRaw = nullptr;
        void* rightAceRaw = nullptr;
        if (GetAce(left, index, &leftAceRaw) == FALSE ||
            GetAce(right, index, &rightAceRaw) == FALSE ||
            leftAceRaw == nullptr || rightAceRaw == nullptr) {
            return false;
        }
        const auto* leftHeader = static_cast<const ACE_HEADER*>(leftAceRaw);
        const auto* rightHeader = static_cast<const ACE_HEADER*>(rightAceRaw);
        if (leftHeader->AceSize != rightHeader->AceSize ||
            leftHeader->AceSize < sizeof(ACE_HEADER)) {
            return false;
        }
        std::vector<BYTE> leftBytes(leftHeader->AceSize);
        std::vector<BYTE> rightBytes(rightHeader->AceSize);
        std::memcpy(leftBytes.data(), leftAceRaw, leftBytes.size());
        std::memcpy(rightBytes.data(), rightAceRaw, rightBytes.size());
        auto* normalizedLeft =
            reinterpret_cast<ACE_HEADER*>(leftBytes.data());
        auto* normalizedRight =
            reinterpret_cast<ACE_HEADER*>(rightBytes.data());
        // Windows can normalize inherited provenance when a descriptor is
        // applied to a newly-created sibling.  The access payload is stable;
        // preserve the protected/unprotected boundary below.
        normalizedLeft->AceFlags &= static_cast<BYTE>(~INHERITED_ACE);
        normalizedRight->AceFlags &= static_cast<BYTE>(~INHERITED_ACE);
        if (std::memcmp(leftBytes.data(), rightBytes.data(), leftBytes.size()) !=
            0) {
            return false;
        }
    }
    return true;
}

bool SecurityMetadataEquivalent(PSECURITY_DESCRIPTOR left,
                                PSECURITY_DESCRIPTOR right) {
    PSID leftOwner = nullptr;
    PSID rightOwner = nullptr;
    PSID leftGroup = nullptr;
    PSID rightGroup = nullptr;
    BOOL leftOwnerDefaulted = FALSE;
    BOOL rightOwnerDefaulted = FALSE;
    BOOL leftGroupDefaulted = FALSE;
    BOOL rightGroupDefaulted = FALSE;
    BOOL leftDaclPresent = FALSE;
    BOOL rightDaclPresent = FALSE;
    BOOL leftDaclDefaulted = FALSE;
    BOOL rightDaclDefaulted = FALSE;
    PACL leftDacl = nullptr;
    PACL rightDacl = nullptr;
    if (GetSecurityDescriptorOwner(left, &leftOwner, &leftOwnerDefaulted) == FALSE ||
        GetSecurityDescriptorOwner(right, &rightOwner, &rightOwnerDefaulted) == FALSE ||
        GetSecurityDescriptorGroup(left, &leftGroup, &leftGroupDefaulted) == FALSE ||
        GetSecurityDescriptorGroup(right, &rightGroup, &rightGroupDefaulted) == FALSE ||
        GetSecurityDescriptorDacl(left, &leftDaclPresent, &leftDacl,
                                  &leftDaclDefaulted) == FALSE ||
        GetSecurityDescriptorDacl(right, &rightDaclPresent, &rightDacl,
                                  &rightDaclDefaulted) == FALSE) {
        return false;
    }
    const bool ownerEqual =
        (leftOwner == nullptr && rightOwner == nullptr) ||
        (leftOwner != nullptr && rightOwner != nullptr &&
         EqualSid(leftOwner, rightOwner) != FALSE);
    const bool groupEqual =
        (leftGroup == nullptr && rightGroup == nullptr) ||
        (leftGroup != nullptr && rightGroup != nullptr &&
         EqualSid(leftGroup, rightGroup) != FALSE);
    bool daclEqual = leftDaclPresent == rightDaclPresent;
    if (daclEqual && leftDaclPresent) {
        // A present NULL DACL is valid and means unrestricted access.
        daclEqual = (leftDacl == nullptr || rightDacl == nullptr)
            ? leftDacl == rightDacl
            : AclEquivalent(leftDacl, rightDacl);
    }
    SECURITY_DESCRIPTOR_CONTROL leftControl = 0;
    SECURITY_DESCRIPTOR_CONTROL rightControl = 0;
    DWORD leftRevision = 0;
    DWORD rightRevision = 0;
    if (GetSecurityDescriptorControl(left, &leftControl, &leftRevision) == FALSE ||
        GetSecurityDescriptorControl(right, &rightControl, &rightRevision) == FALSE) {
        return false;
    }
    // AUTO_INHERITED/AUTO_INHERIT_REQ/DEFAULTED are provenance flags that
    // Windows may normalize while applying an otherwise identical DACL.
    // Keep SE_DACL_PROTECTED strict because it controls future inheritance.
    constexpr SECURITY_DESCRIPTOR_CONTROL comparedControl =
        SE_DACL_PROTECTED;
    return ownerEqual && groupEqual && daclEqual &&
           (leftControl & comparedControl) == (rightControl & comparedControl);
}

bool LabelMetadataEquivalent(PSECURITY_DESCRIPTOR left,
                             PSECURITY_DESCRIPTOR right) {
    BOOL leftPresent = FALSE;
    BOOL rightPresent = FALSE;
    BOOL leftDefaulted = FALSE;
    BOOL rightDefaulted = FALSE;
    PACL leftSacl = nullptr;
    PACL rightSacl = nullptr;
    if (GetSecurityDescriptorSacl(left, &leftPresent, &leftSacl,
                                  &leftDefaulted) == FALSE ||
        GetSecurityDescriptorSacl(right, &rightPresent, &rightSacl,
                                  &rightDefaulted) == FALSE) {
        return false;
    }
    const auto semanticAclState = [](const BOOL present, PACL acl) {
        if (!present || acl == nullptr) return 0;
        ACL_SIZE_INFORMATION info{};
        if (GetAclInformation(acl, &info, sizeof(info),
                              AclSizeInformation) == FALSE) {
            return -1;
        }
        return info.AceCount == 0U ? 0 : 1;
    };
    const int leftState = semanticAclState(leftPresent, leftSacl);
    const int rightState = semanticAclState(rightPresent, rightSacl);
    if (leftState < 0 || rightState < 0) return false;
    const bool saclEqual = leftState == 0 || rightState == 0
        ? leftState == rightState
        : AclEquivalent(leftSacl, rightSacl);
    SECURITY_DESCRIPTOR_CONTROL leftControl = 0;
    SECURITY_DESCRIPTOR_CONTROL rightControl = 0;
    DWORD leftRevision = 0;
    DWORD rightRevision = 0;
    if (GetSecurityDescriptorControl(left, &leftControl, &leftRevision) == FALSE ||
        GetSecurityDescriptorControl(right, &rightControl, &rightRevision) == FALSE) {
        return false;
    }
    // Windows may normalize absent-vs-empty label ACLs and inherited/defaulted
    // provenance while preserving the effective mandatory-label policy. Keep
    // the actual ACE payload and the protected inheritance boundary strict.
    constexpr SECURITY_DESCRIPTOR_CONTROL comparedControl =
        SE_SACL_PROTECTED;
    return saclEqual &&
           (leftControl & comparedControl) == (rightControl & comparedControl);
}

DWORD MetadataDiagnosticError(const DWORD diagnosticCode) noexcept {
#ifdef ARIS_BOOTSTRAP_TEST_DIAGNOSTICS
    return diagnosticCode;
#else
    static_cast<void>(diagnosticCode);
    return ERROR_INVALID_DATA;
#endif
}

bool MetadataEquivalent(const WindowsFileMetadataSnapshot& observed,
                        const WindowsFileMetadataSnapshot& expected,
                        const bool ignoreLastAccess,
                        const bool ignoreChangeTime = false) {
    FILE_BASIC_INFO observedBasic{};
    FILE_BASIC_INFO expectedBasic{};
    if (!DecodeBasicMetadata(observed, observedBasic) ||
        !DecodeBasicMetadata(expected, expectedBasic)) {
        return false;
    }
    if (observedBasic.CreationTime.QuadPart !=
            expectedBasic.CreationTime.QuadPart ||
        (!ignoreLastAccess && observedBasic.LastAccessTime.QuadPart !=
            expectedBasic.LastAccessTime.QuadPart) ||
        observedBasic.LastWriteTime.QuadPart !=
            expectedBasic.LastWriteTime.QuadPart ||
        (!ignoreChangeTime &&
         observedBasic.ChangeTime.QuadPart !=
             expectedBasic.ChangeTime.QuadPart) ||
        observedBasic.FileAttributes != expectedBasic.FileAttributes) {
        return false;
    }
    PSECURITY_DESCRIPTOR observedSecurity = nullptr;
    PSECURITY_DESCRIPTOR expectedSecurity = nullptr;
    if (!DecodeSecurityMetadata(observed, observedSecurity) ||
        !DecodeSecurityMetadata(expected, expectedSecurity)) {
        return false;
    }
    PSECURITY_DESCRIPTOR observedLabel = nullptr;
    PSECURITY_DESCRIPTOR expectedLabel = nullptr;
    return SecurityMetadataEquivalent(observedSecurity, expectedSecurity) &&
           DecodeLabelMetadata(observed, observedLabel) &&
           DecodeLabelMetadata(expected, expectedLabel) &&
           LabelMetadataEquivalent(observedLabel, expectedLabel);
}

bool MetadataMatchesAfterRead(const WindowsFileMetadataSnapshot& observed,
                              const WindowsFileMetadataSnapshot& expected) {
    return MetadataEquivalent(observed, expected, true);
}

bool MetadataMatchesAfterPublication(
    const WindowsFileMetadataSnapshot& observed,
    const WindowsFileMetadataSnapshot& expected) {
    // A Windows atomic replace may normalize ChangeTime even though the
    // published bytes, creation/write times, attributes, owner/group/DACL,
    // and mandatory label remain unchanged. LastAccess is already treated as
    // observational. Bind the journal to the captured published metadata so
    // recovery validates the exact state that actually reached the namespace.
    return MetadataEquivalent(observed, expected, true, true);
}

bool BasicMetadataMatchesAfterWindowsMutation(
    const WindowsFileMetadataSnapshot& observed,
    const WindowsFileMetadataSnapshot& expected) {
    FILE_BASIC_INFO observedBasic{};
    FILE_BASIC_INFO expectedBasic{};
    return DecodeBasicMetadata(observed, observedBasic) &&
           DecodeBasicMetadata(expected, expectedBasic) &&
           observedBasic.CreationTime.QuadPart ==
               expectedBasic.CreationTime.QuadPart &&
           observedBasic.LastWriteTime.QuadPart ==
               expectedBasic.LastWriteTime.QuadPart &&
           observedBasic.FileAttributes == expectedBasic.FileAttributes;
}

// A newly-created file is allowed to receive the destination parent's normal
// inherited/default security descriptor. Content publication may legitimately
// change basic timestamps and attributes, so the pre-publication inheritance
// proof compares only the security descriptor and mandatory label.
bool SecurityMetadataMatches(
    const WindowsFileMetadataSnapshot& observed,
    const WindowsFileMetadataSnapshot& expected) {
    PSECURITY_DESCRIPTOR rawObservedSecurity = nullptr;
    PSECURITY_DESCRIPTOR rawExpectedSecurity = nullptr;
    if (!DecodeSecurityMetadata(observed, rawObservedSecurity) ||
        !DecodeSecurityMetadata(expected, rawExpectedSecurity)) {
        return false;
    }
    PSECURITY_DESCRIPTOR observedLabel = nullptr;
    PSECURITY_DESCRIPTOR expectedLabel = nullptr;
    const bool decodedLabels = DecodeLabelMetadata(observed, observedLabel) &&
        DecodeLabelMetadata(expected, expectedLabel);
    const bool equivalent = decodedLabels &&
        SecurityMetadataEquivalent(
            rawObservedSecurity, rawExpectedSecurity) &&
        LabelMetadataEquivalent(observedLabel, expectedLabel);
    // Both decoders return pointers into the owned byte vectors. LocalFree is
    // reserved for GetSecurityInfo/GetNamedSecurityInfo allocations.
    return equivalent;
}

WindowsFilesystemBackendResult ApplyMetadata(
    HANDLE file,
    const WindowsFileMetadataSnapshot& metadata,
    const WindowsBootstrapFilesystemNativeTestHooks* testHooks,
    const WindowsFilesystemRootRole rootRole,
    const std::string& relativePath) {
    FILE_BASIC_INFO basic{};
    if (!DecodeBasicMetadata(metadata, basic)) return UnsupportedMetadata();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!DecodeSecurityMetadata(metadata, descriptor)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    PSECURITY_DESCRIPTOR expectedLabel = nullptr;
    if (!DecodeLabelMetadata(metadata, expectedLabel)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    BOOL ownerDefaulted = FALSE;
    BOOL groupDefaulted = FALSE;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) ||
        !GetSecurityDescriptorGroup(descriptor, &group, &groupDefaulted) ||
        !GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl,
                                    &daclDefaulted)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    PSECURITY_DESCRIPTOR rawCurrentDescriptor = nullptr;
    const DWORD currentSecurityResult = GetSecurityInfo(
        file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &rawCurrentDescriptor);
    ScopedLocalFree currentDescriptorOwner(rawCurrentDescriptor);
    PSECURITY_DESCRIPTOR currentDescriptor =
        static_cast<PSECURITY_DESCRIPTOR>(currentDescriptorOwner.get());
    if (currentSecurityResult != ERROR_SUCCESS || currentDescriptor == nullptr) {
        return Win32Failure(currentSecurityResult == ERROR_SUCCESS
                                ? ERROR_INVALID_SECURITY_DESCR
                                : currentSecurityResult);
    }
    PSID currentOwner = nullptr;
    PSID currentGroup = nullptr;
    BOOL currentOwnerDefaulted = FALSE;
    BOOL currentGroupDefaulted = FALSE;
    const bool currentSecurityDecoded =
        GetSecurityDescriptorOwner(currentDescriptor, &currentOwner,
                                    &currentOwnerDefaulted) != FALSE &&
        GetSecurityDescriptorGroup(currentDescriptor, &currentGroup,
                                    &currentGroupDefaulted) != FALSE;
    const bool ownerMatches = currentSecurityDecoded &&
        ((owner == nullptr && currentOwner == nullptr) ||
         (owner != nullptr && currentOwner != nullptr &&
          EqualSid(owner, currentOwner) != FALSE));
    const bool groupMatches = currentSecurityDecoded &&
        ((group == nullptr && currentGroup == nullptr) ||
         (group != nullptr && currentGroup != nullptr &&
          EqualSid(group, currentGroup) != FALSE));
    if (!currentSecurityDecoded) {
        return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
    }
    DWORD securityResult = ERROR_SUCCESS;
    if (ownerMatches && groupMatches && (control & SE_DACL_PROTECTED) != 0) {
        SECURITY_INFORMATION securityInformation =
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
        securityResult = SetSecurityInfo(
            file, SE_FILE_OBJECT, securityInformation,
            nullptr, nullptr, daclPresent ? dacl : nullptr, nullptr);
    } else if (ownerMatches && groupMatches) {
        securityResult = SetKernelObjectSecurity(
            file, DACL_SECURITY_INFORMATION, descriptor)
            ? ERROR_SUCCESS
            : GetLastError();
    } else {
        SECURITY_INFORMATION securityInformation =
            DACL_SECURITY_INFORMATION;
        if (!ownerMatches) securityInformation |= OWNER_SECURITY_INFORMATION;
        if (!groupMatches) securityInformation |= GROUP_SECURITY_INFORMATION;
        securityInformation |= (control & SE_DACL_PROTECTED) != 0
            ? PROTECTED_DACL_SECURITY_INFORMATION
            : UNPROTECTED_DACL_SECURITY_INFORMATION;
        securityResult = SetSecurityInfo(
            file, SE_FILE_OBJECT, securityInformation,
            ownerMatches ? nullptr : owner,
            groupMatches ? nullptr : group,
            daclPresent ? dacl : nullptr, nullptr);
    }
    if (securityResult != ERROR_SUCCESS) {
        return MetadataApplyFailure(securityResult);
    }
    PSECURITY_DESCRIPTOR rawCurrentLabel = nullptr;
    const DWORD currentLabelResult = GetSecurityInfo(
        file, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &rawCurrentLabel);
    ScopedLocalFree currentLabelOwner(rawCurrentLabel);
    PSECURITY_DESCRIPTOR currentLabel =
        static_cast<PSECURITY_DESCRIPTOR>(currentLabelOwner.get());
    if (currentLabelResult != ERROR_SUCCESS || currentLabel == nullptr) {
        return Win32Failure(currentLabelResult == ERROR_SUCCESS
                                ? ERROR_INVALID_SECURITY_DESCR
                                : currentLabelResult);
    }
    if (!LabelMetadataEquivalent(currentLabel, expectedLabel)) {
        BOOL expectedSaclPresent = FALSE;
        BOOL expectedSaclDefaulted = FALSE;
        PACL expectedSacl = nullptr;
        if (GetSecurityDescriptorSacl(expectedLabel, &expectedSaclPresent,
                                      &expectedSacl,
                                      &expectedSaclDefaulted) == FALSE) {
            return UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
        }
        const DWORD labelSetResult = SetSecurityInfo(
            file, SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr,
            expectedSaclPresent ? expectedSacl : nullptr);
        if (labelSetResult != ERROR_SUCCESS) {
            return MetadataApplyFailure(labelSetResult);
        }
    }
    if (!SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic))) {
        return MetadataApplyFailure(GetLastError());
    }
    if (testHooks != nullptr && testHooks->afterMetadataApply != nullptr) {
        testHooks->afterMetadataApply(
            testHooks->context, rootRole, relativePath,
            reinterpret_cast<WindowsFilesystemHandle>(file));
    }
    WindowsFileMetadataSnapshot observed;
    const WindowsFilesystemBackendResult captured = CaptureMetadata(file, observed);
    if (!captured.ok()) return captured;
    if (!BasicMetadataMatchesAfterWindowsMutation(observed, metadata)) {
        return UnsupportedMetadata(MetadataDiagnosticError(0xE101U));
    }
    PSECURITY_DESCRIPTOR rawObservedSecurity = nullptr;
    PSECURITY_DESCRIPTOR rawExpectedSecurity = nullptr;
    if (!DecodeSecurityMetadata(observed, rawObservedSecurity) ||
        !DecodeSecurityMetadata(metadata, rawExpectedSecurity)) {
        return UnsupportedMetadata(MetadataDiagnosticError(0xE102U));
    }
    // DecodeSecurityMetadata returns borrowed pointers into the two owned
    // byte vectors.  Only descriptors returned by GetSecurityInfo are
    // LocalFree-owned; attempting to adopt these borrowed pointers would
    // pass non-LocalAlloc memory to LocalFree at scope exit.
    if (!SecurityMetadataEquivalent(rawObservedSecurity,
                                    rawExpectedSecurity)) {
        return UnsupportedMetadata(MetadataDiagnosticError(0xE103U));
    }
    PSECURITY_DESCRIPTOR observedLabel = nullptr;
    if (!DecodeLabelMetadata(observed, observedLabel) ||
        !LabelMetadataEquivalent(observedLabel, expectedLabel)) {
        return UnsupportedMetadata(MetadataDiagnosticError(0xE104U));
    }
    return {};
}

WindowsFilesystemBackendResult MarkForDeletion(HANDLE handle) {
    FILE_DISPOSITION_INFO_EX disposition{};
    disposition.Flags = FILE_DISPOSITION_FLAG_DELETE |
                        FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (!SetFileInformationByHandle(handle, FileDispositionInfoEx,
                                    &disposition, sizeof(disposition))) {
        return Win32Failure(GetLastError());
    }
    return {};
}

WindowsFilesystemBackendResult InjectDeleteFailure(
    const WindowsBootstrapFilesystemNativeTestHooks* hooks,
    bool* failureConsumed,
    const WindowsBootstrapFilesystemNativeTestFailurePoint point) {
    if (hooks == nullptr || failureConsumed == nullptr || *failureConsumed ||
        hooks->accessDeniedFailurePoint != point) {
        return {};
    }
    *failureConsumed = true;
    return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                          ERROR_ACCESS_DENIED);
}

WindowsFilesystemBackendResult DeleteOpenEntryDurable(
    ScopedHandle& entry,
    HANDLE containingDirectory,
    const std::wstring& leaf,
    const bool adapterOwnedCleanup,
    const bool directoryEntry = false,
    const WindowsBootstrapFilesystemNativeTestHooks* testHooks = nullptr,
    bool* failureConsumed = nullptr) {
    const WindowsFilesystemBackendResult marked = MarkForDeletion(entry.get());
    if (!marked.ok()) {
        entry.reset();
        // A failed disposition request has not published a namespace change.
        // Preserve a pre-mutation sharing denial as retryable; only other
        // adapter-owned failures become durability failures.  Wrapping
        // ERROR_ACCESS_DENIED here would make a running image look like an
        // ambiguous post-mark cut and would prevent the terminal successor
        // from retrying after the parent exits.
        if (marked.status == WindowsFilesystemBackendStatus::AccessDenied) {
            return marked;
        }
        return adapterOwnedCleanup
            ? BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                             marked.nativeError)
            : marked;
    }
    const WindowsFilesystemBackendResult afterMark = InjectDeleteFailure(
        testHooks, failureConsumed,
        directoryEntry
            ? WindowsBootstrapFilesystemNativeTestFailurePoint::
                  RemoveDirectoryAfterMarkBeforeRebind
            : WindowsBootstrapFilesystemNativeTestFailurePoint::
                  DeleteAfterMarkBeforeRebind);
    if (!afterMark.ok()) {
        entry.reset();
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              afterMark.nativeError);
    }
    entry.reset();
    NativeOpenResult rebound = OpenRelativeWide(
        containingDirectory, leaf, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        kFileOpen, 0);
    if (rebound.result.ok()) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              ERROR_ALREADY_EXISTS);
    }
    if (rebound.result.status != WindowsFilesystemBackendStatus::NotFound) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              rebound.result.nativeError);
    }
    const WindowsFilesystemBackendResult afterRebind = InjectDeleteFailure(
        testHooks, failureConsumed,
        directoryEntry
            ? WindowsBootstrapFilesystemNativeTestFailurePoint::
                  RemoveDirectoryAfterRebindBeforeDirectoryFlush
            : WindowsBootstrapFilesystemNativeTestFailurePoint::
                  DeleteAfterRebindBeforeDirectoryFlush);
    if (!afterRebind.ok()) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              afterRebind.nativeError);
    }
    const WindowsFilesystemBackendResult flushed =
        FlushDirectory(containingDirectory);
    if (!flushed.ok()) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              flushed.nativeError);
    }
    const WindowsFilesystemBackendResult afterFlush = InjectDeleteFailure(
        testHooks, failureConsumed,
        directoryEntry
            ? WindowsBootstrapFilesystemNativeTestFailurePoint::
                  RemoveDirectoryAfterDirectoryFlushBeforeFinalRebind
            : WindowsBootstrapFilesystemNativeTestFailurePoint::
                  DeleteAfterDirectoryFlushBeforeFinalRebind);
    if (!afterFlush.ok()) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              afterFlush.nativeError);
    }
    NativeOpenResult finalRebound = OpenRelativeWide(
        containingDirectory, leaf, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        kFileOpen, 0);
    if (finalRebound.result.ok()) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              ERROR_ALREADY_EXISTS);
    }
    if (finalRebound.result.status != WindowsFilesystemBackendStatus::NotFound) {
        return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                              finalRebound.result.nativeError);
    }
    return {};
}

WindowsFilesystemBackendResult CleanupCreatedEntry(
    ScopedHandle& entry,
    HANDLE containingDirectory,
    const std::wstring& leaf) {
    return DeleteOpenEntryDurable(entry, containingDirectory, leaf, true);
}

WindowsFilesystemBackendResult RenameOpenHandle(
    HANDLE handle,
    HANDLE targetParent,
    const std::wstring& targetLeaf,
    bool replaceExisting) {
    if (targetLeaf.empty() ||
        targetLeaf.size() >
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) /
                sizeof(wchar_t)) {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_INVALID_NAME);
    }
    const DWORD nameBytes =
        static_cast<DWORD>(targetLeaf.size() * sizeof(wchar_t));
    const std::size_t bufferSize = offsetof(FILE_RENAME_INFO, FileName) +
                                   static_cast<std::size_t>(nameBytes);
    if (bufferSize > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())) {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_FILENAME_EXCED_RANGE);
    }
    auto buffer = std::make_unique<unsigned char[]>(bufferSize);
    std::memset(buffer.get(), 0, bufferSize);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.get());
    rename->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS |
                    (replaceExisting ? FILE_RENAME_FLAG_REPLACE_IF_EXISTS : 0U);
    rename->RootDirectory = targetParent;
    rename->FileNameLength = nameBytes;
    std::memcpy(rename->FileName, targetLeaf.data(), nameBytes);
    const NativeFunctions& native = NativeApi();
    if (native.setInformationFile == nullptr) {
        return BackendFailure(WindowsFilesystemBackendStatus::IoFailure,
                              ERROR_PROC_NOT_FOUND);
    }
    IO_STATUS_BLOCK io{};
    const NTSTATUS status = native.setInformationFile(
        handle, &io, rename, static_cast<ULONG>(bufferSize),
        kFileRenameInformationEx);
    if (status < 0) return NtFailure(status);
    return {};
}

WindowsFilesystemBackendResult OpenObservedRelative(
    HANDLE root,
    const std::wstring& relativePath,
    ACCESS_MASK access,
    ULONG share,
    ULONG options,
    NativeOpenResult& opened,
    WindowsFilesystemObservation& observation) {
    opened = OpenRelativeWide(root, relativePath, access, share,
                              kFileOpen, options);
    if (!opened.result.ok()) return opened.result;
    return ObservationForHandle(opened.handle.get(), observation);
}

WindowsFilesystemBackendResult OpenExpectedRegularFile(
    HANDLE root,
    const std::wstring& relativePath,
    const WindowsFileIdentity& expectedIdentity,
    ACCESS_MASK access,
    ULONG share,
    NativeOpenResult& opened,
    const ULONG extraOptions = 0) {
    if (!expectedIdentity.valid) {
        return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                              ERROR_INVALID_DATA);
    }
    WindowsFilesystemObservation observation;
    const WindowsFilesystemBackendResult result = OpenObservedRelative(
        root, relativePath, access, share,
        kFileNonDirectoryFile | extraOptions,
        opened, observation);
    if (!result.ok()) return result;
    if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
        return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                              ERROR_REPARSE_TAG_INVALID);
    }
    if (observation.kind != WindowsFilesystemEntryKind::RegularFile) {
        return BackendFailure(WindowsFilesystemBackendStatus::NotRegularFile,
                              ERROR_INVALID_DATA);
    }
    if (observation.identity != expectedIdentity) {
        return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                              ERROR_INVALID_DATA);
    }
    return {};
}

WindowsFilesystemBackendResult CreateNewFile(
    HANDLE parent,
    const std::wstring& leaf,
    ScopedHandle& output) {
    NativeOpenResult created = OpenRelativeWide(
        parent, leaf,
        GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES |
            READ_CONTROL | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ, kFileCreate,
        kFileNonDirectoryFile | kFileWriteThrough);
    if (!created.result.ok()) return created.result;
    output = std::move(created.handle);
    return {};
}

WindowsFilesystemBackendResult MakePartialDurable(
    const WindowsFilesystemBackendResult& cause) noexcept {
    return BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                          cause.nativeError);
}

class NativeWindowsBootstrapFilesystemBackend final
    : public WindowsBootstrapFilesystemBackend {
public:
    explicit NativeWindowsBootstrapFilesystemBackend(
        const WindowsBootstrapFilesystemTrustedRootPolicy& trustedRoots,
        const WindowsBootstrapFilesystemNativeTestHooks& testHooks)
        : trustedRoots_(trustedRoots), testHooks_(testHooks) {}

    ~NativeWindowsBootstrapFilesystemBackend() override {
        for (const auto& item : handles_) {
            const HANDLE handle = reinterpret_cast<HANDLE>(item.first);
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    }

    bool MetadataMatchesForRecovery(
        const WindowsFileMetadataSnapshot& observed,
        const WindowsFileMetadataSnapshot& expected) const override {
        // Windows can update ChangeTime when the newly installed executable is
        // loaded or receives a system EA. Recovery still binds the same FileId,
        // bytes, creation/write times, attributes, owner, DACL and label.
        return MetadataEquivalent(observed, expected, true, true);
    }

    WindowsFilesystemBackendResult OpenRootExact(
        const std::string& path,
        WindowsFilesystemRootRole role,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputHandle = 0;
        outputIdentity = {};

        std::wstring normalized;
        const WindowsFilesystemBackendResult normalizedResult =
            NormalizeAbsoluteRoot(path, normalized);
        if (!normalizedResult.ok()) return normalizedResult;

        // A backend created without an authenticated policy may still open
        // roots for read-only diagnostics, but it must never hand out a
        // writable operation capability.  Begin/BeginRecovery call
        // ValidateTrustedWriteRoots first; that call binds these exact final
        // paths and identities for the lifetime of this backend.
        const bool authorizedPath = AuthorizedPathForRole(normalized, role);

        DWORD operationAccess = GENERIC_READ;
        if (authorizedPath &&
            (role == WindowsFilesystemRootRole::Install ||
             role == WindowsFilesystemRootRole::Staging ||
             role == WindowsFilesystemRootRole::Transaction)) {
            operationAccess |= GENERIC_WRITE;
        }

        PinnedRootOpen openedRoot;
        const WindowsFilesystemBackendResult opened =
            OpenPinnedAbsoluteRoot(normalized, operationAccess, openedRoot);
        if (!opened.ok()) return opened;
        const WindowsFilesystemObservation& observation = openedRoot.observation;
        const std::wstring& finalPath = openedRoot.finalPath;

        const bool identityAlreadyBound =
            role == WindowsFilesystemRootRole::Install
                ? authorizedInstallIdentity_.valid
                : role == WindowsFilesystemRootRole::Staging
                    ? authorizedStagingIdentity_.valid
                    : role == WindowsFilesystemRootRole::Transaction
                        ? authorizedTransactionIdentity_.valid
                        : false;
        if (authorizedPath && role != WindowsFilesystemRootRole::Backup &&
            identityAlreadyBound &&
            !AuthorizedIdentityForRole(role, observation.identity)) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_INVALID_DATA);
        }

        const std::wstring extendedRootPath = L"\\\\?\\" + normalized;
        ScopedHandle rootPin(CreateFileW(
            extendedRootPath.c_str(),
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!rootPin.valid()) return Win32Failure(GetLastError());
        WindowsFilesystemObservation rootPinObservation;
        const WindowsFilesystemBackendResult rootPinObserved =
            ObservationForHandle(rootPin.get(), rootPinObservation);
        if (!rootPinObserved.ok()) return rootPinObserved;
        std::wstring rootPinFinalPath;
        const WindowsFilesystemBackendResult rootPinFinalResult =
            FinalPathForHandle(rootPin.get(), rootPinFinalPath);
        if (!rootPinFinalResult.ok()) return rootPinFinalResult;
        if (rootPinObservation.kind != WindowsFilesystemEntryKind::Directory ||
            rootPinObservation.identity != observation.identity ||
            !EqualPathExact(rootPinFinalPath, finalPath)) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::ExactPathMismatch,
                ERROR_INVALID_DATA);
        }

        for (const auto& existing : handles_) {
            if ((!existing.second.finalPath.empty() &&
                 PathsOverlap(finalPath, existing.second.finalPath)) ||
                (existing.second.role == role &&
                 role != WindowsFilesystemRootRole::Backup)) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::ExactPathMismatch,
                    ERROR_INVALID_DATA);
            }
        }

        const WindowsFilesystemHandle opaque =
            reinterpret_cast<WindowsFilesystemHandle>(openedRoot.operation.get());
        if (role == WindowsFilesystemRootRole::Install) {
            if (boundInstallVolume_.has_value() &&
                *boundInstallVolume_ != observation.identity.volumeSerial) {
                return BackendFailure(WindowsFilesystemBackendStatus::CrossVolume,
                                      ERROR_NOT_SAME_DEVICE);
            }
            boundInstallVolume_ = observation.identity.volumeSerial;
        } else if (writeRootsAuthorized_ &&
                   (!boundInstallVolume_.has_value() ||
                    *boundInstallVolume_ != observation.identity.volumeSerial)) {
            return BackendFailure(WindowsFilesystemBackendStatus::CrossVolume,
                                  ERROR_NOT_SAME_DEVICE);
        }

        HandleBinding binding;
        binding.role = role;
        binding.identity = observation.identity;
        binding.finalPath = finalPath;
        binding.ancestorPins = std::move(openedRoot.ancestorPins);
        binding.rootPin = std::move(rootPin);
        handles_[opaque] = std::move(binding);
        if (writeRootsAuthorized_) {
            switch (role) {
                case WindowsFilesystemRootRole::Install:
                    authorizedInstallIdentity_ = observation.identity;
                    break;
                case WindowsFilesystemRootRole::Staging:
                    authorizedStagingIdentity_ = observation.identity;
                    break;
                case WindowsFilesystemRootRole::Transaction:
                    authorizedTransactionIdentity_ = observation.identity;
                    break;
                case WindowsFilesystemRootRole::Backup:
                    break;
            }
        }
        journalIdentities_.erase(opaque);
        journalActiveSlots_.erase(opaque);
        journalSlotIdentities_.erase(opaque);
        journalSlotContents_.erase(opaque);
        outputHandle = opaque;
        outputIdentity = observation.identity;
        static_cast<void>(openedRoot.operation.release());
        return {};
    }

    WindowsFilesystemBackendResult OpenOrCreateDirectoryNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        bool requireNew,
        WindowsFilesystemHandle& outputHandle,
        WindowsFileIdentity& outputIdentity) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputHandle = 0;
        outputIdentity = {};
        HANDLE nativeParent = nullptr;
        WindowsFilesystemRootRole parentRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(parent, nativeParent, &parentRole)) {
            return InvalidBoundHandle();
        }
        const WindowsFilesystemBackendResult parentValidated =
            RevalidateBoundRoot(parent);
        if (!parentValidated.ok()) return parentValidated;
        if (parentRole != WindowsFilesystemRootRole::Transaction &&
            parentRole != WindowsFilesystemRootRole::Install) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(parent, parentRole);
        if (!authorized.ok()) return authorized;
        if (parentRole == WindowsFilesystemRootRole::Transaction
                ? !HasJournalLeaseForRoot(parent) : journalLeases_.empty()) {
            return MissingJournalLease();
        }

        std::wstring widePath;
        if (!RelativePathToWide(relativePath, widePath)) return InvalidRelativePath();
        ParentAndLeaf location;
        const WindowsFilesystemBackendResult located =
            OpenContainingDirectory(nativeParent, widePath, location);
        if (!located.ok()) return located;

        const WindowsFilesystemBackendResult beforeCreate =
            FailureAtTestPoint(
                parentRole == WindowsFilesystemRootRole::Install
                    ? WindowsBootstrapFilesystemNativeTestFailurePoint::InstallDirectoryBeforeCreate
                    : WindowsBootstrapFilesystemNativeTestFailurePoint::BackupDirectoryBeforeCreate);
        if (!beforeCreate.ok()) return beforeCreate;

        NativeOpenResult opened = OpenRelativeWide(
            location.parent.get(), location.leaf,
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            requireNew ? kFileCreate : kFileOpen,
            kFileDirectoryFile | kFileWriteThrough,
            FILE_ATTRIBUTE_DIRECTORY);
        if (!opened.result.ok()) return opened.result;

        const bool created = opened.information != kFileOpened;
        const auto afterCreateFailure =
            [created](const WindowsFilesystemBackendResult& failure) {
                return created ? MakePartialDurable(failure) : failure;
            };
        if (created) {
            const WindowsFilesystemBackendResult afterCreateObservation =
                FailureAtTestPoint(
                    parentRole == WindowsFilesystemRootRole::Install
                        ? WindowsBootstrapFilesystemNativeTestFailurePoint::InstallDirectoryAfterCreateBeforeObservation
                        : WindowsBootstrapFilesystemNativeTestFailurePoint::BackupDirectoryAfterCreateBeforeObservation);
            if (!afterCreateObservation.ok()) {
                return MakePartialDurable(afterCreateObservation);
            }
        }

        WindowsFilesystemObservation observation;
        const WindowsFilesystemBackendResult observed =
            ObservationForHandle(opened.handle.get(), observation);
        if (!observed.ok()) return afterCreateFailure(observed);
        if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return afterCreateFailure(
                BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                               ERROR_REPARSE_TAG_INVALID));
        }
        if (observation.kind != WindowsFilesystemEntryKind::Directory) {
            return afterCreateFailure(
                BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                               ERROR_DIRECTORY));
        }

        // A reused install directory can have been created immediately before
        // an interruption. Complete both flushes before publishing its binding.
        if (created || parentRole == WindowsFilesystemRootRole::Install) {
            if (parentRole == WindowsFilesystemRootRole::Install) {
                const auto beforeFlush = FailureAtTestPoint(
                    WindowsBootstrapFilesystemNativeTestFailurePoint::InstallDirectoryBeforeFlush);
                if (!beforeFlush.ok()) return afterCreateFailure(beforeFlush);
            }
            const WindowsFilesystemBackendResult childFlushed =
                FlushDirectory(opened.handle.get());
            const WindowsFilesystemBackendResult parentFlushed =
                childFlushed.ok() ? FlushDirectory(location.parent.get())
                                  : childFlushed;
            if (!parentFlushed.ok()) {
                opened.handle.reset();
                return afterCreateFailure(parentFlushed);
            }
        }

        const WindowsFilesystemHandle opaque =
            reinterpret_cast<WindowsFilesystemHandle>(opened.handle.get());
        std::wstring finalPath;
        const WindowsFilesystemBackendResult finalPathResult =
            FinalPathForHandle(opened.handle.get(), finalPath);
        if (!finalPathResult.ok()) return afterCreateFailure(finalPathResult);
        NativeOpenResult rootPin = OpenRelativeWide(
            location.parent.get(), location.leaf,
            FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, kFileOpen,
            kFileDirectoryFile);
        if (!rootPin.result.ok()) return afterCreateFailure(rootPin.result);
        WindowsFilesystemObservation rootPinObservation;
        const WindowsFilesystemBackendResult rootPinObserved =
            ObservationForHandle(rootPin.handle.get(), rootPinObservation);
        if (!rootPinObserved.ok()) return afterCreateFailure(rootPinObserved);
        std::wstring rootPinFinalPath;
        const WindowsFilesystemBackendResult rootPinFinalResult =
            FinalPathForHandle(rootPin.handle.get(), rootPinFinalPath);
        if (!rootPinFinalResult.ok()) {
            return afterCreateFailure(rootPinFinalResult);
        }
        if (rootPinObservation.kind != WindowsFilesystemEntryKind::Directory ||
            rootPinObservation.identity != observation.identity ||
            !EqualPathExact(rootPinFinalPath, finalPath)) {
            return afterCreateFailure(
                BackendFailure(WindowsFilesystemBackendStatus::ExactPathMismatch,
                               ERROR_INVALID_DATA));
        }
        HandleBinding binding;
        binding.role = parentRole == WindowsFilesystemRootRole::Install
            ? WindowsFilesystemRootRole::Install : WindowsFilesystemRootRole::Backup;
        binding.identity = observation.identity;
        binding.finalPath = std::move(finalPath);
        binding.rootPin = std::move(rootPin.handle);
        handles_[opaque] = std::move(binding);
        outputHandle = opaque;
        outputIdentity = observation.identity;
        static_cast<void>(opened.handle.release());
        return {};
    }

    WindowsFilesystemBackendResult ObserveNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        WindowsFilesystemObservation& output) override {
        std::lock_guard<std::mutex> lock(mutex_);
        output = {};
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole rootRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &rootRole)) {
            return InvalidBoundHandle();
        }

        std::wstring widePath;
        if (!RelativePathToWide(relativePath, widePath)) return InvalidRelativePath();
        NativeOpenResult opened;
        WindowsFilesystemObservation observation;
        const ACCESS_MASK observeAccess = rootRole ==
                WindowsFilesystemRootRole::Staging
            ? FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA
            : GENERIC_READ | READ_CONTROL;
        const WindowsFilesystemBackendResult observed = OpenObservedRelative(
            nativeRoot, widePath, observeAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
            opened, observation);
        if (observed.status == WindowsFilesystemBackendStatus::NotFound) return {};
        if (observed.status == WindowsFilesystemBackendStatus::ReparsePoint) {
            output.kind = WindowsFilesystemEntryKind::ReparsePoint;
            return {};
        }
        if (!observed.ok()) return observed;
        output = observation;
        if (output.kind == WindowsFilesystemEntryKind::RegularFile) {
            const WindowsFilesystemBackendResult captured =
                rootRole == WindowsFilesystemRootRole::Staging
                    ? ValidateSupportedMetadata(opened.handle.get())
                    : CaptureMetadata(opened.handle.get(), output.metadata);
            if (!captured.ok()) return captured;
            const WindowsFilesystemBackendResult fingerprinted =
                FingerprintHandle(opened.handle.get(), output.content);
            if (!fingerprinted.ok()) return fingerprinted;
        }
        return {};
    }

    WindowsFilesystemBackendResult CopyFileDurableNoFollow(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputFingerprint,
        WindowsFileMetadataSnapshot& outputMetadata) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputIdentity = {};
        outputFingerprint = {};
        outputMetadata = {};
        HANDLE nativeSourceRoot = nullptr;
        HANDLE nativeTargetRoot = nullptr;
        WindowsFilesystemRootRole sourceRole = WindowsFilesystemRootRole::Install;
        WindowsFilesystemRootRole targetRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(sourceRoot, nativeSourceRoot, &sourceRole) ||
            !BoundHandle(targetRoot, nativeTargetRoot, &targetRole)) {
            return InvalidBoundHandle();
        }
        const WindowsFilesystemBackendResult sourceRootValidated =
            RevalidateBoundRoot(sourceRoot);
        if (!sourceRootValidated.ok()) return sourceRootValidated;
        const WindowsFilesystemBackendResult targetRootValidated =
            RevalidateBoundRoot(targetRoot);
        if (!targetRootValidated.ok()) return targetRootValidated;
        if (sourceRole != WindowsFilesystemRootRole::Install ||
            targetRole != WindowsFilesystemRootRole::Backup) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult sourceAuthorized =
            RequireAuthorizedMutation(sourceRoot,
                                      WindowsFilesystemRootRole::Install);
        if (!sourceAuthorized.ok()) return sourceAuthorized;
        const WindowsFilesystemBackendResult targetAuthorized =
            RequireAuthorizedMutation(targetRoot,
                                      WindowsFilesystemRootRole::Backup);
        if (!targetAuthorized.ok()) return targetAuthorized;
        if (journalLeases_.empty()) return MissingJournalLease();

        std::wstring wideSource;
        std::wstring wideTarget;
        if (!RelativePathToWide(sourcePath, wideSource) ||
            !RelativePathToWide(targetPath, wideTarget)) {
            return InvalidRelativePath();
        }

        NativeOpenResult source;
        const WindowsFilesystemBackendResult sourceOpened = OpenExpectedRegularFile(
            nativeSourceRoot, wideSource, expectedSource,
            GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, source);
        if (!sourceOpened.ok()) return sourceOpened;

        WindowsFileMetadataSnapshot sourceMetadata;
        const WindowsFilesystemBackendResult sourceMetadataCaptured =
            CaptureMetadata(source.handle.get(), sourceMetadata);
        if (!sourceMetadataCaptured.ok()) return sourceMetadataCaptured;

        WindowsFileContentFingerprint sourceFingerprint;
        const WindowsFilesystemBackendResult sourceFingerprinted =
            FingerprintHandle(source.handle.get(), sourceFingerprint);
        if (!sourceFingerprinted.ok()) return sourceFingerprinted;

        ParentAndLeaf target;
        const WindowsFilesystemBackendResult targetLocated =
            OpenContainingDirectory(nativeTargetRoot, wideTarget, target);
        if (!targetLocated.ok()) return targetLocated;

        NativeOpenResult observedTarget;
        WindowsFilesystemObservation targetObservation;
        const WindowsFilesystemBackendResult targetObserved = OpenObservedRelative(
            target.parent.get(), target.leaf, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            0, observedTarget, targetObservation);
        if (targetObserved.ok()) {
            return targetObservation.kind == WindowsFilesystemEntryKind::ReparsePoint
                ? BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                 ERROR_REPARSE_TAG_INVALID)
                : BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                 ERROR_FILE_EXISTS);
        }
        if (targetObserved.status != WindowsFilesystemBackendStatus::NotFound) {
            return targetObserved;
        }

        std::wstring backupTemporaryLeaf = target.leaf + L".tmp";
        if (!IsSafeRelativeComponent(backupTemporaryLeaf) ||
            EqualPathInsensitive(backupTemporaryLeaf, target.leaf)) {
            return InvalidRelativePath();
        }
        ScopedHandle created;
        const WindowsFilesystemBackendResult targetCreated =
            CreateNewFile(target.parent.get(), backupTemporaryLeaf, created);
        if (!targetCreated.ok()) return targetCreated;

        WindowsFilesystemBackendResult copied =
            CopyHandleContents(source.handle.get(), created.get());
        if (copied.ok() && !FlushFileBuffers(created.get())) {
            copied = BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                                    GetLastError());
        }

        WindowsFileContentFingerprint targetFingerprint;
        if (copied.ok()) {
            copied = FingerprintHandle(created.get(), targetFingerprint);
            if (copied.ok() && !ContentMatches(sourceFingerprint, targetFingerprint)) {
                copied = ContentMismatch();
            }
        }

        WindowsFileContentFingerprint sourceAfterFingerprint;
        WindowsFileMetadataSnapshot sourceAfterMetadata;
        if (copied.ok()) {
            copied = FingerprintHandle(source.handle.get(),
                                       sourceAfterFingerprint);
            if (copied.ok() &&
                !ContentMatches(sourceFingerprint, sourceAfterFingerprint)) {
                copied = ContentMismatch();
            }
        }
        if (copied.ok()) {
            copied = CaptureMetadata(source.handle.get(), sourceAfterMetadata);
            if (copied.ok() &&
                !MetadataMatchesAfterRead(sourceAfterMetadata, sourceMetadata)) {
                copied = UnsupportedMetadata(ERROR_INVALID_DATA);
            }
        }

        WindowsFileIdentity identity;
        if (copied.ok()) copied = IdentityForHandle(created.get(), identity);
        WindowsFileMetadataSnapshot createdMetadata;
        if (copied.ok()) {
            copied = CaptureMetadata(created.get(), createdMetadata);
            if (copied.ok() && !createdMetadata.valid) {
                copied = UnsupportedMetadata(ERROR_INVALID_SECURITY_DESCR);
            }
        }
        if (!copied.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(created, target.parent.get(),
                                    backupTemporaryLeaf);
            return cleaned.ok() ? copied : cleaned;
        }

        const WindowsFilesystemBackendResult renamed = RenameOpenHandle(
            created.get(), target.parent.get(), target.leaf, false);
        if (!renamed.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(created, target.parent.get(),
                                    backupTemporaryLeaf);
            return cleaned.ok() ? renamed : cleaned;
        }
        const WindowsFilesystemBackendResult afterBackupPublish =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    BackupAfterPublishBeforeRebind);
        if (!afterBackupPublish.ok()) {
            return MakePartialDurable(afterBackupPublish);
        }
        NativeOpenResult published;
        const WindowsFilesystemBackendResult rebound = OpenExpectedRegularFile(
            target.parent.get(), target.leaf, identity,
            FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            published);
        if (!rebound.ok()) return MakePartialDurable(rebound);
        WindowsFileMetadataSnapshot publishedMetadata;
        const WindowsFilesystemBackendResult publishedCaptured =
            CaptureMetadata(published.handle.get(), publishedMetadata);
        if (!publishedCaptured.ok()) {
            return MakePartialDurable(
                publishedCaptured);
        }
        const WindowsFilesystemBackendResult directoryFlushed =
            FlushDirectory(target.parent.get());
        if (!directoryFlushed.ok()) return MakePartialDurable(directoryFlushed);
        outputIdentity = identity;
        outputFingerprint = sourceFingerprint;
        outputMetadata = std::move(sourceMetadata);
        return {};
    }

    WindowsFilesystemBackendResult ReplaceFileDurableNoFollow(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        const WindowsFileContentFingerprint& expectedSourceContent,
        const WindowsFileMetadataSnapshot& expectedSourceMetadata,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        const WindowsFileIdentity& expectedTargetParent,
        const std::string& temporaryLeaf,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedTarget,
        const WindowsFileContentFingerprint& expectedTargetContent,
        const WindowsFileMetadataSnapshot& expectedTargetMetadata,
        WindowsFileIdentity& outputIdentity,
        WindowsFileMetadataSnapshot& outputMetadata) override {
        return ReplaceFileDurableNoFollowWithCheckpoint(
            sourceRoot, sourcePath, expectedSource, expectedSourceContent,
            expectedSourceMetadata, targetRoot, targetPath,
            expectedTargetParent, temporaryLeaf, mode, expectedTarget,
            expectedTargetContent, expectedTargetMetadata, {}, {}, {}, nullptr,
            outputIdentity, outputMetadata);
    }

    WindowsFilesystemBackendResult ReplaceFileDurableNoFollowWithCheckpoint(
        WindowsFilesystemHandle sourceRoot,
        const std::string& sourcePath,
        const WindowsFileIdentity& expectedSource,
        const WindowsFileContentFingerprint& expectedSourceContent,
        const WindowsFileMetadataSnapshot& expectedSourceMetadata,
        WindowsFilesystemHandle targetRoot,
        const std::string& targetPath,
        const WindowsFileIdentity& expectedTargetParent,
        const std::string& temporaryLeaf,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedTarget,
        const WindowsFileContentFingerprint& expectedTargetContent,
        const WindowsFileMetadataSnapshot& expectedTargetMetadata,
        const WindowsFileIdentity& expectedPreparedIdentity,
        const WindowsFileContentFingerprint& expectedPreparedContent,
        const WindowsFileMetadataSnapshot& expectedPreparedMetadata,
        const WindowsFilesystemPublicationCheckpoint* checkpoint,
        WindowsFileIdentity& outputIdentity,
        WindowsFileMetadataSnapshot& outputMetadata) override {
        std::unique_lock<std::mutex> lock(mutex_);
        outputIdentity = {};
        outputMetadata = {};
        HANDLE nativeSourceRoot = nullptr;
        HANDLE nativeTargetRoot = nullptr;
        WindowsFilesystemRootRole sourceRole = WindowsFilesystemRootRole::Install;
        WindowsFilesystemRootRole targetRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(sourceRoot, nativeSourceRoot, &sourceRole) ||
            !BoundHandle(targetRoot, nativeTargetRoot, &targetRole)) {
            return InvalidBoundHandle();
        }
        const WindowsFilesystemBackendResult sourceRootValidated =
            RevalidateBoundRoot(sourceRoot);
        if (!sourceRootValidated.ok()) return sourceRootValidated;
        const WindowsFilesystemBackendResult targetRootValidated =
            RevalidateBoundRoot(targetRoot);
        if (!targetRootValidated.ok()) return targetRootValidated;
        if ((sourceRole != WindowsFilesystemRootRole::Staging &&
             sourceRole != WindowsFilesystemRootRole::Backup) ||
            targetRole != WindowsFilesystemRootRole::Install) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult targetAuthorized =
            RequireAuthorizedMutation(targetRoot,
                                      WindowsFilesystemRootRole::Install);
        if (!targetAuthorized.ok()) return targetAuthorized;
        const WindowsFilesystemBackendResult sourceAuthorized =
            sourceRole == WindowsFilesystemRootRole::Staging
                ? RequireAuthorizedMutation(
                      sourceRoot, WindowsFilesystemRootRole::Staging)
                : RequireAuthorizedMutation(
                      sourceRoot, WindowsFilesystemRootRole::Backup);
        if (!sourceAuthorized.ok()) return sourceAuthorized;
        if ((sourceRole == WindowsFilesystemRootRole::Backup) !=
            expectedSourceMetadata.valid) {
            return UnsupportedMetadata();
        }
        if (expectedSourceMetadata.valid) {
            const WindowsFilesystemBackendResult sourceMetadataShape =
                ValidateMetadataSnapshotStructure(expectedSourceMetadata);
            if (!sourceMetadataShape.ok()) return sourceMetadataShape;
        }
        const bool inheritNewTargetMetadata =
            mode == WindowsFilesystemReplaceMode::CreateNew &&
            sourceRole != WindowsFilesystemRootRole::Backup;
        if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            const WindowsFilesystemBackendResult targetMetadataShape =
                ValidateMetadataSnapshotStructure(expectedTargetMetadata);
            if (!targetMetadataShape.ok()) return targetMetadataShape;
        }
        // A prepared publication is a restart capability.  Validate every
        // supplied value before opening the source, target, or temporary leaf;
        // malformed journal metadata must therefore be a pure preflight
        // failure rather than an I/O-dependent mismatch.
        if (expectedPreparedIdentity.valid != expectedPreparedContent.valid ||
            expectedPreparedIdentity.valid != expectedPreparedMetadata.valid) {
            return UnsupportedMetadata(ERROR_INVALID_DATA);
        }
        if (expectedPreparedMetadata.valid) {
            const WindowsFilesystemBackendResult preparedMetadataShape =
                ValidateMetadataSnapshotStructure(expectedPreparedMetadata);
            if (!preparedMetadataShape.ok()) return preparedMetadataShape;
        }
        if (journalLeases_.empty()) return MissingJournalLease();

        std::wstring wideSource;
        std::wstring wideTarget;
        std::wstring wideTemporaryLeaf;
        if (!RelativePathToWide(sourcePath, wideSource) ||
            !RelativePathToWide(targetPath, wideTarget) ||
            !RelativePathToWide(temporaryLeaf, wideTemporaryLeaf) ||
            wideTemporaryLeaf.find(L'\\') != std::wstring::npos) {
            return InvalidRelativePath();
        }

        NativeOpenResult source;
        const WindowsFilesystemBackendResult sourceOpened = OpenExpectedRegularFile(
            nativeSourceRoot, wideSource, expectedSource,
            FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA,
            FILE_SHARE_READ | FILE_SHARE_DELETE, source);
        if (!sourceOpened.ok()) return sourceOpened;

        // Staging/backup security descriptors are not package authority. We
        // still reject hard links, ADS, EAs, and unsupported attributes, but
        // content reads do not require READ_CONTROL on the source object.
        const WindowsFilesystemBackendResult sourceMetadataCaptured =
            ValidateSupportedMetadata(source.handle.get());
        if (!sourceMetadataCaptured.ok()) return sourceMetadataCaptured;

        WindowsFileContentFingerprint sourceFingerprint;
        const WindowsFilesystemBackendResult sourceFingerprinted =
            FingerprintHandle(source.handle.get(), sourceFingerprint);
        if (!sourceFingerprinted.ok()) return sourceFingerprinted;
        if (!ContentMatches(sourceFingerprint, expectedSourceContent)) {
            return ContentMismatch();
        }

        ParentAndLeaf target;
        const WindowsFilesystemBackendResult targetLocated =
            OpenContainingDirectory(nativeTargetRoot, wideTarget, target);
        if (!targetLocated.ok()) return targetLocated;
        WindowsFileIdentity targetParentIdentity;
        const WindowsFilesystemBackendResult targetParentBound =
            IdentityForHandle(target.parent.get(), targetParentIdentity);
        if (!targetParentBound.ok()) return targetParentBound;
        if (!expectedTargetParent.valid ||
            targetParentIdentity != expectedTargetParent) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::IdentityMismatch,
                ERROR_INVALID_DATA);
        }
        WindowsFileMetadataSnapshot parentSecurityMetadata;
        if (inheritNewTargetMetadata) {
            // The temp is opened with a null security descriptor, so Windows
            // applies the destination parent's normal inheritance/defaults.
            // Capture the parent before any test seam or mutation and require
            // it to remain stable through publication; direct parent/child
            // ACL byte equality is intentionally not required because
            // inherited ACE flags are transformed on the child.
            const WindowsFilesystemBackendResult parentSecurityCaptured =
                CaptureSecurityMetadata(target.parent.get(),
                                        parentSecurityMetadata);
            if (!parentSecurityCaptured.ok()) return parentSecurityCaptured;
        }
        if (EqualPathInsensitive(wideTemporaryLeaf, target.leaf)) {
            return InvalidRelativePath();
        }

        if (testHooks_.beforeTargetMutation != nullptr) {
            testHooks_.beforeTargetMutation(testHooks_.context, targetRole,
                                            targetPath);
        }

        NativeOpenResult existingTarget;
        if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            const WindowsFilesystemBackendResult targetOpened =
                OpenExpectedRegularFile(
                    target.parent.get(), target.leaf, expectedTarget,
                    GENERIC_READ | READ_CONTROL | DELETE |
                        FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                    existingTarget);
            if (!targetOpened.ok()) return targetOpened;
            WindowsFileMetadataSnapshot targetMetadata;
            const WindowsFilesystemBackendResult targetMetadataCaptured =
                CaptureMetadata(existingTarget.handle.get(), targetMetadata);
            if (!targetMetadataCaptured.ok()) return targetMetadataCaptured;
            if (!expectedTargetMetadata.valid ||
                !MetadataMatchesAfterRead(targetMetadata,
                                          expectedTargetMetadata)) {
                return UnsupportedMetadata();
            }
            WindowsFileContentFingerprint targetFingerprint;
            const WindowsFilesystemBackendResult targetFingerprinted =
                FingerprintHandle(existingTarget.handle.get(), targetFingerprint);
            if (!targetFingerprinted.ok()) return targetFingerprinted;
            if (!ContentMatches(targetFingerprint, expectedTargetContent)) {
                return ContentMismatch();
            }
        } else if (mode == WindowsFilesystemReplaceMode::CreateNew) {
            if (expectedTarget.valid || expectedTargetContent.valid ||
                expectedTargetMetadata.valid) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_INVALID_DATA);
            }
            NativeOpenResult observed;
            WindowsFilesystemObservation observation;
            const WindowsFilesystemBackendResult targetObserved = OpenObservedRelative(
                target.parent.get(), target.leaf, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                0, observed, observation);
            if (targetObserved.ok()) {
                return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                      ERROR_FILE_EXISTS);
            }
            if (targetObserved.status != WindowsFilesystemBackendStatus::NotFound) {
                return targetObserved;
            }
        } else {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_PARAMETER);
        }

        const WindowsFileMetadataSnapshot desiredMetadata =
            sourceRole == WindowsFilesystemRootRole::Backup
                ? expectedSourceMetadata
                : mode == WindowsFilesystemReplaceMode::ReplaceExisting
                    ? expectedTargetMetadata
                    : WindowsFileMetadataSnapshot{};

        const bool prepared = expectedPreparedIdentity.valid;
        ScopedHandle temporary;
        WindowsFileMetadataSnapshot inheritedMetadata;
        WindowsFileContentFingerprint targetFingerprint;
        WindowsFileIdentity newIdentity = expectedPreparedIdentity;
        WindowsFilesystemBackendResult copied;
        if (prepared) {
            NativeOpenResult preparedOpen;
            copied = OpenExpectedRegularFile(
                target.parent.get(), wideTemporaryLeaf,
                expectedPreparedIdentity,
                GENERIC_READ | READ_CONTROL | DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                preparedOpen);
            if (copied.ok()) temporary = std::move(preparedOpen.handle);
            if (copied.ok()) {
                copied = FingerprintHandle(temporary.get(), targetFingerprint);
            }
            if (copied.ok() &&
                !ContentMatches(targetFingerprint, expectedPreparedContent)) {
                copied = ContentMismatch();
            }
            WindowsFileMetadataSnapshot preparedMetadata;
            if (copied.ok()) {
                copied = CaptureMetadata(temporary.get(), preparedMetadata);
            }
            if (copied.ok() &&
                !MetadataMatchesAfterRead(preparedMetadata,
                                           expectedPreparedMetadata)) {
                copied = UnsupportedMetadata(ERROR_INVALID_DATA);
            }
            if (copied.ok() && inheritNewTargetMetadata) {
                inheritedMetadata = expectedPreparedMetadata;
            }
            if (!copied.ok()) return copied;
        } else {
            copied = CreateNewFile(target.parent.get(), wideTemporaryLeaf,
                                   temporary);
            if (!copied.ok()) return copied;

            if (inheritNewTargetMetadata) {
                const WindowsFilesystemBackendResult inheritedCaptured =
                    CaptureMetadata(temporary.get(), inheritedMetadata);
                if (!inheritedCaptured.ok()) {
                    const WindowsFilesystemBackendResult cleaned =
                        CleanupCreatedEntry(temporary, target.parent.get(),
                                            wideTemporaryLeaf);
                    return cleaned.ok() ? inheritedCaptured : cleaned;
                }
            }

            copied = CopyHandleContents(source.handle.get(), temporary.get());
            if (copied.ok() && !FlushFileBuffers(temporary.get())) {
                copied = BackendFailure(
                    WindowsFilesystemBackendStatus::DurabilityFailed,
                    GetLastError());
            }
            if (copied.ok()) {
                copied = FingerprintHandle(temporary.get(), targetFingerprint);
                if (copied.ok() &&
                    !ContentMatches(expectedSourceContent, targetFingerprint)) {
                    copied = ContentMismatch();
                }
            }
            WindowsFileContentFingerprint sourceAfterFingerprint;
            if (copied.ok()) {
                copied = FingerprintHandle(source.handle.get(),
                                           sourceAfterFingerprint);
                if (copied.ok() &&
                    !ContentMatches(expectedSourceContent,
                                    sourceAfterFingerprint)) {
                    copied = ContentMismatch();
                }
            }
            if (copied.ok()) {
                copied = ValidateSupportedMetadata(source.handle.get());
            }
            if (copied.ok()) copied = IdentityForHandle(temporary.get(), newIdentity);
            if (copied.ok() && desiredMetadata.valid) {
                copied = ApplyMetadata(temporary.get(), desiredMetadata,
                                       &testHooks_, targetRole, targetPath);
            }
            if (copied.ok() && !FlushFileBuffers(temporary.get())) {
                copied = BackendFailure(
                    WindowsFilesystemBackendStatus::DurabilityFailed,
                    GetLastError());
            }
            if (!copied.ok()) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                return cleaned.ok() ? copied : cleaned;
            }
        }

        if (testHooks_.beforeTemporaryPublish != nullptr) {
            testHooks_.beforeTemporaryPublish(
                testHooks_.context, targetRole, targetPath, temporaryLeaf,
                reinterpret_cast<WindowsFilesystemHandle>(temporary.get()));
        }

        WindowsFileMetadataSnapshot currentParentSecurity;
        const WindowsFilesystemBackendResult parentSecurityRechecked =
            inheritNewTargetMetadata
                ? CaptureSecurityMetadata(target.parent.get(),
                                          currentParentSecurity)
                : WindowsFilesystemBackendResult{};
        WindowsFileMetadataSnapshot prePublishMetadata;
        const WindowsFilesystemBackendResult prePublishCaptured =
            CaptureMetadata(temporary.get(), prePublishMetadata);
        const bool parentSecurityMatches =
            !inheritNewTargetMetadata ||
            (parentSecurityRechecked.ok() &&
             SecurityMetadataMatches(currentParentSecurity,
                                     parentSecurityMetadata));
        const bool prePublishMetadataMatches = parentSecurityMatches &&
            prePublishCaptured.ok() &&
             (inheritNewTargetMetadata
                  ? SecurityMetadataMatches(prePublishMetadata,
                                             inheritedMetadata)
                  : MetadataMatchesAfterPublication(prePublishMetadata,
                                                    desiredMetadata));
        if (!prePublishMetadataMatches) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, target.parent.get(),
                                    wideTemporaryLeaf);
            const WindowsFilesystemBackendResult failure =
                !parentSecurityRechecked.ok()
                    ? parentSecurityRechecked
                    : prePublishCaptured.ok()
                    ? UnsupportedMetadata(MetadataDiagnosticError(0xE105U))
                    : prePublishCaptured;
            return cleaned.ok() ? failure : cleaned;
        }

        const WindowsFilesystemBackendResult temporaryEntryFlushed =
            FlushDirectory(target.parent.get());
        if (!temporaryEntryFlushed.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, target.parent.get(),
                                    wideTemporaryLeaf);
            return cleaned.ok() ? temporaryEntryFlushed : cleaned;
        }

        if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            WindowsFileContentFingerprint currentContent;
            const WindowsFilesystemBackendResult refingerprinted =
                FingerprintHandle(existingTarget.handle.get(), currentContent);
            if (!refingerprinted.ok() ||
                !ContentMatches(currentContent, expectedTargetContent)) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                if (!cleaned.ok()) return cleaned;
                return refingerprinted.ok() ? ContentMismatch() : refingerprinted;
            }
            WindowsFileMetadataSnapshot currentTargetMetadata;
            const WindowsFilesystemBackendResult targetMetadataRechecked =
                CaptureMetadata(existingTarget.handle.get(),
                                currentTargetMetadata);
            if (!targetMetadataRechecked.ok() ||
                !MetadataMatchesAfterRead(currentTargetMetadata,
                                          expectedTargetMetadata)) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                if (!cleaned.ok()) return cleaned;
                return targetMetadataRechecked.ok()
                    ? UnsupportedMetadata(ERROR_INVALID_DATA)
                    : targetMetadataRechecked;
            }
            NativeOpenResult nameBoundTarget;
            const WindowsFilesystemBackendResult nameRebound =
                OpenExpectedRegularFile(
                    target.parent.get(), target.leaf, expectedTarget,
                    FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nameBoundTarget);
            if (!nameRebound.ok()) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                return cleaned.ok()
                    ? BackendFailure(
                          WindowsFilesystemBackendStatus::IdentityMismatch,
                          nameRebound.nativeError)
                    : cleaned;
            }
        }

        const WindowsFilesystemBackendResult beforeAtomicPublish =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    ReplacementBeforeAtomicPublish);
        if (!beforeAtomicPublish.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, target.parent.get(),
                                    wideTemporaryLeaf);
            return cleaned.ok() ? beforeAtomicPublish : cleaned;
        }

        if (!prepared && checkpoint != nullptr &&
            checkpoint->onPrepared != nullptr) {
            // The port checkpoint persists through the backend's journal
            // primitive.  Release the native mutex around that nested call;
            // no native handle escapes and the trusted updater serializes its
            // own transaction operations.
            lock.unlock();
            const WindowsFilesystemBackendResult checkpointed =
                checkpoint->onPrepared(
                    checkpoint->context, newIdentity, targetFingerprint,
                    prePublishMetadata);
            lock.lock();
            if (!checkpointed.ok()) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                return cleaned.ok() ? checkpointed : cleaned;
            }
        }

        const WindowsFilesystemBackendResult afterPreparedCheckpoint =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    ReplacementAfterPreparedCheckpointBeforeAtomicPublish);
        if (!afterPreparedCheckpoint.ok()) {
            return MakePartialDurable(afterPreparedCheckpoint);
        }

        // The prepared checkpoint is an intentional cross-component handoff
        // and releases the backend mutex while the journal generation is
        // persisted.  Rebind the destination immediately after that handoff
        // so a same-name target replacement cannot be adopted by the final
        // atomic rename.  The containing-directory handle is already pinned,
        // but its identity is checked again as a cheap fail-closed guard.
        const auto cleanupBeforePublish =
            [&temporary, &target, &wideTemporaryLeaf](
                const WindowsFilesystemBackendResult failure) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, target.parent.get(),
                                        wideTemporaryLeaf);
                return cleaned.ok() ? failure : cleaned;
            };
        WindowsFileIdentity reboundTargetParentIdentity;
        const WindowsFilesystemBackendResult reboundTargetParent =
            IdentityForHandle(target.parent.get(), reboundTargetParentIdentity);
        if (!reboundTargetParent.ok()) {
            return cleanupBeforePublish(reboundTargetParent);
        }
        if (reboundTargetParentIdentity != expectedTargetParent) {
            return cleanupBeforePublish(
                BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                               ERROR_INVALID_DATA));
        }

        if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            NativeOpenResult reboundTarget;
            const WindowsFilesystemBackendResult targetReopened =
                OpenExpectedRegularFile(
                    target.parent.get(), target.leaf, expectedTarget,
                    GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_DELETE, reboundTarget);
            if (!targetReopened.ok()) {
                const WindowsFilesystemBackendResult failure =
                    targetReopened.status == WindowsFilesystemBackendStatus::NotFound
                        ? BackendFailure(
                              WindowsFilesystemBackendStatus::IdentityMismatch,
                              targetReopened.nativeError)
                        : targetReopened;
                return cleanupBeforePublish(failure);
            }
            WindowsFileContentFingerprint reboundTargetContent;
            const WindowsFilesystemBackendResult targetRefingerprinted =
                FingerprintHandle(reboundTarget.handle.get(),
                                  reboundTargetContent);
            if (!targetRefingerprinted.ok()) {
                return cleanupBeforePublish(targetRefingerprinted);
            }
            if (!ContentMatches(reboundTargetContent,
                                expectedTargetContent)) {
                return cleanupBeforePublish(ContentMismatch());
            }
            WindowsFileMetadataSnapshot reboundTargetMetadata;
            const WindowsFilesystemBackendResult targetMetadataRechecked =
                CaptureMetadata(reboundTarget.handle.get(),
                                reboundTargetMetadata);
            if (!targetMetadataRechecked.ok()) {
                return cleanupBeforePublish(targetMetadataRechecked);
            }
            if (!MetadataMatchesAfterRead(reboundTargetMetadata,
                                          expectedTargetMetadata)) {
                return cleanupBeforePublish(UnsupportedMetadata(ERROR_INVALID_DATA));
            }
        } else {
            NativeOpenResult reboundTarget;
            WindowsFilesystemObservation reboundObservation;
            const WindowsFilesystemBackendResult targetObserved =
                OpenObservedRelative(
                    target.parent.get(), target.leaf, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, reboundTarget, reboundObservation);
            if (targetObserved.ok()) {
                return cleanupBeforePublish(
                    BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                   ERROR_FILE_EXISTS));
            }
            if (targetObserved.status != WindowsFilesystemBackendStatus::NotFound) {
                return cleanupBeforePublish(targetObserved);
            }
        }

        const WindowsFilesystemBackendResult renamed = RenameOpenHandle(
            temporary.get(), target.parent.get(), target.leaf,
            mode == WindowsFilesystemReplaceMode::ReplaceExisting);
        if (!renamed.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, target.parent.get(),
                                    wideTemporaryLeaf);
            return cleaned.ok() ? renamed : cleaned;
        }
        if (testHooks_.afterAtomicPublish != nullptr) {
            testHooks_.afterAtomicPublish(
                testHooks_.context, targetRole, targetPath,
                reinterpret_cast<WindowsFilesystemHandle>(temporary.get()));
        }
        const WindowsFilesystemBackendResult afterAtomicPublish =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    ReplacementAfterAtomicPublishBeforeDirectoryFlush);
        if (!afterAtomicPublish.ok()) return MakePartialDurable(afterAtomicPublish);
        NativeOpenResult published;
        const WindowsFilesystemBackendResult rebound = OpenExpectedRegularFile(
            target.parent.get(), target.leaf, newIdentity,
            GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            published);
        if (!rebound.ok()) return MakePartialDurable(rebound);
        // The temporary already passed the destination-side metadata check
        // immediately before publication.  Applying the descriptor again
        // through the pre-publication handle is both redundant and unsafe:
        // a restart-resumed handle is intentionally opened read-only and may
        // not carry WRITE_DAC/WRITE_OWNER/FILE_WRITE_ATTRIBUTES.  Rebind the
        // published name and verify its metadata instead; any drift is a
        // durable-state failure below.
        WindowsFileMetadataSnapshot publishedMetadata;
        const WindowsFilesystemBackendResult publishedCaptured =
            CaptureMetadata(published.handle.get(), publishedMetadata);
        if (!publishedCaptured.ok() ||
            (desiredMetadata.valid &&
             !MetadataMatchesAfterPublication(publishedMetadata,
                                              desiredMetadata))) {
            return MakePartialDurable(publishedCaptured.ok()
                                          ? UnsupportedMetadata(ERROR_CRC)
                                          : publishedCaptured);
        }
        const WindowsFilesystemBackendResult directoryFlushed =
            FlushDirectory(target.parent.get());
        if (!directoryFlushed.ok()) return MakePartialDurable(directoryFlushed);
        outputIdentity = newIdentity;
        outputMetadata = std::move(publishedMetadata);
        return {};
    }

    WindowsFilesystemBackendResult DeleteFileDurableNoFollow(
        WindowsFilesystemHandle root,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedParent,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        const WindowsFileMetadataSnapshot& expectedMetadata) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole rootRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &rootRole)) return InvalidBoundHandle();
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        if (rootRole != WindowsFilesystemRootRole::Install &&
            rootRole != WindowsFilesystemRootRole::Backup) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult authorized =
            rootRole == WindowsFilesystemRootRole::Install
                ? RequireAuthorizedMutation(
                      root, WindowsFilesystemRootRole::Install)
                : RequireAuthorizedMutation(root,
                                             WindowsFilesystemRootRole::Backup);
        if (!authorized.ok()) return authorized;
        if ((rootRole == WindowsFilesystemRootRole::Install) !=
            expectedMetadata.valid) {
            return UnsupportedMetadata();
        }
        if (journalLeases_.empty()) return MissingJournalLease();
        std::wstring widePath;
        if (!RelativePathToWide(relativePath, widePath)) return InvalidRelativePath();

        ParentAndLeaf target;
        const WindowsFilesystemBackendResult located =
            OpenContainingDirectory(nativeRoot, widePath, target);
        if (!located.ok()) return located;
        WindowsFileIdentity parentIdentity;
        const WindowsFilesystemBackendResult parentBound =
            IdentityForHandle(target.parent.get(), parentIdentity);
        if (!parentBound.ok()) return parentBound;
        if (!expectedParent.valid || parentIdentity != expectedParent) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::IdentityMismatch,
                ERROR_INVALID_DATA);
        }

        NativeOpenResult opened;
        const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
            target.parent.get(), target.leaf, expectedIdentity,
            GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, opened);
        if (!bound.ok()) return bound;

        WindowsFileContentFingerprint fingerprint;
        const WindowsFilesystemBackendResult fingerprinted =
            FingerprintHandle(opened.handle.get(), fingerprint);
        if (!fingerprinted.ok()) return fingerprinted;
        if (!ContentMatches(fingerprint, expectedContent)) {
            return ContentMismatch();
        }
        WindowsFileMetadataSnapshot currentMetadata;
        const WindowsFilesystemBackendResult metadataCaptured =
            CaptureMetadata(opened.handle.get(), currentMetadata);
        if (!metadataCaptured.ok()) return metadataCaptured;
        if (expectedMetadata.valid &&
            !MetadataMatchesAfterRead(currentMetadata, expectedMetadata)) {
            return UnsupportedMetadata();
        }

        WindowsFileIdentity currentIdentity;
        const WindowsFilesystemBackendResult identityRechecked =
            IdentityForHandle(opened.handle.get(), currentIdentity);
        if (!identityRechecked.ok() || currentIdentity != expectedIdentity) {
            return identityRechecked.ok()
                ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                 ERROR_INVALID_DATA)
                : identityRechecked;
        }
        WindowsFileContentFingerprint contentRechecked;
        const WindowsFilesystemBackendResult contentRecheckedResult =
            FingerprintHandle(opened.handle.get(), contentRechecked);
        if (!contentRecheckedResult.ok() ||
            !ContentMatches(contentRechecked, expectedContent)) {
            return contentRecheckedResult.ok()
                ? ContentMismatch()
                : contentRecheckedResult;
        }
        WindowsFileMetadataSnapshot metadataRechecked;
        const WindowsFilesystemBackendResult metadataRecheckedResult =
            CaptureMetadata(opened.handle.get(), metadataRechecked);
        if (!metadataRecheckedResult.ok() ||
            (expectedMetadata.valid &&
             !MetadataMatchesAfterRead(metadataRechecked,
                                       expectedMetadata))) {
            return metadataRecheckedResult.ok()
                ? UnsupportedMetadata()
                : metadataRecheckedResult;
        }

        const WindowsFilesystemBackendResult marked =
            DeleteOpenEntryDurable(opened.handle, target.parent.get(),
                                   target.leaf, false, false, &testHooks_,
                                   &accessDeniedFailureConsumed_);
        if (!marked.ok()) return marked;
        return {};
    }

    WindowsFilesystemBackendResult CleanupInvocationResidueNoFollow(
        WindowsFilesystemHandle stagingRoot,
        WindowsFilesystemHandle transactionRoot,
        const WindowsBootstrapInvocationCleanup& cleanup) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeStaging = nullptr;
        HANDLE nativeTransaction = nullptr;
        WindowsFilesystemRootRole stagingRole = WindowsFilesystemRootRole::Install;
        WindowsFilesystemRootRole transactionRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(stagingRoot, nativeStaging, &stagingRole) ||
            !BoundHandle(transactionRoot, nativeTransaction, &transactionRole)) {
            return InvalidBoundHandle();
        }
        if (stagingRole != WindowsFilesystemRootRole::Staging ||
            transactionRole != WindowsFilesystemRootRole::Transaction) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult stagingValidated =
            RevalidateBoundRoot(stagingRoot);
        if (!stagingValidated.ok()) return stagingValidated;
        const WindowsFilesystemBackendResult transactionValidated =
            RevalidateBoundRoot(transactionRoot);
        if (!transactionValidated.ok()) return transactionValidated;
        const WindowsFilesystemBackendResult stagingAuthorized =
            RequireAuthorizedMutation(stagingRoot,
                                      WindowsFilesystemRootRole::Staging);
        if (!stagingAuthorized.ok()) return stagingAuthorized;
        const WindowsFilesystemBackendResult transactionAuthorized =
            RequireAuthorizedMutation(transactionRoot,
                                      WindowsFilesystemRootRole::Transaction);
        if (!transactionAuthorized.ok()) return transactionAuthorized;
        if (!HasJournalLeaseForRoot(transactionRoot)) {
            return MissingJournalLease();
        }

        const bool runtimeAuthorityComplete =
            cleanup.runtimePairExpected &&
            cleanup.runtimePairDirectoryIdentity.valid &&
            cleanup.runtimePairBootstrapIdentity.valid &&
            cleanup.runtimePairBootstrapContent.valid;
        if (!cleanup.runtimePairExpected &&
            (cleanup.runtimePairDirectoryIdentity.valid ||
             cleanup.runtimePairBootstrapIdentity.valid ||
             cleanup.runtimePairBootstrapContent.valid ||
             !cleanup.transactionFiles.empty())) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_DATA);
        }
        if (cleanup.runtimePairExpected &&
            (!runtimeAuthorityComplete || cleanup.transactionFiles.size() != 1U)) {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_DATA);
        }

        std::set<std::string> stagingNames;
        for (const auto& file : cleanup.stagingFiles) {
            std::wstring ignored;
            if (!file.identity.valid || !file.content.valid ||
                !RelativePathToWide(file.relativePath, ignored)) {
                return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                      ERROR_INVALID_NAME);
            }
            std::string folded = file.relativePath;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](const unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            if (!stagingNames.insert(std::move(folded)).second) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_DUP_NAME);
            }
        }
        std::set<std::string> transactionNames;
        for (const auto& file : cleanup.transactionFiles) {
            std::wstring ignored;
            if (!file.identity.valid || !file.content.valid ||
                !RelativePathToWide(file.relativePath, ignored)) {
                return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                      ERROR_INVALID_NAME);
            }
            std::string folded = file.relativePath;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](const unsigned char value) {
                               return static_cast<char>(std::tolower(value));
                           });
            if (!transactionNames.insert(std::move(folded)).second) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_DUP_NAME);
            }
            if (!EqualPathInsensitive(
                    std::wstring(ignored),
                    std::wstring(L"runtime-pair\\HarkenSearchBootstrap.exe"))) {
                return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                      ERROR_INVALID_NAME);
            }
        }

        const auto validateKnownFile = [&cleanup](
            HANDLE nativeRoot,
            const WindowsBootstrapInvocationCleanupFile& file) {
            std::wstring widePath;
            if (!RelativePathToWide(file.relativePath, widePath)) {
                return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                      ERROR_INVALID_NAME);
            }
                ParentAndLeaf target;
                const WindowsFilesystemBackendResult located =
                    OpenContainingDirectory(nativeRoot, widePath, target);
            if (!located.ok()) {
                if (located.status == WindowsFilesystemBackendStatus::NotFound &&
                    cleanup.allowAlreadyRemoved &&
                    widePath.find(L'\\') == std::wstring::npos) {
                    return WindowsFilesystemBackendResult{};
                }
                return located.status == WindowsFilesystemBackendStatus::NotFound &&
                               cleanup.allowAlreadyRemoved
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_PATH_NOT_FOUND)
                    : located;
            }
            NativeOpenResult opened;
            const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
                target.parent.get(), target.leaf, file.identity,
                GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_DELETE, opened);
            if (bound.status == WindowsFilesystemBackendStatus::NotFound) {
                return cleanup.allowAlreadyRemoved
                    ? WindowsFilesystemBackendResult{}
                    : BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_FILE_NOT_FOUND);
            }
            if (!bound.ok()) return bound;

            const WindowsFilesystemBackendResult metadata =
                ValidateSupportedMetadata(opened.handle.get());
            if (!metadata.ok()) return metadata;

            WindowsFileIdentity observedIdentity;
            const WindowsFilesystemBackendResult identityObserved =
                IdentityForHandle(opened.handle.get(), observedIdentity);
            if (!identityObserved.ok() || observedIdentity != file.identity) {
                return identityObserved.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : identityObserved;
            }
            WindowsFileContentFingerprint firstContent;
            const WindowsFilesystemBackendResult firstFingerprint =
                FingerprintHandle(opened.handle.get(), firstContent);
            if (!firstFingerprint.ok()) return firstFingerprint;
            if (!ContentMatches(firstContent, file.content)) {
                return ContentMismatch();
            }
            WindowsFileIdentity reboundIdentity;
            const WindowsFilesystemBackendResult reboundIdentityResult =
                IdentityForHandle(opened.handle.get(), reboundIdentity);
            if (!reboundIdentityResult.ok() || reboundIdentity != file.identity) {
                return reboundIdentityResult.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : reboundIdentityResult;
            }
            WindowsFileContentFingerprint secondContent;
            const WindowsFilesystemBackendResult secondFingerprint =
                FingerprintHandle(opened.handle.get(), secondContent);
            if (!secondFingerprint.ok()) return secondFingerprint;
            if (!ContentMatches(secondContent, file.content)) {
                return ContentMismatch();
            }
            return WindowsFilesystemBackendResult{};
        };

        const auto deleteKnownFile = [this, &cleanup](
            HANDLE nativeRoot,
            const WindowsBootstrapInvocationCleanupFile& file) {
            std::wstring widePath;
            if (!RelativePathToWide(file.relativePath, widePath)) {
                return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                      ERROR_INVALID_NAME);
            }
            ParentAndLeaf target;
            const WindowsFilesystemBackendResult located =
                OpenContainingDirectory(nativeRoot, widePath, target);
            if (!located.ok()) {
                if (located.status == WindowsFilesystemBackendStatus::NotFound &&
                    cleanup.allowAlreadyRemoved &&
                    widePath.find(L'\\') == std::wstring::npos) {
                    return WindowsFilesystemBackendResult{};
                }
                return located.status == WindowsFilesystemBackendStatus::NotFound &&
                               cleanup.allowAlreadyRemoved
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_PATH_NOT_FOUND)
                    : located;
            }
            NativeOpenResult opened;
            const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
                target.parent.get(), target.leaf, file.identity,
                GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_DELETE, opened);
            if (bound.status == WindowsFilesystemBackendStatus::NotFound) {
                return cleanup.allowAlreadyRemoved
                    ? WindowsFilesystemBackendResult{}
                    : BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_FILE_NOT_FOUND);
            }
            if (!bound.ok()) return bound;
            const WindowsFilesystemBackendResult metadata =
                ValidateSupportedMetadata(opened.handle.get());
            if (!metadata.ok()) return metadata;
            WindowsFileContentFingerprint content;
            const WindowsFilesystemBackendResult fingerprinted =
                FingerprintHandle(opened.handle.get(), content);
            if (!fingerprinted.ok()) return fingerprinted;
            if (!ContentMatches(content, file.content)) return ContentMismatch();
            WindowsFileIdentity identity;
            const WindowsFilesystemBackendResult reboundIdentity =
                IdentityForHandle(opened.handle.get(), identity);
            if (!reboundIdentity.ok() || identity != file.identity) {
                return reboundIdentity.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : reboundIdentity;
            }
            WindowsFileContentFingerprint reboundContent;
            const WindowsFilesystemBackendResult reboundFingerprint =
                FingerprintHandle(opened.handle.get(), reboundContent);
            if (!reboundFingerprint.ok()) return reboundFingerprint;
            if (!ContentMatches(reboundContent, file.content)) {
                return ContentMismatch();
            }
            return DeleteOpenEntryDurable(
                opened.handle, target.parent.get(), target.leaf, true, false,
                &testHooks_, &accessDeniedFailureConsumed_);
        };

        const auto validateRuntimeFile = [&cleanup](HANDLE runtimePairRoot,
                                            const WindowsBootstrapInvocationCleanupFile& file) {
            NativeOpenResult opened;
            const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
                runtimePairRoot, kRuntimePairBootstrapName,
                file.identity,
                GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                opened);
            if (bound.status == WindowsFilesystemBackendStatus::NotFound) {
                return cleanup.allowAlreadyRemoved
                    ? WindowsFilesystemBackendResult{}
                    : BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_FILE_NOT_FOUND);
            }
            if (!bound.ok()) return bound;
            const WindowsFilesystemBackendResult metadata =
                ValidateSupportedMetadata(opened.handle.get());
            if (!metadata.ok()) return metadata;
            WindowsFileContentFingerprint firstContent;
            const WindowsFilesystemBackendResult firstFingerprint =
                FingerprintHandle(opened.handle.get(), firstContent);
            if (!firstFingerprint.ok()) return firstFingerprint;
            if (!ContentMatches(firstContent, file.content)) return ContentMismatch();
            WindowsFileIdentity reboundIdentity;
            const WindowsFilesystemBackendResult identityObserved =
                IdentityForHandle(opened.handle.get(), reboundIdentity);
            if (!identityObserved.ok() || reboundIdentity != file.identity) {
                return identityObserved.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : identityObserved;
            }
            WindowsFileContentFingerprint secondContent;
            const WindowsFilesystemBackendResult secondFingerprint =
                FingerprintHandle(opened.handle.get(), secondContent);
            if (!secondFingerprint.ok()) return secondFingerprint;
            if (!ContentMatches(secondContent, file.content)) return ContentMismatch();
            return WindowsFilesystemBackendResult{};
        };

        const auto deleteRuntimeFile = [this, &cleanup](
            HANDLE runtimePairRoot,
            const WindowsBootstrapInvocationCleanupFile& file) {
            NativeOpenResult opened;
            const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
                runtimePairRoot, kRuntimePairBootstrapName, file.identity,
                GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_DELETE, opened);
            if (bound.status == WindowsFilesystemBackendStatus::NotFound) {
                return cleanup.allowAlreadyRemoved
                    ? WindowsFilesystemBackendResult{}
                    : BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_FILE_NOT_FOUND);
            }
            if (!bound.ok()) return bound;
            const WindowsFilesystemBackendResult metadata =
                ValidateSupportedMetadata(opened.handle.get());
            if (!metadata.ok()) return metadata;
            WindowsFileContentFingerprint content;
            const WindowsFilesystemBackendResult fingerprinted =
                FingerprintHandle(opened.handle.get(), content);
            if (!fingerprinted.ok()) return fingerprinted;
            if (!ContentMatches(content, file.content)) return ContentMismatch();
            WindowsFileIdentity identity;
            const WindowsFilesystemBackendResult identityObserved =
                IdentityForHandle(opened.handle.get(), identity);
            if (!identityObserved.ok() || identity != file.identity) {
                return identityObserved.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : identityObserved;
            }
            WindowsFileContentFingerprint reboundContent;
            const WindowsFilesystemBackendResult reboundFingerprint =
                FingerprintHandle(opened.handle.get(), reboundContent);
            if (!reboundFingerprint.ok()) return reboundFingerprint;
            if (!ContentMatches(reboundContent, file.content)) {
                return ContentMismatch();
            }
            return DeleteOpenEntryDurable(
                opened.handle, runtimePairRoot, kRuntimePairBootstrapName,
                true, false, &testHooks_, &accessDeniedFailureConsumed_);
        };

        const auto validateRuntimeNamespace =
            [&](HANDLE runtimePairRoot,
                const WindowsBootstrapInvocationCleanupFile& expectedFile) {
                std::vector<NativeDirectoryEntrySnapshot> entries;
                const WindowsFilesystemBackendResult enumerated =
                    ReadDirectoryEntriesNoFollow(runtimePairRoot, entries);
                if (!enumerated.ok()) return enumerated;
                bool expectedSeen = false;
                for (const auto& entry : entries) {
                    if (entry.name == L"." || entry.name == L"..") continue;
                    if (!EqualPathInsensitive(entry.name,
                                              kRuntimePairBootstrapName)) {
                        return BackendFailure(
                            WindowsFilesystemBackendStatus::DirectoryNotEmpty,
                            ERROR_DIR_NOT_EMPTY);
                    }
                    if (expectedSeen) {
                        return BackendFailure(
                            WindowsFilesystemBackendStatus::DirectoryNotEmpty,
                            ERROR_DIR_NOT_EMPTY);
                    }
                    if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                        return BackendFailure(
                            WindowsFilesystemBackendStatus::ReparsePoint,
                            ERROR_REPARSE_TAG_INVALID);
                    }
                    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        return BackendFailure(
                            WindowsFilesystemBackendStatus::DirectoryNotEmpty,
                            ERROR_DIR_NOT_EMPTY);
                    }
                    expectedSeen = true;
                }
                if (!expectedSeen) return WindowsFilesystemBackendResult{};
                return validateRuntimeFile(runtimePairRoot, expectedFile);
            };

        NativeOpenResult runtimePairForDelete;
        WindowsFileIdentity runtimePairIdentity;
        bool runtimePairPresent = false;
        bool runtimePairMissing = false;
        if (!cleanup.runtimePairExpected) {
            const WindowsFilesystemBackendResult absent =
                RequireRuntimePairMissing(nativeTransaction);
            if (!absent.ok()) return absent;
        }
        if (!cleanup.transactionFiles.empty()) {
            NativeOpenResult initialRuntimePair;
            WindowsFilesystemObservation initialObservation;
            const WindowsFilesystemBackendResult initialOpened =
                OpenObservedRelative(
                    nativeTransaction, kRuntimePairDirectoryName,
                    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
                        DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    kFileDirectoryFile, initialRuntimePair,
                    initialObservation);
            if (initialOpened.status == WindowsFilesystemBackendStatus::NotFound) {
                if (!cleanup.allowAlreadyRemoved) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_FILE_NOT_FOUND);
                }
                const WindowsFilesystemBackendResult absent =
                    RequireRuntimePairMissing(nativeTransaction);
                if (!absent.ok()) return absent;
                runtimePairMissing = true;
            } else {
                if (!initialOpened.ok()) return initialOpened;
                if (initialObservation.kind ==
                    WindowsFilesystemEntryKind::ReparsePoint) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::ReparsePoint,
                        ERROR_REPARSE_TAG_INVALID);
                }
                if (initialObservation.kind !=
                        WindowsFilesystemEntryKind::Directory ||
                    !initialObservation.identity.valid) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::NotDirectory,
                        ERROR_DIRECTORY);
                }
                if (initialObservation.identity !=
                    cleanup.runtimePairDirectoryIdentity) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_INVALID_DATA);
                }
                runtimePairPresent = true;
                runtimePairIdentity = cleanup.runtimePairDirectoryIdentity;
                const WindowsFilesystemBackendResult namespaceChecked =
                    validateRuntimeNamespace(
                        initialRuntimePair.handle.get(),
                        cleanup.transactionFiles.front());
                if (!namespaceChecked.ok()) return namespaceChecked;

                // The existing native test seam is intentionally exercised at
                // this exact race boundary. Production callers leave it null.
                if (testHooks_.beforeTargetMutation != nullptr) {
                    testHooks_.beforeTargetMutation(
                        testHooks_.context,
                        WindowsFilesystemRootRole::Transaction,
                        "runtime-pair");
                }
                initialRuntimePair.handle.reset();

                WindowsFilesystemObservation reboundObservation;
                const WindowsFilesystemBackendResult rebound =
                    OpenObservedRelative(
                        nativeTransaction, kRuntimePairDirectoryName,
                        FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                            FILE_READ_ATTRIBUTES | GENERIC_WRITE | DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        kFileDirectoryFile, runtimePairForDelete,
                        reboundObservation);
                if (!rebound.ok()) {
                    return rebound.status == WindowsFilesystemBackendStatus::NotFound
                        ? BackendFailure(
                              WindowsFilesystemBackendStatus::IdentityMismatch,
                              ERROR_INVALID_DATA)
                        : rebound;
                }
                if (reboundObservation.kind ==
                        WindowsFilesystemEntryKind::ReparsePoint) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::ReparsePoint,
                        ERROR_REPARSE_TAG_INVALID);
                }
                if (reboundObservation.kind !=
                        WindowsFilesystemEntryKind::Directory ||
                    reboundObservation.identity != runtimePairIdentity) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_INVALID_DATA);
                }
                const WindowsFilesystemBackendResult reboundNamespace =
                    validateRuntimeNamespace(
                        runtimePairForDelete.handle.get(),
                        cleanup.transactionFiles.front());
                if (!reboundNamespace.ok()) return reboundNamespace;
            }
        }

        if (runtimePairPresent) {
            NativeOpenResult currentImageCandidate;
            const WindowsFilesystemBackendResult candidateOpened =
                OpenExpectedRegularFile(
                    runtimePairForDelete.handle.get(),
                    kRuntimePairBootstrapName,
                    cleanup.transactionFiles.front().identity,
                    GENERIC_READ | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    currentImageCandidate);
            if (!candidateOpened.ok()) return candidateOpened;
            bool currentProcessImage = false;
            const WindowsFilesystemBackendResult imageChecked =
                CurrentProcessImageMatches(
                    currentImageCandidate.handle.get(), currentProcessImage);
            if (!imageChecked.ok()) return imageChecked;
            if (currentProcessImage) {
                return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                      ERROR_ACCESS_DENIED);
            }
        }

        // Every known file must pass identity/content validation before the
        // first durable delete. This keeps an unknown runtime child or a
        // replaced staging file from causing partial invocation cleanup.
        for (const auto& file : cleanup.stagingFiles) {
            const WindowsFilesystemBackendResult validated =
                validateKnownFile(nativeStaging, file);
            if (!validated.ok()) return validated;
        }

        if (runtimePairMissing) {
            const WindowsFilesystemBackendResult absent =
                RequireRuntimePairMissing(nativeTransaction);
            if (!absent.ok()) return absent;
        }
        if (runtimePairPresent) {
            const WindowsFilesystemBackendResult namespaceChecked =
                validateRuntimeNamespace(
                    runtimePairForDelete.handle.get(),
                    cleanup.transactionFiles.front());
            if (!namespaceChecked.ok()) return namespaceChecked;
        }

        for (const auto& file : cleanup.stagingFiles) {
            if (runtimePairMissing) {
                const WindowsFilesystemBackendResult absent =
                    RequireRuntimePairMissing(nativeTransaction);
                if (!absent.ok()) return absent;
            }
            if (runtimePairPresent) {
                const WindowsFilesystemBackendResult namespaceChecked =
                    validateRuntimeNamespace(
                        runtimePairForDelete.handle.get(),
                        cleanup.transactionFiles.front());
                if (!namespaceChecked.ok()) return namespaceChecked;
            }
            const WindowsFilesystemBackendResult removed =
                deleteKnownFile(nativeStaging, file);
            if (!removed.ok()) return removed;
            const WindowsFilesystemBackendResult rebound =
                RevalidateBoundRoot(stagingRoot);
            if (!rebound.ok()) return rebound;
        }
        for (const auto& file : cleanup.transactionFiles) {
            if (runtimePairMissing) {
                const WindowsFilesystemBackendResult absent =
                    RequireRuntimePairMissing(nativeTransaction);
                if (!absent.ok()) return absent;
                const WindowsFilesystemBackendResult rebound =
                    RevalidateBoundRoot(transactionRoot);
                if (!rebound.ok()) return rebound;
                continue;
            }
            const WindowsFilesystemBackendResult removed = runtimePairPresent
                ? deleteRuntimeFile(runtimePairForDelete.handle.get(), file)
                : deleteKnownFile(nativeTransaction, file);
            if (!removed.ok()) return removed;
            const WindowsFilesystemBackendResult rebound =
                RevalidateBoundRoot(transactionRoot);
            if (!rebound.ok()) return rebound;
        }

        if (runtimePairPresent) {
            runtimePairForDelete.handle.reset();
            NativeOpenResult directoryForDelete;
            WindowsFilesystemObservation directoryObservation;
            const WindowsFilesystemBackendResult directoryRebound =
                OpenObservedRelative(
                    nativeTransaction, kRuntimePairDirectoryName,
                    FILE_LIST_DIRECTORY | FILE_TRAVERSE |
                        FILE_READ_ATTRIBUTES | DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    kFileDirectoryFile, directoryForDelete,
                    directoryObservation);
            if (!directoryRebound.ok()) {
                return directoryRebound.status ==
                           WindowsFilesystemBackendStatus::NotFound
                    ? BackendFailure(
                          WindowsFilesystemBackendStatus::IdentityMismatch,
                          ERROR_INVALID_DATA)
                    : directoryRebound;
            }
            if (directoryObservation.kind ==
                    WindowsFilesystemEntryKind::ReparsePoint) {
                return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                      ERROR_REPARSE_TAG_INVALID);
            }
            if (directoryObservation.kind !=
                    WindowsFilesystemEntryKind::Directory ||
                directoryObservation.identity != runtimePairIdentity) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_INVALID_DATA);
            }
            const WindowsFilesystemBackendResult emptyChecked =
                validateRuntimeNamespace(
                    directoryForDelete.handle.get(),
                    cleanup.transactionFiles.front());
            if (!emptyChecked.ok()) return emptyChecked;
            const WindowsFilesystemBackendResult removedDirectory =
                DeleteOpenEntryDurable(
                    directoryForDelete.handle, nativeTransaction,
                    L"runtime-pair", true, true, &testHooks_,
                    &accessDeniedFailureConsumed_);
            if (!removedDirectory.ok()) return removedDirectory;
        }
        const WindowsFilesystemBackendResult runtimePairAbsent =
            RequireRuntimePairMissing(nativeTransaction);
        if (!runtimePairAbsent.ok()) return runtimePairAbsent;
        return RevalidateBoundRoot(transactionRoot);
    }

    WindowsFilesystemBackendResult PersistJournalDurable(
        WindowsFilesystemHandle root,
        const std::vector<std::uint8_t>& bytes,
        WindowsFilesystemReplaceMode mode,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputIdentity = {};
        outputContent = {};
        HANDLE nativeRoot = nullptr;
        const auto binding = handles_.find(root);
        if (binding == handles_.end()) return InvalidBoundHandle();
        if (binding->second.role != WindowsFilesystemRootRole::Transaction) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(root,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        nativeRoot = reinterpret_cast<HANDLE>(root);
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        auto activeLease = journalLeases_.end();
        for (auto lease = journalLeases_.begin();
             lease != journalLeases_.end(); ++lease) {
            if (lease->second.root != root || activeLease != journalLeases_.end()) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::AccessDenied,
                    ERROR_SHARING_VIOLATION);
            }
            activeLease = lease;
        }
        const WindowsFilesystemBackendResult namespaceValid =
            ValidateJournalNamespace(nativeRoot);
        if (!namespaceValid.ok()) return namespaceValid;

        const auto observeMissing = [nativeRoot](const wchar_t* name) {
            NativeOpenResult opened;
            WindowsFilesystemObservation observation;
            const WindowsFilesystemBackendResult observed = OpenObservedRelative(
                nativeRoot, name, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                0, opened, observation);
            if (observed.status == WindowsFilesystemBackendStatus::NotFound) {
                return WindowsFilesystemBackendResult{};
            }
            if (observed.ok()) {
                return BackendFailure(WindowsFilesystemBackendStatus::AlreadyExists,
                                      ERROR_FILE_EXISTS);
            }
            return observed;
        };

        const wchar_t* activeName = nullptr;
        const wchar_t* targetName = kJournalSlotAName;
        const wchar_t* temporaryName = kJournalSlotATempName;
        NativeOpenResult existing;
        const auto knownIdentity = journalIdentities_.find(root);
        const auto knownSlot = journalActiveSlots_.find(root);
        if (mode == WindowsFilesystemReplaceMode::CreateNew) {
            if (expectedIdentity.valid || expectedContent.valid ||
                knownIdentity != journalIdentities_.end() ||
                knownSlot != journalActiveSlots_.end()) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_INVALID_DATA);
            }
            for (const wchar_t* candidate : {kJournalSlotAName, kJournalSlotBName,
                                             kJournalSlotATempName,
                                             kJournalSlotBTempName,
                                             kLegacyJournalFileName,
                                             kLegacyJournalTempName}) {
                const WindowsFilesystemBackendResult absent =
                    observeMissing(candidate);
                if (!absent.ok()) return absent;
            }
        } else if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            if (!expectedIdentity.valid || !expectedContent.valid ||
                (knownIdentity != journalIdentities_.end() &&
                 knownIdentity->second != expectedIdentity)) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_INVALID_DATA);
            }
            if (knownSlot != journalActiveSlots_.end()) {
                activeName = knownSlot->second == kJournalSlotAName
                    ? kJournalSlotAName
                    : kJournalSlotBName;
            } else {
                for (const wchar_t* candidate : {kJournalSlotAName,
                                                 kJournalSlotBName}) {
                    NativeOpenResult observed;
                    WindowsFilesystemObservation observation;
                    const WindowsFilesystemBackendResult rebound =
                        OpenObservedRelative(
                            nativeRoot, candidate,
                            GENERIC_READ | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            0, observed, observation);
                    if (rebound.status == WindowsFilesystemBackendStatus::NotFound) {
                        continue;
                    }
                    if (!rebound.ok()) return rebound;
                    if (observation.kind != WindowsFilesystemEntryKind::RegularFile ||
                        observation.identity != expectedIdentity) {
                        continue;
                    }
                    if (activeName != nullptr) {
                        return BackendFailure(
                            WindowsFilesystemBackendStatus::IdentityMismatch,
                            ERROR_DUP_NAME);
                    }
                    activeName = candidate;
                }
            }
            if (activeName == nullptr) {
                return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                      ERROR_INVALID_DATA);
            }
            const WindowsFilesystemBackendResult opened = OpenExpectedRegularFile(
                nativeRoot, activeName, expectedIdentity,
                GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                existing);
            if (!opened.ok()) return opened;
            WindowsFileContentFingerprint currentContent;
            const WindowsFilesystemBackendResult fingerprinted =
                FingerprintHandle(existing.handle.get(), currentContent);
            if (!fingerprinted.ok()) return fingerprinted;
            if (!ContentMatches(currentContent, expectedContent)) {
                return ContentMismatch();
            }
            // A fresh backend may discover the active slot by identity rather
            // than through ReadJournalCandidates.  Capture its content now so
            // a later generation can authenticate the inactive slot before it
            // is overwritten.
            journalSlotIdentities_[root][activeName] = expectedIdentity;
            journalSlotContents_[root][activeName] = expectedContent;
            if (activeName == kJournalSlotAName) {
                targetName = kJournalSlotBName;
                temporaryName = kJournalSlotBTempName;
            } else {
                targetName = kJournalSlotAName;
                temporaryName = kJournalSlotATempName;
            }
            const WindowsFilesystemBackendResult temporaryAbsent =
                observeMissing(temporaryName);
            if (!temporaryAbsent.ok()) return temporaryAbsent;

            NativeOpenResult inactive;
            WindowsFilesystemObservation inactiveObservation;
            const WindowsFilesystemBackendResult inactiveObserved =
                OpenObservedRelative(
                    nativeRoot, targetName,
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, inactive, inactiveObservation);
            const auto knownSlots = journalSlotIdentities_.find(root);
            const auto knownInactive = knownSlots == journalSlotIdentities_.end()
                ? std::map<std::wstring, WindowsFileIdentity>::const_iterator{}
                : knownSlots->second.find(targetName);
            const auto knownContents = journalSlotContents_.find(root);
            const auto knownInactiveContent =
                knownContents == journalSlotContents_.end()
                    ? std::map<std::wstring,
                               WindowsFileContentFingerprint>::const_iterator{}
                    : knownContents->second.find(targetName);
            const bool inactiveWasKnown =
                knownSlots != journalSlotIdentities_.end() &&
                knownInactive != knownSlots->second.end();
            if (inactiveObserved.ok()) {
                if (inactiveObservation.kind !=
                        WindowsFilesystemEntryKind::RegularFile ||
                    !inactiveWasKnown ||
                    inactiveObservation.identity != knownInactive->second ||
                    knownContents == journalSlotContents_.end() ||
                    knownInactiveContent == knownContents->second.end() ||
                    !knownInactiveContent->second.valid) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_INVALID_DATA);
                }
                WindowsFileContentFingerprint inactiveContent;
                const WindowsFilesystemBackendResult inactiveFingerprinted =
                    FingerprintHandle(inactive.handle.get(), inactiveContent);
                if (!inactiveFingerprinted.ok()) return inactiveFingerprinted;
                if (!ContentMatches(inactiveContent,
                                    knownInactiveContent->second)) {
                    return ContentMismatch();
                }
            } else if (inactiveObserved.status ==
                           WindowsFilesystemBackendStatus::NotFound) {
                if (inactiveWasKnown) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_FILE_NOT_FOUND);
                }
            } else {
                return inactiveObserved;
            }
        } else {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_PARAMETER);
        }

        ScopedHandle temporary;
        const WindowsFilesystemBackendResult created =
            CreateNewFile(nativeRoot, temporaryName, temporary);
        if (!created.ok()) return created;

        WindowsFilesystemBackendResult written =
            WriteBytes(temporary.get(), bytes.data(), bytes.size());
        if (written.ok() && !FlushFileBuffers(temporary.get())) {
            written = BackendFailure(WindowsFilesystemBackendStatus::DurabilityFailed,
                                     GetLastError());
        }
        WindowsFileIdentity journalIdentity;
        if (written.ok()) written = IdentityForHandle(temporary.get(), journalIdentity);
        WindowsFileContentFingerprint journalContent;
        if (written.ok()) written = FingerprintHandle(temporary.get(), journalContent);
        WindowsFileContentFingerprint intendedContent;
        if (written.ok()) written = FingerprintBytes(bytes, intendedContent);
        if (written.ok() && !ContentMatches(journalContent, intendedContent)) {
            written = ContentMismatch();
        }
        if (!written.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, nativeRoot,
                                    temporaryName);
            return cleaned.ok() ? written : cleaned;
        }

        // The file's bytes are durable, but the directory entry is not yet
        // published durably.  This is a real restart cut (a crash here can
        // leave a durable temp with no durable name), so expose it separately
        // from the later post-directory-flush hook.
        const WindowsFilesystemBackendResult afterTemporaryFileFlush =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    JournalAfterTemporaryFileFlushBeforeDirectoryFlush);
        if (!afterTemporaryFileFlush.ok()) {
            temporary.reset();
            return MakePartialDurable(afterTemporaryFileFlush);
        }

        const WindowsFilesystemBackendResult temporaryEntryFlushed =
            FlushDirectory(nativeRoot);
        if (!temporaryEntryFlushed.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, nativeRoot,
                                    temporaryName);
            return cleaned.ok() ? temporaryEntryFlushed : cleaned;
        }
        const WindowsFilesystemBackendResult afterTemporaryFlush =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    JournalAfterTemporaryFlush);
        if (!afterTemporaryFlush.ok()) {
            temporary.reset();
            return MakePartialDurable(afterTemporaryFlush);
        }

        if (mode == WindowsFilesystemReplaceMode::ReplaceExisting) {
            WindowsFileIdentity currentIdentity;
            const WindowsFilesystemBackendResult rebound =
                IdentityForHandle(existing.handle.get(), currentIdentity);
            if (!rebound.ok() || currentIdentity != expectedIdentity) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, nativeRoot,
                                        temporaryName);
                if (!cleaned.ok()) return cleaned;
                return rebound.ok()
                    ? BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                     ERROR_INVALID_DATA)
                    : rebound;
            }
            WindowsFileContentFingerprint currentContent;
            const WindowsFilesystemBackendResult refingerprinted =
                FingerprintHandle(existing.handle.get(), currentContent);
            if (!refingerprinted.ok() ||
                !ContentMatches(currentContent, expectedContent)) {
                const WindowsFilesystemBackendResult cleaned =
                    CleanupCreatedEntry(temporary, nativeRoot,
                                        temporaryName);
                if (!cleaned.ok()) return cleaned;
                return refingerprinted.ok() ? ContentMismatch() : refingerprinted;
            }
        }

        const WindowsFilesystemBackendResult renamed = RenameOpenHandle(
            temporary.get(), nativeRoot, targetName,
            mode == WindowsFilesystemReplaceMode::ReplaceExisting);
        if (!renamed.ok()) {
            const WindowsFilesystemBackendResult cleaned =
                CleanupCreatedEntry(temporary, nativeRoot,
                                    temporaryName);
            return cleaned.ok() ? renamed : cleaned;
        }
        const WindowsFilesystemBackendResult afterSlotPublish =
            FailureAtTestPoint(
                WindowsBootstrapFilesystemNativeTestFailurePoint::
                    JournalAfterSlotPublishBeforeDirectoryFlush);
        if (!afterSlotPublish.ok()) return MakePartialDurable(afterSlotPublish);
        NativeOpenResult published;
        const WindowsFilesystemBackendResult rebound = OpenExpectedRegularFile(
            nativeRoot, targetName, journalIdentity,
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            published);
        if (!rebound.ok()) return MakePartialDurable(rebound);
        journalIdentities_[root] = journalIdentity;
        journalActiveSlots_[root] = targetName;
        journalSlotIdentities_[root][targetName] = journalIdentity;
        journalSlotContents_[root][targetName] = journalContent;
        const WindowsFilesystemBackendResult flushed = FlushDirectory(nativeRoot);
        if (!flushed.ok()) return MakePartialDurable(flushed);
        if (activeLease != journalLeases_.end()) {
            activeLease->second.handle = std::move(temporary);
            activeLease->second.identity = journalIdentity;
            activeLease->second.content = journalContent;
        }
        outputIdentity = journalIdentity;
        outputContent = journalContent;
        return {};
    }

    WindowsFilesystemBackendResult ValidateTrustedWriteRoots(
        const ValidatedInstallPlan& plan,
        const WindowsBootstrapFilesystemRoots& roots) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto reject = [](
            const char* stage,
            const WindowsFilesystemBackendResult& failure) {
            WriteLocalDemoTrustedRootsDiagnostic(stage, failure);
            return failure;
        };
        WriteLocalDemoTrustedRootsDiagnostic("entered");
        writeRootsAuthorized_ = false;
        authorizedInstallPath_.clear();
        authorizedStagingPath_.clear();
        authorizedTransactionPath_.clear();
        authorizedInstallIdentity_ = {};
        authorizedStagingIdentity_ = {};
        authorizedTransactionIdentity_ = {};
        if (!plan.isValidated()) {
            return reject(
                "plan-invalid",
                BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                               ERROR_INVALID_DATA));
        }
        if (!trustedRoots_.authenticated ||
            trustedRoots_.updaterOwnedBase.empty() ||
            trustedRoots_.invocationIdentity.empty() ||
            trustedRoots_.stagingRoot.empty() ||
            trustedRoots_.transactionRoot.empty() ||
            roots.updaterOwnedBase != trustedRoots_.updaterOwnedBase ||
            roots.invocationIdentity != trustedRoots_.invocationIdentity ||
            plan.invocation.invocationIdentity !=
                trustedRoots_.invocationIdentity) {
            return reject(
                "trusted-binding-mismatch",
                BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                               ERROR_ACCESS_DENIED));
        }
        if (!trustedRoots_.expectedUpdaterOwnedBase.empty()) {
            std::wstring expectedBase;
            const WindowsFilesystemBackendResult expectedNormalized =
                NormalizeAbsoluteRoot(trustedRoots_.expectedUpdaterOwnedBase,
                                      expectedBase);
            if (!expectedNormalized.ok()) {
                return reject("expected-base-normalize", expectedNormalized);
            }
            std::wstring suppliedBase;
            const WindowsFilesystemBackendResult suppliedNormalized =
                NormalizeAbsoluteRoot(roots.updaterOwnedBase, suppliedBase);
            if (!suppliedNormalized.ok() ||
                !EqualPathInsensitive(expectedBase, suppliedBase)) {
                return reject(
                    "expected-base-mismatch",
                    BackendFailure(
                        WindowsFilesystemBackendStatus::AccessDenied,
                        ERROR_ACCESS_DENIED));
            }
        }
        if (trustedRoots_.authenticatedParentProcess.valid &&
            !SameAuthenticatedParentProcess(
                  trustedRoots_.authenticatedParentProcess,
                  plan.invocation.parentProcess)) {
            return reject(
                "parent-process-mismatch",
                BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                               ERROR_ACCESS_DENIED));
        }
        if (trustedRoots_.terminalCleanup) {
            if (!trustedRoots_.terminalCleanupParentProcess.valid ||
                trustedRoots_.transactionRoot.empty()) {
                return reject(
                    "terminal-parent-missing",
                    BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                   ERROR_ACCESS_DENIED));
            }
            std::wstring terminalImage;
            std::wstring transactionRoot;
            const WindowsFilesystemBackendResult terminalNormalized =
                NormalizeAbsoluteRoot(
                    trustedRoots_.terminalCleanupParentProcess.imagePath,
                    terminalImage);
            const WindowsFilesystemBackendResult transactionNormalized =
                NormalizeAbsoluteRoot(trustedRoots_.transactionRoot,
                                      transactionRoot);
            if (!terminalNormalized.ok() || !transactionNormalized.ok()) {
                return reject(
                    "terminal-parent-normalize",
                    BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                   ERROR_ACCESS_DENIED));
            }
            transactionRoot.append(L"\\runtime-pair\\HarkenSearchBootstrap.exe");
            if (!EqualPathInsensitive(terminalImage, transactionRoot)) {
                return reject(
                    "terminal-parent-path-mismatch",
                    BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                   ERROR_ACCESS_DENIED));
            }
        }
        if (!handles_.empty()) {
            // Authorization must precede any root capability.  Rebinding an
            // already-open untrusted handle would leave its access mode and
            // identity outside the authenticated invocation boundary.
            return reject(
                "handles-already-open",
                BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                               ERROR_ACCESS_DENIED));
        }
        std::wstring base;
        std::wstring staging;
        std::wstring transaction;
        std::wstring install;
        std::wstring trustedStaging;
        std::wstring trustedTransaction;
        std::wstring signedBase;
        std::wstring signedStaging;
        std::wstring signedTransaction;
        WindowsFilesystemBackendResult normalized =
            NormalizeAbsoluteRoot(trustedRoots_.updaterOwnedBase, base);
        if (!normalized.ok()) return reject("base-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(roots.stagingRoot, staging);
        if (!normalized.ok()) return reject("staging-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(roots.transactionRoot, transaction);
        if (!normalized.ok()) return reject("transaction-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(trustedRoots_.stagingRoot,
                                           trustedStaging);
        if (!normalized.ok()) return reject("trusted-staging-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(trustedRoots_.transactionRoot,
                                           trustedTransaction);
        if (!normalized.ok()) return reject("trusted-transaction-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(plan.invocation.updaterOwnedBase,
                                           signedBase);
        if (!normalized.ok()) return reject("signed-base-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(plan.invocation.stagingRoot,
                                           signedStaging);
        if (!normalized.ok()) return reject("signed-staging-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(plan.invocation.transactionRoot,
                                           signedTransaction);
        if (!normalized.ok()) return reject("signed-transaction-normalize", normalized);
        normalized = NormalizeAbsoluteRoot(plan.identity.targetRoot, install);
        if (!normalized.ok()) return reject("install-normalize", normalized);
        if (!EqualPathInsensitive(staging, trustedStaging) ||
            !EqualPathInsensitive(transaction, trustedTransaction) ||
            !EqualPathInsensitive(base, signedBase) ||
            !EqualPathInsensitive(staging, signedStaging) ||
            !EqualPathInsensitive(transaction, signedTransaction) ||
            !IsSameOrDescendantPath(staging, base) ||
            EqualPathInsensitive(staging, base) ||
            !IsSameOrDescendantPath(transaction, base) ||
            EqualPathInsensitive(transaction, base) ||
            PathsOverlap(staging, transaction) ||
            PathsOverlap(install, base) || PathsOverlap(install, staging) ||
            PathsOverlap(install, transaction)) {
            return reject(
                "path-policy-rejected",
                BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                               ERROR_ACCESS_DENIED));
        }
        if (trustedRoots_.invocationIdentity.size() > 256U ||
            trustedRoots_.invocationIdentity.find('~') != std::string::npos) {
            return reject(
                "invocation-id-rejected",
                BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                               ERROR_INVALID_NAME));
        }

        // Bind the canonical identities before handing any writable root
        // capability to the caller.  OpenRootExact repeats the exact-path and
        // FileId check against these values, so a reparse/rename substitution
        // between policy validation and the first mutation is rejected rather
        // than silently becoming the authority for this invocation.  The
        // probes are read-only and scoped; no namespace mutation occurs here.
        const auto prebind = [](const std::wstring& path,
                                WindowsFileIdentity& identity,
                                const bool requireTrustedStorage) {
            PinnedRootOpen opened;
            const WindowsFilesystemBackendResult result =
                OpenPinnedAbsoluteRoot(path, GENERIC_READ, opened);
            if (!result.ok()) return result;
            if (opened.observation.kind != WindowsFilesystemEntryKind::Directory ||
                !opened.observation.identity.valid) {
                return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                                      ERROR_DIRECTORY);
            }
            if (requireTrustedStorage) {
                const WindowsFilesystemBackendResult security =
                    ValidateTrustedStorageSecurity(opened.operation.get());
                if (!security.ok()) return security;
            }
            identity = opened.observation.identity;
            return WindowsFilesystemBackendResult{};
        };
        WindowsFileIdentity baseIdentity;
        WindowsFileIdentity installIdentity;
        WindowsFileIdentity stagingIdentity;
        WindowsFileIdentity transactionIdentity;
        WriteLocalDemoTrustedRootsDiagnostic("prebind-base");
        normalized = prebind(base, baseIdentity, true);
        if (!normalized.ok()) return reject("prebind-base-failed", normalized);
        WriteLocalDemoTrustedRootsDiagnostic("prebind-install");
        normalized = prebind(install, installIdentity, false);
        if (!normalized.ok()) return reject("prebind-install-failed", normalized);
        WriteLocalDemoTrustedRootsDiagnostic("prebind-staging");
        normalized = prebind(staging, stagingIdentity, true);
        if (!normalized.ok()) return reject("prebind-staging-failed", normalized);
        WriteLocalDemoTrustedRootsDiagnostic("prebind-transaction");
        normalized = prebind(transaction, transactionIdentity, true);
        if (!normalized.ok()) return reject("prebind-transaction-failed", normalized);
        if (installIdentity.volumeSerial != stagingIdentity.volumeSerial ||
            installIdentity.volumeSerial != transactionIdentity.volumeSerial ||
            installIdentity == stagingIdentity ||
            installIdentity == transactionIdentity ||
            stagingIdentity == transactionIdentity) {
            return reject(
                "identity-policy-rejected",
                BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                               ERROR_INVALID_DATA));
        }

        authorizedInstallPath_ = std::move(install);
        authorizedStagingPath_ = std::move(staging);
        authorizedTransactionPath_ = std::move(transaction);
        authorizedInstallIdentity_ = installIdentity;
        authorizedStagingIdentity_ = stagingIdentity;
        authorizedTransactionIdentity_ = transactionIdentity;
        writeRootsAuthorized_ = true;
        WriteLocalDemoTrustedRootsDiagnostic("success");
        return {};
    }

    WindowsFilesystemBackendResult ReadJournalCandidatesNoFollow(
        WindowsFilesystemHandle root,
        std::vector<WindowsBootstrapJournalCandidate>& output) override {
        std::lock_guard<std::mutex> lock(mutex_);
        output.clear();
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &role)) return InvalidBoundHandle();
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        if (role != WindowsFilesystemRootRole::Transaction) return InvalidRole();
        if (writeRootsAuthorized_) {
            const WindowsFilesystemBackendResult authorized =
                RequireAuthorizedMutation(
                    root, WindowsFilesystemRootRole::Transaction);
            if (!authorized.ok()) return authorized;
        } else {
            // Restart admission needs a read-only probe before a validated
            // plan exists. The probe is restricted to the trusted,
            // production-derived transaction root and cannot yield a writable
            // handle or authorize any mutation primitive.
            const auto recoveryBinding = handles_.find(root);
            std::wstring trustedTransaction;
            std::wstring trustedBase;
            if (!trustedRoots_.authenticated ||
                !trustedRoots_.authenticatedParentProcess.valid ||
                trustedRoots_.expectedUpdaterOwnedBase.empty() ||
                recoveryBinding == handles_.end() ||
                !NormalizeAbsoluteRoot(trustedRoots_.transactionRoot,
                                       trustedTransaction).ok() ||
                !NormalizeAbsoluteRoot(trustedRoots_.expectedUpdaterOwnedBase,
                                       trustedBase).ok() ||
                !IsSameOrDescendantPath(trustedTransaction, trustedBase) ||
                EqualPathInsensitive(trustedTransaction, trustedBase) ||
                !EqualPathExact(recoveryBinding->second.finalPath,
                                trustedTransaction)) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::AccessDenied,
                    ERROR_ACCESS_DENIED);
            }
            const WindowsFilesystemBackendResult transactionSecurity =
                ValidateTrustedStorageSecurity(nativeRoot);
            if (!transactionSecurity.ok()) return transactionSecurity;
            PinnedRootOpen baseRoot;
            const WindowsFilesystemBackendResult baseOpened =
                OpenPinnedAbsoluteRoot(trustedBase, GENERIC_READ, baseRoot);
            if (!baseOpened.ok()) return baseOpened;
            const WindowsFilesystemBackendResult baseSecurity =
                ValidateTrustedStorageSecurity(baseRoot.operation.get());
            if (!baseSecurity.ok()) return baseSecurity;
        }
        if (!journalLeases_.empty()) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_SHARING_VIOLATION);
        }
        const WindowsFilesystemBackendResult namespaceValid =
            ValidateJournalNamespace(nativeRoot);
        if (!namespaceValid.ok()) return namespaceValid;

        struct CandidateName {
            const wchar_t* wide = nullptr;
            const char* narrow = nullptr;
            bool temporary = false;
        };
        const std::array<CandidateName, 4> names = {{
            {kJournalSlotAName, "aris-bootstrap.journal.a", false},
            {kJournalSlotBName, "aris-bootstrap.journal.b", false},
            {kJournalSlotATempName, "aris-bootstrap.journal.a.tmp", true},
            {kJournalSlotBTempName, "aris-bootstrap.journal.b.tmp", true},
        }};
        for (const CandidateName& name : names) {
            NativeOpenResult opened;
            WindowsFilesystemObservation observation;
            const WindowsFilesystemBackendResult observed = OpenObservedRelative(
                nativeRoot, name.wide,
                GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, kFileNonDirectoryFile,
                opened, observation);
            if (observed.status == WindowsFilesystemBackendStatus::NotFound) continue;
            if (!observed.ok()) return observed;
            if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
                return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                      ERROR_REPARSE_TAG_INVALID);
            }
            if (observation.kind != WindowsFilesystemEntryKind::RegularFile ||
                !observation.identity.valid) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::NotRegularFile,
                    ERROR_INVALID_DATA);
            }
            const WindowsFilesystemBackendResult metadataValidated =
                ValidateSupportedMetadata(opened.handle.get());
            if (!metadataValidated.ok()) return metadataValidated;
            WindowsBootstrapJournalCandidate candidate;
            candidate.name = name.narrow;
            candidate.temporary = name.temporary;
            candidate.identity = observation.identity;
            WindowsFilesystemBackendResult read = ReadBytesBounded(
                opened.handle.get(), 4U * 1024U * 1024U, candidate.bytes);
            if (read.ok()) {
                read = FingerprintHandle(opened.handle.get(), candidate.content);
            }
            if (read.ok()) {
                WindowsFileContentFingerprint bytesContent;
                read = FingerprintBytes(candidate.bytes, bytesContent);
                if (read.ok() && !ContentMatches(bytesContent,
                                                 candidate.content)) {
                    read = ContentMismatch();
                }
            }
            if (!read.ok()) return read;
            if (!candidate.temporary) {
                journalSlotIdentities_[root][name.wide] = candidate.identity;
                journalSlotContents_[root][name.wide] = candidate.content;
            }
            output.push_back(std::move(candidate));
        }
        return RevalidateBoundRoot(root);
    }

    WindowsFilesystemBackendResult RetireJournalCandidateDurable(
        const WindowsFilesystemHandle root,
        const WindowsBootstrapJournalCandidate& candidate) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &role)) return InvalidBoundHandle();
        if (role != WindowsFilesystemRootRole::Transaction) return InvalidRole();
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(root,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        if (!journalLeases_.empty()) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_SHARING_VIOLATION);
        }
        const WindowsFilesystemBackendResult runtimePairAbsent =
            RequireRuntimePairMissing(nativeRoot);
        if (!runtimePairAbsent.ok()) return runtimePairAbsent;
        const WindowsFilesystemBackendResult namespaceValid =
            ValidateJournalNamespace(nativeRoot);
        if (!namespaceValid.ok()) return namespaceValid;
        // Journal retirement is a separate destructive path from
        // Apply(RemoveBackup). Recheck that the adapter-owned backup
        // namespace is still absent for every candidate deletion so an
        // interrupted cleanup cannot be silently bypassed.
        const WindowsFilesystemBackendResult backupAbsent =
            ValidateBackupNamespace(nativeRoot, {}, {}, false, {});
        if (!backupAbsent.ok()) return backupAbsent;
        const wchar_t* name = nullptr;
        if (candidate.name == "aris-bootstrap.journal.a") {
            name = kJournalSlotAName;
        } else if (candidate.name == "aris-bootstrap.journal.b") {
            name = kJournalSlotBName;
        } else if (candidate.name == "aris-bootstrap.journal.a.tmp") {
            name = kJournalSlotATempName;
        } else if (candidate.name == "aris-bootstrap.journal.b.tmp") {
            name = kJournalSlotBTempName;
        } else {
            return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                                  ERROR_INVALID_NAME);
        }
        if (!candidate.identity.valid || !candidate.content.valid) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::IdentityMismatch,
                ERROR_INVALID_DATA);
        }
        NativeOpenResult opened;
        const WindowsFilesystemBackendResult bound = OpenExpectedRegularFile(
            nativeRoot, name, candidate.identity,
            GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_DELETE, opened);
        if (!bound.ok()) return bound;
        WindowsFileContentFingerprint observedContent;
        const WindowsFilesystemBackendResult fingerprinted =
            FingerprintHandle(opened.handle.get(), observedContent);
        if (!fingerprinted.ok()) return fingerprinted;
        if (!ContentMatches(observedContent, candidate.content)) {
            return ContentMismatch();
        }
        const WindowsFilesystemBackendResult metadataValidated =
            ValidateSupportedMetadata(opened.handle.get());
        if (!metadataValidated.ok()) return metadataValidated;
        const WindowsFilesystemBackendResult removed =
            DeleteOpenEntryDurable(opened.handle, nativeRoot, name, false,
                                   false, &testHooks_,
                                   &accessDeniedFailureConsumed_);
        if (!removed.ok()) return removed;
        journalSlotIdentities_[root].erase(name);
        journalSlotContents_[root].erase(name);
        const auto activeIdentity = journalIdentities_.find(root);
        if (activeIdentity != journalIdentities_.end() &&
            activeIdentity->second == candidate.identity) {
            journalIdentities_.erase(activeIdentity);
            journalActiveSlots_.erase(root);
        }
        return RevalidateBoundRoot(root);
    }

    WindowsFilesystemBackendResult ValidateBackupNamespaceNoFollow(
        WindowsFilesystemHandle transactionRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries,
        const bool expectDirectory,
        const WindowsFileIdentity& expectedDirectoryIdentity) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(transactionRoot, nativeRoot, &role)) {
            return InvalidBoundHandle();
        }
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(transactionRoot);
        if (!rootValidated.ok()) return rootValidated;
        if (role != WindowsFilesystemRootRole::Transaction) return InvalidRole();
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(transactionRoot,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        return ValidateBackupNamespace(nativeRoot, expectedEntries,
                                       allowedMissingEntries,
                                       expectDirectory,
                                       expectedDirectoryIdentity);
    }

    WindowsFilesystemBackendResult ValidateBackupNamespaceBoundNoFollow(
        WindowsFilesystemHandle backupRoot,
        const std::vector<std::string>& expectedEntries,
        const std::vector<std::string>& allowedMissingEntries) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeBackup = nullptr;
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(backupRoot, nativeBackup, &role)) {
            return InvalidBoundHandle();
        }
        if (role != WindowsFilesystemRootRole::Backup) return InvalidRole();
        const WindowsFilesystemBackendResult rebound =
            RevalidateBoundRoot(backupRoot);
        if (!rebound.ok()) return rebound;
        return ValidateBackupNamespaceBound(nativeBackup, expectedEntries,
                                            allowedMissingEntries);
    }

    WindowsFilesystemBackendResult PromoteJournalCandidateDurable(
        WindowsFilesystemHandle root,
        const WindowsBootstrapJournalCandidate& candidate,
        WindowsFileIdentity& outputIdentity,
        WindowsFileContentFingerprint& outputContent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputIdentity = {};
        outputContent = {};
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &role)) return InvalidBoundHandle();
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        if (role != WindowsFilesystemRootRole::Transaction) return InvalidRole();
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(root,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        if (!journalLeases_.empty()) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_SHARING_VIOLATION);
        }
        const WindowsFilesystemBackendResult namespaceValid =
            ValidateJournalNamespace(nativeRoot);
        if (!namespaceValid.ok()) return namespaceValid;
        const wchar_t* sourceName = nullptr;
        const wchar_t* targetName = nullptr;
        bool temporary = false;
        if (candidate.name == "aris-bootstrap.journal.a") {
            sourceName = kJournalSlotAName;
            targetName = kJournalSlotAName;
        } else if (candidate.name == "aris-bootstrap.journal.b") {
            sourceName = kJournalSlotBName;
            targetName = kJournalSlotBName;
        } else if (candidate.name == "aris-bootstrap.journal.a.tmp") {
            sourceName = kJournalSlotATempName;
            targetName = kJournalSlotAName;
            temporary = true;
        } else if (candidate.name == "aris-bootstrap.journal.b.tmp") {
            sourceName = kJournalSlotBTempName;
            targetName = kJournalSlotBName;
            temporary = true;
        } else {
            return InvalidRelativePath();
        }
        if (!candidate.identity.valid || !candidate.content.valid) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_INVALID_DATA);
        }
        NativeOpenResult opened;
        const WindowsFilesystemBackendResult rebound = OpenExpectedRegularFile(
            nativeRoot, sourceName, candidate.identity,
            GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, opened);
        if (!rebound.ok()) return rebound;
        WindowsFileContentFingerprint currentContent;
        const WindowsFilesystemBackendResult fingerprinted =
            FingerprintHandle(opened.handle.get(), currentContent);
        if (!fingerprinted.ok()) return fingerprinted;
        if (!ContentMatches(currentContent, candidate.content)) {
            return ContentMismatch();
        }
        WindowsFileContentFingerprint bytesContent;
        const WindowsFilesystemBackendResult bytesFingerprinted =
            FingerprintBytes(candidate.bytes, bytesContent);
        if (!bytesFingerprinted.ok()) return bytesFingerprinted;
        if (!ContentMatches(bytesContent, candidate.content)) {
            return ContentMismatch();
        }
        if (temporary) {
            NativeOpenResult replacedSlot;
            WindowsFilesystemObservation replacedObservation;
            const WindowsFilesystemBackendResult replacedObserved =
                OpenObservedRelative(
                    nativeRoot, targetName,
                    GENERIC_READ | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_DELETE,
                    kFileNonDirectoryFile, replacedSlot,
                    replacedObservation);
            bool replaceExisting = false;
            if (replacedObserved.ok()) {
                if (!candidate.replacedSlotIdentity.valid ||
                    !candidate.replacedSlotContent.valid ||
                    replacedObservation.kind !=
                        WindowsFilesystemEntryKind::RegularFile ||
                    replacedObservation.identity !=
                        candidate.replacedSlotIdentity) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_INVALID_DATA);
                }
                WindowsFileContentFingerprint replacedContent;
                const WindowsFilesystemBackendResult replacedFingerprinted =
                    FingerprintHandle(replacedSlot.handle.get(),
                                      replacedContent);
                if (!replacedFingerprinted.ok()) return replacedFingerprinted;
                if (!ContentMatches(replacedContent,
                                    candidate.replacedSlotContent)) {
                    return ContentMismatch();
                }
                replaceExisting = true;
            } else if (replacedObserved.status !=
                           WindowsFilesystemBackendStatus::NotFound) {
                return replacedObserved;
            } else if (candidate.replacedSlotIdentity.valid ||
                       candidate.replacedSlotContent.valid) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::IdentityMismatch,
                    ERROR_FILE_NOT_FOUND);
            }
            const WindowsFilesystemBackendResult renamed = RenameOpenHandle(
                opened.handle.get(), nativeRoot, targetName, replaceExisting);
            if (!renamed.ok()) return renamed;
            const WindowsFilesystemBackendResult flushed =
                FlushDirectory(nativeRoot);
            if (!flushed.ok()) return MakePartialDurable(flushed);
        }
        NativeOpenResult published;
        const WindowsFilesystemBackendResult publishedRebound =
            OpenExpectedRegularFile(
                nativeRoot, targetName, candidate.identity,
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                published);
        if (!publishedRebound.ok()) return MakePartialDurable(publishedRebound);
        journalIdentities_[root] = candidate.identity;
        journalActiveSlots_[root] = targetName;
        journalSlotIdentities_[root][targetName] = candidate.identity;
        journalSlotContents_[root][targetName] = candidate.content;
        outputIdentity = candidate.identity;
        outputContent = candidate.content;
        return {};
    }

    WindowsFilesystemBackendResult AcquireJournalMutationLease(
        WindowsFilesystemHandle root,
        const WindowsFileIdentity& expectedIdentity,
        const WindowsFileContentFingerprint& expectedContent,
        WindowsFilesystemHandle& outputLease) override {
        std::lock_guard<std::mutex> lock(mutex_);
        outputLease = 0;
        HANDLE nativeRoot = nullptr;
        WindowsFilesystemRootRole rootRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(root, nativeRoot, &rootRole)) return InvalidBoundHandle();
        const WindowsFilesystemBackendResult rootValidated =
            RevalidateBoundRoot(root);
        if (!rootValidated.ok()) return rootValidated;
        if (rootRole != WindowsFilesystemRootRole::Transaction) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(root,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        if (!expectedIdentity.valid || !expectedContent.valid) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_INVALID_DATA);
        }
        const WindowsFilesystemBackendResult namespaceValid =
            ValidateJournalNamespace(nativeRoot);
        if (!namespaceValid.ok()) return namespaceValid;
        if (!journalLeases_.empty()) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_SHARING_VIOLATION);
        }

        const wchar_t* activeName = nullptr;
        const auto knownSlot = journalActiveSlots_.find(root);
        if (knownSlot != journalActiveSlots_.end()) {
            activeName = knownSlot->second == kJournalSlotAName
                ? kJournalSlotAName
                : kJournalSlotBName;
        } else {
            for (const wchar_t* candidate : {kJournalSlotAName,
                                             kJournalSlotBName}) {
                NativeOpenResult candidateHandle;
                WindowsFilesystemObservation observation;
                const WindowsFilesystemBackendResult observed =
                    OpenObservedRelative(
                        nativeRoot, candidate, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        0, candidateHandle, observation);
                if (observed.status == WindowsFilesystemBackendStatus::NotFound) {
                    continue;
                }
                if (!observed.ok()) return observed;
                if (observation.kind != WindowsFilesystemEntryKind::RegularFile ||
                    observation.identity != expectedIdentity) {
                    continue;
                }
                if (activeName != nullptr) {
                    return BackendFailure(
                        WindowsFilesystemBackendStatus::IdentityMismatch,
                        ERROR_DUP_NAME);
                }
                activeName = candidate;
            }
        }
        if (activeName == nullptr) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_FILE_NOT_FOUND);
        }
        NativeOpenResult opened;
        const WindowsFilesystemBackendResult rebound = OpenExpectedRegularFile(
            nativeRoot, activeName, expectedIdentity,
            GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, opened);
        if (!rebound.ok()) return rebound;
        const WindowsFilesystemBackendResult metadataValidated =
            ValidateSupportedMetadata(opened.handle.get());
        if (!metadataValidated.ok()) return metadataValidated;
        WindowsFileContentFingerprint currentContent;
        const WindowsFilesystemBackendResult fingerprinted =
            FingerprintHandle(opened.handle.get(), currentContent);
        if (!fingerprinted.ok()) return fingerprinted;
        if (!ContentMatches(currentContent, expectedContent)) {
            return ContentMismatch();
        }

        WindowsFilesystemHandle opaque = nextJournalLeaseToken_++;
        while (opaque == 0 || journalLeases_.find(opaque) != journalLeases_.end()) {
            opaque = nextJournalLeaseToken_++;
        }
        JournalLeaseBinding leaseBinding;
        leaseBinding.root = root;
        leaseBinding.identity = expectedIdentity;
        leaseBinding.content = expectedContent;
        leaseBinding.handle = std::move(opened.handle);
        journalIdentities_[root] = expectedIdentity;
        journalActiveSlots_[root] = activeName;
        journalSlotIdentities_[root][activeName] = expectedIdentity;
        journalSlotContents_[root][activeName] = expectedContent;
        journalLeases_[opaque] = std::move(leaseBinding);
        outputLease = opaque;
        return {};
    }

    void ReleaseJournalMutationLease(
        WindowsFilesystemHandle lease) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = journalLeases_.find(lease);
        if (found == journalLeases_.end()) return;
        journalLeases_.erase(found);
    }

    WindowsFilesystemBackendResult RemoveEmptyDirectoryDurableNoFollow(
        WindowsFilesystemHandle parent,
        const std::string& relativePath,
        const WindowsFileIdentity& expectedIdentity) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE nativeParent = nullptr;
        WindowsFilesystemRootRole parentRole = WindowsFilesystemRootRole::Install;
        if (!BoundHandle(parent, nativeParent, &parentRole)) {
            return InvalidBoundHandle();
        }
        const WindowsFilesystemBackendResult parentValidated =
            RevalidateBoundRoot(parent);
        if (!parentValidated.ok()) return parentValidated;
        if (parentRole != WindowsFilesystemRootRole::Transaction) {
            return InvalidRole();
        }
        const WindowsFilesystemBackendResult authorized =
            RequireAuthorizedMutation(parent,
                                      WindowsFilesystemRootRole::Transaction);
        if (!authorized.ok()) return authorized;
        if (!HasJournalLeaseForRoot(parent)) return MissingJournalLease();
        if (!expectedIdentity.valid) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_INVALID_DATA);
        }

        std::wstring widePath;
        if (!RelativePathToWide(relativePath, widePath)) return InvalidRelativePath();
        ParentAndLeaf target;
        const WindowsFilesystemBackendResult located =
            OpenContainingDirectory(nativeParent, widePath, target);
        if (!located.ok()) return located;

        NativeOpenResult opened;
        WindowsFilesystemObservation observation;
        const WindowsFilesystemBackendResult observed = OpenObservedRelative(
            target.parent.get(), target.leaf,
            DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            kFileDirectoryFile, opened, observation);
        if (!observed.ok()) return observed;
        if (observation.kind == WindowsFilesystemEntryKind::ReparsePoint) {
            return BackendFailure(WindowsFilesystemBackendStatus::ReparsePoint,
                                  ERROR_REPARSE_TAG_INVALID);
        }
        if (observation.kind != WindowsFilesystemEntryKind::Directory) {
            return BackendFailure(WindowsFilesystemBackendStatus::NotDirectory,
                                  ERROR_DIRECTORY);
        }
        if (observation.identity != expectedIdentity) {
            return BackendFailure(WindowsFilesystemBackendStatus::IdentityMismatch,
                                  ERROR_INVALID_DATA);
        }

        const WindowsFilesystemBackendResult marked =
            DeleteOpenEntryDurable(opened.handle, target.parent.get(),
                                   target.leaf, false, true, &testHooks_,
                                   &accessDeniedFailureConsumed_);
        if (!marked.ok()) return marked;
        return {};
    }

    void CloseFilesystemHandle(WindowsFilesystemHandle handle) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = handles_.find(handle);
        if (found == handles_.end()) return;
        for (auto lease = journalLeases_.begin();
             lease != journalLeases_.end();) {
            if (lease->second.root != handle) {
                ++lease;
                continue;
            }
            lease = journalLeases_.erase(lease);
        }
        const HANDLE native = reinterpret_cast<HANDLE>(handle);
        if (native != nullptr && native != INVALID_HANDLE_VALUE) CloseHandle(native);
        handles_.erase(found);
        journalIdentities_.erase(handle);
        journalActiveSlots_.erase(handle);
        journalSlotIdentities_.erase(handle);
        journalSlotContents_.erase(handle);
        if (handles_.empty()) boundInstallVolume_.reset();
    }

private:
    bool AuthorizedPathForRole(
        const std::wstring& normalized,
        const WindowsFilesystemRootRole role) const {
        if (!writeRootsAuthorized_) return false;
        switch (role) {
            case WindowsFilesystemRootRole::Install:
                return EqualPathInsensitive(normalized, authorizedInstallPath_);
            case WindowsFilesystemRootRole::Staging:
                return EqualPathInsensitive(normalized, authorizedStagingPath_);
            case WindowsFilesystemRootRole::Transaction:
                return EqualPathInsensitive(normalized,
                                            authorizedTransactionPath_);
            case WindowsFilesystemRootRole::Backup:
                // Backup handles are capabilities derived from the authorized
                // transaction handle.  An arbitrary absolute Backup path is
                // never a writable root (it may still be opened read-only for
                // diagnostics such as literal-tilde validation).
                return false;
        }
        return false;
    }

    bool AuthorizedIdentityForRole(
        const WindowsFilesystemRootRole role,
        const WindowsFileIdentity& actual) const {
        if (!writeRootsAuthorized_ || !actual.valid) return false;
        switch (role) {
            case WindowsFilesystemRootRole::Install:
                return actual == authorizedInstallIdentity_;
            case WindowsFilesystemRootRole::Staging:
                return actual == authorizedStagingIdentity_;
            case WindowsFilesystemRootRole::Transaction:
                return actual == authorizedTransactionIdentity_;
            case WindowsFilesystemRootRole::Backup:
                return false;
        }
        return false;
    }

    WindowsFilesystemBackendResult RequireAuthorizedMutation(
        WindowsFilesystemHandle handle,
        const WindowsFilesystemRootRole expectedRole) const {
        if (!writeRootsAuthorized_) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
        const auto found = handles_.find(handle);
        if (found == handles_.end() || found->second.role != expectedRole) {
            return InvalidBoundHandle();
        }
        const HandleBinding& binding = found->second;
        if (expectedRole == WindowsFilesystemRootRole::Backup) {
            if (!IsSameOrDescendantPath(binding.finalPath,
                                        authorizedTransactionPath_) ||
                EqualPathInsensitive(binding.finalPath,
                                     authorizedTransactionPath_)) {
                return BackendFailure(
                    WindowsFilesystemBackendStatus::AccessDenied,
                    ERROR_ACCESS_DENIED);
            }
            return {};
        }
        const bool pathAuthorized =
            AuthorizedPathForRole(binding.finalPath, expectedRole);
        const bool identityAuthorized =
            AuthorizedIdentityForRole(expectedRole, binding.identity);
        if (!pathAuthorized || !identityAuthorized) {
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
        return {};
    }

    WindowsFilesystemBackendResult FailureAtTestPoint(
        const WindowsBootstrapFilesystemNativeTestFailurePoint point) {
        if (!accessDeniedFailureConsumed_ &&
            testHooks_.accessDeniedFailurePoint == point) {
            accessDeniedFailureConsumed_ = true;
            return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                                  ERROR_ACCESS_DENIED);
        }
        return {};
    }

    struct HandleBinding {
        WindowsFilesystemRootRole role = WindowsFilesystemRootRole::Install;
        WindowsFileIdentity identity;
        std::wstring finalPath;
        std::vector<ScopedHandle> ancestorPins;
        ScopedHandle rootPin;
    };

    struct JournalLeaseBinding {
        WindowsFilesystemHandle root = 0;
        WindowsFileIdentity identity;
        WindowsFileContentFingerprint content;
        ScopedHandle handle;
    };

    bool BoundHandle(
        WindowsFilesystemHandle handle,
        HANDLE& output,
        WindowsFilesystemRootRole* outputRole = nullptr) const {
        const auto found = handles_.find(handle);
        if (found == handles_.end()) return false;
        output = reinterpret_cast<HANDLE>(handle);
        if (outputRole != nullptr) *outputRole = found->second.role;
        return output != nullptr && output != INVALID_HANDLE_VALUE;
    }

    WindowsFilesystemBackendResult RevalidateBoundRoot(
        WindowsFilesystemHandle handle) const {
        const auto found = handles_.find(handle);
        if (found == handles_.end()) return InvalidBoundHandle();
        const HANDLE native = reinterpret_cast<HANDLE>(handle);
        std::wstring currentPath;
        const WindowsFilesystemBackendResult pathResult =
            FinalPathForHandle(native, currentPath);
        if (!pathResult.ok()) return pathResult;
        if (!EqualPathExact(currentPath, found->second.finalPath)) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::ExactPathMismatch,
                ERROR_INVALID_DATA);
        }
        WindowsFileIdentity currentIdentity;
        const WindowsFilesystemBackendResult identityResult =
            IdentityForHandle(native, currentIdentity);
        if (!identityResult.ok()) return identityResult;
        if (currentIdentity != found->second.identity) {
            return BackendFailure(
                WindowsFilesystemBackendStatus::IdentityMismatch,
                ERROR_INVALID_DATA);
        }
        return {};
    }

    static WindowsFilesystemBackendResult InvalidBoundHandle() {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_INVALID_HANDLE);
    }

    static WindowsFilesystemBackendResult InvalidRelativePath() {
        return BackendFailure(WindowsFilesystemBackendStatus::InvalidPath,
                              ERROR_INVALID_NAME);
    }

    static WindowsFilesystemBackendResult InvalidRole() {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_ACCESS_DENIED);
    }

    bool HasJournalLeaseForRoot(WindowsFilesystemHandle root) const {
        return std::any_of(
            journalLeases_.begin(), journalLeases_.end(),
            [root](const auto& entry) { return entry.second.root == root; });
    }

    static WindowsFilesystemBackendResult MissingJournalLease() {
        return BackendFailure(WindowsFilesystemBackendStatus::AccessDenied,
                              ERROR_SHARING_VIOLATION);
    }

    std::mutex mutex_;
    std::map<WindowsFilesystemHandle, HandleBinding> handles_;
    std::map<WindowsFilesystemHandle, WindowsFileIdentity> journalIdentities_;
    std::map<WindowsFilesystemHandle, std::wstring> journalActiveSlots_;
    std::map<WindowsFilesystemHandle,
             std::map<std::wstring, WindowsFileIdentity>>
        journalSlotIdentities_;
    std::map<WindowsFilesystemHandle,
             std::map<std::wstring, WindowsFileContentFingerprint>>
        journalSlotContents_;
    std::map<WindowsFilesystemHandle, JournalLeaseBinding> journalLeases_;
    WindowsFilesystemHandle nextJournalLeaseToken_ = 1;
    std::optional<std::uint64_t> boundInstallVolume_;
    WindowsBootstrapFilesystemTrustedRootPolicy trustedRoots_;
    WindowsBootstrapFilesystemNativeTestHooks testHooks_;
    bool accessDeniedFailureConsumed_ = false;
    bool writeRootsAuthorized_ = false;
    std::wstring authorizedInstallPath_;
    std::wstring authorizedStagingPath_;
    std::wstring authorizedTransactionPath_;
    WindowsFileIdentity authorizedInstallIdentity_;
    WindowsFileIdentity authorizedStagingIdentity_;
    WindowsFileIdentity authorizedTransactionIdentity_;
};

}  // namespace

bool WindowsBootstrapMandatoryLabelDescriptorsEquivalentForTesting(
    const void* left,
    const void* right) {
    return left != nullptr && right != nullptr &&
        LabelMetadataEquivalent(
            const_cast<PSECURITY_DESCRIPTOR>(left),
            const_cast<PSECURITY_DESCRIPTOR>(right));
}

std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemNativeTestHooks& testHooks) {
    return std::make_unique<NativeWindowsBootstrapFilesystemBackend>(
        WindowsBootstrapFilesystemTrustedRootPolicy{}, testHooks);
}

std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemTrustedRootPolicy& trustedRoots,
    const WindowsBootstrapFilesystemNativeTestHooks& testHooks) {
    return std::make_unique<NativeWindowsBootstrapFilesystemBackend>(
        trustedRoots, testHooks);
}

}  // namespace aris::bootstrap

#else

namespace aris::bootstrap {

std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemNativeTestHooks&) {
    return {};
}

std::unique_ptr<WindowsBootstrapFilesystemBackend>
CreateNativeWindowsBootstrapFilesystemBackend(
    const WindowsBootstrapFilesystemTrustedRootPolicy&,
    const WindowsBootstrapFilesystemNativeTestHooks&) {
    return {};
}

}  // namespace aris::bootstrap

#endif
