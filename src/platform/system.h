#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Platform {

// @stage7-contract
// API contract: process/system helpers operate on caller-owned UTF-8 or wide
// buffers as declared and do not retain them. Handle-returning functions transfer
// the documented borrowed/owned responsibility to the caller; failed queries
// return their established false/status/zero result without partial guarantees.

namespace Internet {
constexpr std::uint32_t kOpenTypePreconfigured = 0;
constexpr std::uint32_t kOptionConnectTimeout = 2;
constexpr std::uint32_t kOptionSendTimeout = 5;
constexpr std::uint32_t kOptionReceiveTimeout = 6;
constexpr std::uint16_t kDefaultHttpsPort = 443;
constexpr std::uint32_t kFlagReload = 0x80000000u;
constexpr std::uint32_t kFlagSecure = 0x00800000u;
constexpr std::uint32_t kFlagNoCacheWrite = 0x04000000u;
constexpr std::uint32_t kQueryStatusCode = 19;
constexpr std::uint32_t kQueryContentLength = 5;
constexpr std::uint32_t kQueryFlagNumber = 0x20000000u;
} // namespace Internet

bool IsSystemDarkMode();
std::string GetMachineId();
std::wstring LegacyHardwareFingerprint();
bool ProtectUserData(std::string_view plaintext, std::string& protectedBytes);
bool UnprotectUserData(std::string_view protectedBytes, std::string& plaintext);
bool StoreGenericCredential(const std::string& targetUtf8, std::string_view value);
bool LoadGenericCredential(const std::string& targetUtf8, std::string& value);
bool DeleteGenericCredential(const std::string& targetUtf8);

void OpenInFileManager(const std::string& pathUtf8);
void OpenUrl(const std::string& url);
namespace MessageBoxFlags {
constexpr unsigned int Ok = 0;
constexpr unsigned int Error = 0x10;
constexpr unsigned int Warning = 0x30;
constexpr unsigned int Information = 0x40;
}
int ShowMessageBox(void* ownerWindow,
                   const wchar_t* message,
                   const wchar_t* title,
                   unsigned int flags);
bool LaunchProcess(const std::string& exePathUtf8, const std::string& argsUtf8 = "");
uint64_t MonotonicMilliseconds();
uint64_t CurrentProcessId();
bool CurrentProcessIsElevated();
uint32_t LastSystemError();
uint32_t GetModuleFileNameBuffer(void* moduleHandle, wchar_t* buffer, uint32_t capacity);
uint32_t GetTempPathBuffer(wchar_t* buffer, uint32_t capacity);
void* OpenCrashFile(const wchar_t* path, bool append);
void OpenCrashReportFolder(const wchar_t* path) noexcept;
void* CurrentProcessHandle();
bool WideToUtf8Buffer(const wchar_t* value, char* buffer, uint32_t capacity);

enum class ProcessMemoryUsageStatus {
    Unsupported,
    Success,
    QueryFailed,
};

struct ProcessMemoryUsageResult {
    ProcessMemoryUsageStatus status = ProcessMemoryUsageStatus::Unsupported;
    uint64_t privateUsageBytes = 0;
};

ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage();

void CloseNativeHandle(void* handle);

using WindowEnumerationCallback = bool (*)(void* windowHandle, void* context);
void EnumerateTopLevelWindows(WindowEnumerationCallback callback, void* context);
void RequestWindowClose(void* windowHandle);
int WindowClassName(void* windowHandle, char* buffer, int capacity);
uint64_t WindowProcessId(void* windowHandle);
uint32_t WindowThreadId(void* windowHandle);
bool WindowIsValid(void* windowHandle);
bool WindowIsVisible(void* windowHandle);
bool WindowSetForeground(void* windowHandle);
bool WindowShow(void* windowHandle, int command);
void SleepMilliseconds(uint32_t milliseconds);
bool WaitForNativeThreadHandle(void* threadHandle, uint32_t timeoutMilliseconds);
bool AcquireNamedMutex(const wchar_t* name, void*& ownedHandle,
                       bool& alreadyExisted, bool& abandoned);
void ReleaseNamedMutex(void* ownedHandle);
void WriteDebuggerOutput(std::string_view text);
bool CloseInternetHandle(void* handle);
enum class ProcessArgumentStatus {
    Unsupported,
    Success,
    NotFound,
    QueryFailed,
};

struct ProcessArgumentResult {
    ProcessArgumentStatus status = ProcessArgumentStatus::Unsupported;
    std::wstring value;
    uint32_t systemError = 0;
};

ProcessArgumentResult ReadProcessArgument(uint32_t index);
void FreeComTaskMemory(void* handle);
void* OpenInternetSession(const wchar_t* agent, uint32_t accessType);
bool SetInternetOption(void* handle, uint32_t option, uint32_t value);
void* ConnectInternetHttp(void* session, const std::wstring& host, uint16_t port);
void* OpenHttpRequest(void* connection,
                      const wchar_t* method,
                      const std::wstring& path,
                      uint32_t flags,
                      const wchar_t* acceptType = nullptr);
bool SendHttpRequest(void* request,
                     const std::wstring& headers,
                     const void* body,
                     uint32_t bodyBytes);
bool QueryHttpInfoNumber(void* request, uint32_t infoLevel, uint32_t& value);
void* OpenPreconfiguredInternetSession(const wchar_t* agent);
bool SetInternetTimeouts(void* handle, uint32_t timeoutMs);
void* ConnectInternetHttps(void* session, const std::wstring& host);
void* OpenSecureJsonPostRequest(void* connection, const std::wstring& path);
void* OpenJsonPostRequest(void* connection, const std::wstring& path, bool secure);
bool QueryHttpStatusCode(void* request, uint32_t& statusCode);
void* OpenInternetUrl(void* session, const std::wstring& url, uint32_t flags);
bool ReadInternetFile(void* request, void* buffer, uint32_t bufferBytes, uint32_t& bytesRead);
void* OpenExclusiveWriteFile(const std::string& pathUtf8);
bool WriteFileBytes(void* fileHandle, const void* bytes, uint32_t byteCount);
void CloseFileHandle(void* fileHandle);
bool CanOpenExclusiveWritePath(const std::string& pathUtf8);

enum class ProcessImagePathStatus {
    Unsupported,
    Success,
    NotFound,
    InvalidArgument,
    QueryFailed,
    EncodingFailed,
};

struct ProcessImagePathResult {
    ProcessImagePathStatus status = ProcessImagePathStatus::Unsupported;
    std::string pathUtf8;
    uint32_t systemError = 0;
};

ProcessImagePathResult ReadProcessImagePath(uint32_t processId);

enum class ProcessImageLookupStatus {
    Unsupported,
    Found,
    NotFound,
    InvalidArgument,
    QueryFailed,
    EncodingFailed,
};

struct ProcessImageLookupResult {
    ProcessImageLookupStatus status = ProcessImageLookupStatus::Unsupported;
    std::string executablePathUtf8;
    unsigned long systemError = 0;
};

ProcessImageLookupResult FindProcessByImageName(
    const std::string& imageNameUtf8,
    const std::string& preferredExecutablePathUtf8 = "");

enum class RegisteredExecutableScope {
    CurrentUser,
    LocalMachine,
};

enum class RegisteredExecutablePathStatus {
    Unsupported,
    Success,
    NotFound,
    InvalidArgument,
    QueryFailed,
    EncodingFailed,
    ValueTooLarge,
};

struct RegisteredExecutablePathResult {
    RegisteredExecutablePathStatus status = RegisteredExecutablePathStatus::Unsupported;
    std::string executablePathUtf8;
    uint32_t systemError = 0;
};

RegisteredExecutablePathResult ReadRegisteredExecutablePath(
    RegisteredExecutableScope scope, const std::string& executableNameUtf8);

enum class RegistryHive {
    CurrentUser,
    LocalMachine,
};

enum class RegistryStringStatus {
    Unsupported,
    Success,
    NotFound,
    InvalidArgument,
    QueryFailed,
    EncodingFailed,
    ValueTooLarge,
};

struct RegistryStringResult {
    RegistryStringStatus status = RegistryStringStatus::Unsupported;
    std::string valueUtf8;
    uint32_t systemError = 0;
};

RegistryStringResult ReadRegistryString(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8);
bool WriteRegistryString(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8,
    const std::string& valueUtf8);
bool DeleteRegistryValue(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8);
void NotifyShellAssociationsChanged();
RegistryStringResult QueryEffectiveFileAssociation(const std::string& extensionUtf8);
bool LaunchDefaultAppsSettings();

enum class EnvironmentStringStatus {
    Unsupported,
    Success,
    NotFound,
    InvalidArgument,
    QueryFailed,
    EncodingFailed,
    ValueTooLarge,
};

struct EnvironmentStringResult {
    EnvironmentStringStatus status = EnvironmentStringStatus::Unsupported;
    std::string valueUtf8;
    uint32_t systemError = 0;
};

EnvironmentStringResult ReadEnvironmentVariable(const std::string& nameUtf8);
EnvironmentStringResult ExpandEnvironmentVariables(const std::string& valueUtf8);

enum class UserDataDirectoryStatus {
    Unsupported,
    Success,
    QueryFailed,
    EncodingFailed,
};

struct UserDataDirectoryResult {
    UserDataDirectoryStatus status = UserDataDirectoryStatus::Unsupported;
    std::string pathUtf8;
    unsigned long systemError = 0;
};

UserDataDirectoryResult GetRoamingAppDataDirectory();

enum class ElevatedProcessWaitStatus {
    Unsupported,
    LaunchCancelled,
    LaunchFailed,
    Exited,
    TimedOut,
    WaitFailed,
    ExitCodeUnavailable,
};

struct ElevatedProcessWaitOptions {
    std::string executableUtf8;
    std::string parametersUtf8;
    bool hideWindow = true;
    uint32_t timeoutMs = 0;
    uint32_t timeoutTerminateExitCode = 1;
};

struct ElevatedProcessWaitResult {
    ElevatedProcessWaitStatus status = ElevatedProcessWaitStatus::Unsupported;
    int exitCode = -1;
    unsigned long systemError = 0;
    bool started = false;
    bool timedOut = false;
    bool terminateAttempted = false;
    bool terminateSucceeded = false;
};

ElevatedProcessWaitResult RunElevatedProcessAndWait(
    const ElevatedProcessWaitOptions& options);

void InitThreadCom();
void UninitThreadCom();
void SetThreadBackgroundPriority();
void SetThreadBackgroundMode(bool enable);

std::string OpenFileDialog(void* ownerWindow, const std::string& title,
                           const std::string& filter = "");
std::string SaveFileDialog(void* ownerWindow, const std::string& title,
                           const std::string& filter = "");
std::string BrowseFolderDialog(void* ownerWindow, const std::string& title = "",
                               const std::string& initialPathUtf8 = "");
std::string BrowseFolderDialogModern(void* ownerWindow, const std::string& title);

std::string GetAppDataDir();
std::string GetExeDir();
std::string GetTempDirectory();
std::string GetTempDragDirectory();

} // namespace Platform
