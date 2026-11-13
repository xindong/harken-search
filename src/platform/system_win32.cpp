#ifdef _WIN32

#include "system.h"
#include "platform/harken_user_data_layout.h"
#include "explorer_shell_path.h"
#include "text_encoding.h"
#include "text_encoding.inl"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <commdlg.h>
#include <combaseapi.h>
#include <intrin.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wininet.h>
#include <wincrypt.h>
#include <wincred.h>
#include <psapi.h>
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Wininet.lib")
#pragma comment(lib, "Shlwapi.lib")

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace Platform {

std::wstring LegacyHardwareFingerprint() {
    std::wstring rawId;
    DWORD volumeSerial = 0;
    if (GetVolumeInformationW(L"C:\\", nullptr, 0, &volumeSerial,
                              nullptr, nullptr, nullptr, 0)) {
        wchar_t buffer[32];
        swprintf_s(buffer, 32, L"%08X", volumeSerial);
        rawId += buffer;
    }
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    wchar_t cpuBuffer[64];
    swprintf_s(cpuBuffer, 64, L"%08X%08X", cpuInfo[0], cpuInfo[3]);
    rawId += cpuBuffer;
    return rawId;
}

bool ProtectUserData(std::string_view plaintext, std::string& protectedBytes) {
    protectedBytes.clear();
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    protectedBytes.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool UnprotectUserData(std::string_view protectedBytes, std::string& plaintext) {
    plaintext.clear();
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(protectedBytes.size());
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(protectedBytes.data()));
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;
    plaintext.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool StoreGenericCredential(const std::string& targetUtf8, std::string_view value) {
    if (targetUtf8.empty()) return false;
    const std::wstring target = TextEncoding::Utf8ToWide(targetUtf8);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) == TRUE;
}

bool LoadGenericCredential(const std::string& targetUtf8, std::string& value) {
    value.clear();
    if (targetUtf8.empty()) return false;
    const std::wstring target = TextEncoding::Utf8ToWide(targetUtf8);
    PCREDENTIALW credential = nullptr;
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential) != TRUE) return false;
    const bool hasBlob = credential->CredentialBlob && credential->CredentialBlobSize > 0;
    if (hasBlob) value.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                              credential->CredentialBlobSize);
    CredFree(credential);
    return hasBlob;
}

bool DeleteGenericCredential(const std::string& targetUtf8) {
    if (targetUtf8.empty()) return false;
    const std::wstring target = TextEncoding::Utf8ToWide(targetUtf8);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) == TRUE) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

bool IsSystemDarkMode() {
    HKEY hKey = nullptr;
    DWORD value = 1; // default: light
    DWORD size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(hKey);
    }
    return value == 0;
}

std::string GetMachineId() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        char buf[256] = {};
        DWORD bufLen = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "MachineGuid", nullptr, &type,
                             (LPBYTE)buf, &bufLen) == ERROR_SUCCESS && type == REG_SZ) {
            RegCloseKey(hKey);
            return std::string(buf);
        }
        RegCloseKey(hKey);
    }
    return "unknown-machine";
}

void OpenInFileManager(const std::string& pathUtf8) {
    std::wstring wpath = TextEncoding::Utf8ToWide(pathUtf8);
    if (wpath.empty()) return;
    const std::wstring shellPath = Detail::PrepareExplorerPathForShell(wpath);
    if (shellPath.empty()) return;
    std::error_code ec;
    const std::filesystem::path fsPath(wpath);
    const std::filesystem::path shellFsPath(shellPath);
    const bool exists = std::filesystem::exists(fsPath, ec);
    const bool isDirectory = exists && std::filesystem::is_directory(fsPath, ec);
    if (exists && !isDirectory) {
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(shellPath.c_str())) {
            const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            if (SUCCEEDED(hr)) {
                return;
            }
        }

        const std::wstring cmd = L"/select,\"" + shellPath + L"\"";
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL) > 32) {
            return;
        }
    }

    const std::wstring folderPath = isDirectory ? shellPath : shellFsPath.parent_path().wstring();
    if (!folderPath.empty()) {
        if ((INT_PTR)ShellExecuteW(nullptr, L"explore", folderPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) {
            return;
        }
    }
}

void OpenUrl(const std::string& url) {
    std::wstring wurl = TextEncoding::Utf8ToWide(url);
    ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int ShowMessageBox(void* ownerWindow,
                   const wchar_t* message,
                   const wchar_t* title,
                   unsigned int flags) {
    return MessageBoxW(static_cast<HWND>(ownerWindow), message, title,
                       static_cast<UINT>(flags));
}

bool LaunchProcess(const std::string& exePathUtf8, const std::string& argsUtf8) {
    std::wstring wExe = TextEncoding::Utf8ToWide(exePathUtf8);
    std::wstring wArgs = TextEncoding::Utf8ToWide(argsUtf8);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + wExe + L"\"";
    if (!wArgs.empty()) cmdLine += L" " + wArgs;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS,
                             nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok != FALSE;
}

uint64_t MonotonicMilliseconds() {
    return static_cast<uint64_t>(GetTickCount64());
}

uint64_t CurrentProcessId() {
    return static_cast<uint64_t>(GetCurrentProcessId());
}

bool CurrentProcessIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION info{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(
        token, TokenElevation, &info, sizeof(info), &size) != FALSE &&
        info.TokenIsElevated != FALSE;
    CloseHandle(token);
    return elevated;
}

bool WaitForNativeThreadHandle(void* threadHandle, uint32_t timeoutMilliseconds) {
    if (!threadHandle) return false;
    return WaitForSingleObject(static_cast<HANDLE>(threadHandle),
                               static_cast<DWORD>(timeoutMilliseconds)) == WAIT_OBJECT_0;
}

bool AcquireNamedMutex(const wchar_t* name, void*& ownedHandle,
                       bool& alreadyExisted, bool& abandoned) {
    ownedHandle = nullptr;
    alreadyExisted = false;
    abandoned = false;
    if (!name || !*name) return false;
    HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    if (!handle) return false;
    alreadyExisted = GetLastError() == ERROR_ALREADY_EXISTS;
    const DWORD wait = WaitForSingleObject(handle, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(handle);
        return false;
    }
    abandoned = wait == WAIT_ABANDONED;
    ownedHandle = handle;
    return true;
}

void ReleaseNamedMutex(void* ownedHandle) {
    if (!ownedHandle) return;
    HANDLE handle = static_cast<HANDLE>(ownedHandle);
    ReleaseMutex(handle);
    CloseHandle(handle);
}

uint32_t GetModuleFileNameBuffer(void* moduleHandle, wchar_t* buffer, uint32_t capacity) {
    if (!moduleHandle || !buffer || capacity == 0) return 0;
    return static_cast<uint32_t>(GetModuleFileNameW(
        static_cast<HMODULE>(moduleHandle), buffer, static_cast<DWORD>(capacity)));
}

uint32_t GetTempPathBuffer(wchar_t* buffer, uint32_t capacity) {
    if (!buffer || capacity == 0) return 0;
    if (HarkenUserData::PreviewIsolationActive()) {
        std::wstring temp = HarkenUserData::Utf8ToWidePath(HarkenUserData::PreviewIsolationRootUtf8());
        temp += L"\\temp\\";
        std::wstring dir = temp;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        if (temp.size() + 1 > capacity) return static_cast<uint32_t>(temp.size() + 1);
        for (size_t i = 0; i <= temp.size(); ++i) buffer[i] = temp.c_str()[i];
        return static_cast<uint32_t>(temp.size());
    }
    return static_cast<uint32_t>(GetTempPathW(
        static_cast<DWORD>(capacity), buffer));
}

void* OpenCrashFile(const wchar_t* path, bool append) {
    if (!path || !*path) return nullptr;
    const DWORD access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
    const DWORD share = append ? FILE_SHARE_READ | FILE_SHARE_WRITE : 0;
    const DWORD disposition = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    HANDLE handle = CreateFileW(
        path, access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : static_cast<void*>(handle);
}

void OpenCrashReportFolder(const wchar_t* path) noexcept {
    if (!path || !path[0]) return;
    ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
}

void* CurrentProcessHandle() {
    return static_cast<void*>(GetCurrentProcess());
}

bool WideToUtf8Buffer(const wchar_t* value, char* buffer, uint32_t capacity) {
    if (!value || !buffer || capacity == 0) return false;
    return WideCharToMultiByte(
        CP_UTF8, 0, value, -1, buffer, static_cast<int>(capacity), nullptr, nullptr) > 0;
}

ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return {ProcessMemoryUsageStatus::QueryFailed, 0};
    }
    return {ProcessMemoryUsageStatus::Success,
            static_cast<uint64_t>(counters.PrivateUsage),
            static_cast<uint64_t>(counters.WorkingSetSize)};
}

void CloseNativeHandle(void* handle) {
    if (handle) {
        CloseHandle(static_cast<HANDLE>(handle));
    }
}

uint32_t LastSystemError() {
    return static_cast<uint32_t>(GetLastError());
}

void EnumerateTopLevelWindows(WindowEnumerationCallback callback, void* context) {
    if (!callback) return;
    struct Context {
        WindowEnumerationCallback callback;
        void* userContext;
    } thunkContext{callback, context};
    EnumWindows(
        [](HWND windowHandle, LPARAM rawContext) -> BOOL {
            const auto* current = reinterpret_cast<const Context*>(rawContext);
            return current->callback(static_cast<void*>(windowHandle), current->userContext)
                ? TRUE : FALSE;
        },
        reinterpret_cast<LPARAM>(&thunkContext));
}

void RequestWindowClose(void* windowHandle) {
    if (!windowHandle) return;
    PostMessageW(static_cast<HWND>(windowHandle), WM_CLOSE, 0, 0);
}

int WindowClassName(void* windowHandle, char* buffer, int capacity) {
    if (!windowHandle || !buffer || capacity <= 0) return 0;
    return GetClassNameA(static_cast<HWND>(windowHandle), buffer, capacity);
}

uint64_t WindowProcessId(void* windowHandle) {
    DWORD pid = 0;
    if (!windowHandle) return 0;
    GetWindowThreadProcessId(static_cast<HWND>(windowHandle), &pid);
    return static_cast<uint64_t>(pid);
}

uint32_t WindowThreadId(void* windowHandle) {
    if (!windowHandle) return 0;
    return static_cast<uint32_t>(GetWindowThreadProcessId(static_cast<HWND>(windowHandle), nullptr));
}

bool WindowIsValid(void* windowHandle) {
    if (!windowHandle) return false;
    return IsWindow(static_cast<HWND>(windowHandle)) != FALSE;
}

bool WindowIsVisible(void* windowHandle) {
    if (!windowHandle) return false;
    return IsWindowVisible(static_cast<HWND>(windowHandle)) != FALSE;
}

bool WindowSetForeground(void* windowHandle) {
    if (!windowHandle) return false;
    return SetForegroundWindow(static_cast<HWND>(windowHandle)) != FALSE;
}

bool WindowShow(void* windowHandle, int command) {
    if (!windowHandle) return false;
    return ShowWindow(static_cast<HWND>(windowHandle), command) != FALSE;
}

bool CloseInternetHandle(void* handle) {
    return handle != nullptr && InternetCloseHandle(static_cast<HINTERNET>(handle)) == TRUE;
}

ProcessArgumentResult ReadProcessArgument(uint32_t index) {
    ProcessArgumentResult result;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        result.status = ProcessArgumentStatus::QueryFailed;
        result.systemError = GetLastError();
        return result;
    }
    struct LocalArgvGuard {
        LPWSTR* value;
        ~LocalArgvGuard() { LocalFree(value); }
    } guard{argv};
    if (index >= static_cast<uint32_t>(argc)) {
        result.status = ProcessArgumentStatus::NotFound;
        return result;
    }
    result.value = argv[index] ? argv[index] : L"";
    result.status = ProcessArgumentStatus::Success;
    return result;
}

void FreeComTaskMemory(void* handle) {
    if (handle) {
        CoTaskMemFree(handle);
    }
}

void* OpenInternetSession(const wchar_t* agent, uint32_t accessType) {
    return InternetOpenW(agent, static_cast<DWORD>(accessType), nullptr, nullptr, 0);
}

bool SetInternetOption(void* handle, uint32_t option, uint32_t value) {
    if (!handle) return false;
    DWORD nativeValue = static_cast<DWORD>(value);
    return InternetSetOptionW(
        static_cast<HINTERNET>(handle),
        static_cast<DWORD>(option),
        &nativeValue,
        sizeof(nativeValue)) == TRUE;
}

void* ConnectInternetHttp(void* session, const std::wstring& host, uint16_t port) {
    if (!session || host.empty()) return nullptr;
    return InternetConnectW(
        static_cast<HINTERNET>(session),
        host.c_str(),
        static_cast<INTERNET_PORT>(port),
        nullptr,
        nullptr,
        INTERNET_SERVICE_HTTP,
        0,
        0);
}

void* OpenHttpRequest(void* connection,
                      const wchar_t* method,
                      const std::wstring& path,
                      uint32_t flags,
                      const wchar_t* acceptType) {
    if (!connection || !method || path.empty()) return nullptr;
    const wchar_t* acceptTypes[] = {acceptType, nullptr};
    return HttpOpenRequestW(
        static_cast<HINTERNET>(connection),
        method,
        path.c_str(),
        nullptr,
        nullptr,
        acceptType ? acceptTypes : nullptr,
        static_cast<DWORD>(flags),
        0);
}

bool SendHttpRequest(void* request,
                     const std::wstring& headers,
                     const void* body,
                     uint32_t bodyBytes) {
    if (!request) return false;
    return HttpSendRequestW(
        static_cast<HINTERNET>(request),
        headers.c_str(),
        static_cast<DWORD>(headers.length()),
        const_cast<void*>(body),
        static_cast<DWORD>(bodyBytes)) == TRUE;
}

bool QueryHttpInfoNumber(void* request, uint32_t infoLevel, uint32_t& value) {
    value = 0;
    if (!request) return false;
    DWORD nativeValue = 0;
    DWORD nativeSize = sizeof(nativeValue);
    const BOOL ok = HttpQueryInfoW(
        static_cast<HINTERNET>(request),
        static_cast<DWORD>(infoLevel),
        &nativeValue,
        &nativeSize,
        nullptr);
    value = static_cast<uint32_t>(nativeValue);
    return ok == TRUE;
}

void* OpenPreconfiguredInternetSession(const wchar_t* agent) {
    return OpenInternetSession(agent, INTERNET_OPEN_TYPE_PRECONFIG);
}

bool SetInternetTimeouts(void* handle, uint32_t timeoutMs) {
    const bool connectSet = SetInternetOption(handle, INTERNET_OPTION_CONNECT_TIMEOUT, timeoutMs);
    const bool receiveSet = SetInternetOption(handle, INTERNET_OPTION_RECEIVE_TIMEOUT, timeoutMs);
    const bool sendSet = SetInternetOption(handle, INTERNET_OPTION_SEND_TIMEOUT, timeoutMs);
    return connectSet && receiveSet && sendSet;
}

void* ConnectInternetHttps(void* session, const std::wstring& host) {
    return ConnectInternetHttp(session, host, INTERNET_DEFAULT_HTTPS_PORT);
}

void* OpenSecureJsonPostRequest(void* connection, const std::wstring& path) {
    return OpenHttpRequest(
        connection,
        L"POST",
        path,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD,
        L"application/json");
}

void* OpenJsonPostRequest(void* connection, const std::wstring& path, bool secure) {
    return OpenHttpRequest(
        connection,
        L"POST",
        path,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_AUTO_REDIRECT | (secure ? INTERNET_FLAG_SECURE : 0),
        L"application/json");
}

bool QueryHttpStatusCode(void* request, uint32_t& statusCode) {
    return QueryHttpInfoNumber(
        request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, statusCode);
}

void* OpenInternetUrl(void* session, const std::wstring& url, uint32_t flags) {
    if (!session || url.empty()) return nullptr;
    return InternetOpenUrlW(
        static_cast<HINTERNET>(session),
        url.c_str(),
        nullptr,
        0,
        static_cast<DWORD>(flags),
        0);
}

bool ReadInternetFile(void* request, void* buffer, uint32_t bufferBytes, uint32_t& bytesRead) {
    bytesRead = 0;
    if (!request || !buffer || bufferBytes == 0) return false;
    DWORD nativeBytesRead = 0;
    const BOOL ok = InternetReadFile(
        static_cast<HINTERNET>(request),
        buffer,
        static_cast<DWORD>(bufferBytes),
        &nativeBytesRead);
    bytesRead = static_cast<uint32_t>(nativeBytesRead);
    return ok == TRUE;
}

void* OpenExclusiveWriteFile(const std::string& pathUtf8) {
    if (pathUtf8.empty() || pathUtf8.find('\0') != std::string::npos) return nullptr;
    const std::wstring path = TextEncoding::Utf8ToWidePermissive(std::string_view(pathUtf8.c_str()));
    if (path.empty()) return nullptr;
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    return file == INVALID_HANDLE_VALUE ? nullptr : file;
}

bool WriteFileBytes(void* fileHandle, const void* bytes, uint32_t byteCount) {
    if (!fileHandle || !bytes) return false;
    DWORD bytesWritten = 0;
    return WriteFile(
        static_cast<HANDLE>(fileHandle),
        bytes,
        static_cast<DWORD>(byteCount),
        &bytesWritten,
        nullptr) == TRUE;
}

void CloseFileHandle(void* fileHandle) {
    if (fileHandle) {
        CloseHandle(static_cast<HANDLE>(fileHandle));
    }
}

bool CanOpenExclusiveWritePath(const std::string& pathUtf8) {
    if (pathUtf8.empty() || pathUtf8.find('\0') != std::string::npos) return true;
    const std::wstring path = TextEncoding::Utf8ToWidePermissive(std::string_view(pathUtf8.c_str()));
    if (path.empty()) return true;
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        return true;
    }
    const DWORD err = GetLastError();
    return err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED;
}

namespace {

class ScopedProcessHandle {
public:
    explicit ScopedProcessHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedProcessHandle() {
        if (handle_) CloseHandle(handle_);
    }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

std::wstring LowerPath(const std::wstring& value) {
    std::wstring lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    return lower;
}

} // namespace

ProcessImagePathResult ReadProcessImagePath(uint32_t processId) {
    ProcessImagePathResult result;
    if (processId == 0) {
        result.status = ProcessImagePathStatus::InvalidArgument;
        return result;
    }
    ScopedProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process.get()) {
        result.status = ProcessImagePathStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(GetLastError());
        return result;
    }
    std::wstring path(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) || size == 0) {
        result.status = ProcessImagePathStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(GetLastError());
        return result;
    }
    path.resize(size);
    result.pathUtf8 = TextEncoding::WideToUtf8Strict(path);
    if (result.pathUtf8.empty()) {
        result.status = ProcessImagePathStatus::EncodingFailed;
        return result;
    }
    result.status = ProcessImagePathStatus::Success;
    return result;
}

ProcessImageLookupResult FindProcessByImageName(
    const std::string& imageNameUtf8,
    const std::string& preferredExecutablePathUtf8) {
    ProcessImageLookupResult result;
    if (imageNameUtf8.empty()) {
        result.status = ProcessImageLookupStatus::InvalidArgument;
        return result;
    }
    const std::wstring imageName = TextEncoding::Utf8ToWideStrict(imageNameUtf8);
    if (imageName.empty() || imageName.find_first_of(L"\\/") != std::wstring::npos) {
        result.status = ProcessImageLookupStatus::InvalidArgument;
        return result;
    }
    std::wstring preferred;
    if (!preferredExecutablePathUtf8.empty()) {
        preferred = TextEncoding::Utf8ToWideStrict(preferredExecutablePathUtf8);
        if (preferred.empty()) {
            result.status = ProcessImageLookupStatus::EncodingFailed;
            return result;
        }
        preferred = LowerPath(std::filesystem::path(preferred).lexically_normal().wstring());
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        result.status = ProcessImageLookupStatus::QueryFailed;
        result.systemError = GetLastError();
        return result;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> processIds;
    DWORD firstError = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, imageName.c_str()) == 0) {
                processIds.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
        const DWORD enumerationError = GetLastError();
        if (enumerationError != ERROR_NO_MORE_FILES) firstError = enumerationError;
    } else {
        firstError = GetLastError();
    }
    CloseHandle(snapshot);

    std::string firstPath;
    bool sawEncodingFailure = false;
    for (DWORD processId : processIds) {
        ScopedProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                FALSE, processId));
        if (!process.get()) {
            if (firstError == 0) firstError = GetLastError();
            continue;
        }
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size) || size == 0) {
            if (firstError == 0) firstError = GetLastError();
            continue;
        }
        path.resize(size);
        const std::string pathUtf8 = TextEncoding::WideToUtf8Strict(path);
        if (pathUtf8.empty()) {
            sawEncodingFailure = true;
            continue;
        }
        if (firstPath.empty()) firstPath = pathUtf8;
        if (!preferred.empty() &&
            LowerPath(std::filesystem::path(path).lexically_normal().wstring()) == preferred) {
            result.status = ProcessImageLookupStatus::Found;
            result.executablePathUtf8 = pathUtf8;
            return result;
        }
    }
    if (!firstPath.empty()) {
        result.status = ProcessImageLookupStatus::Found;
        result.executablePathUtf8 = firstPath;
    } else if (firstError != 0) {
        result.status = ProcessImageLookupStatus::QueryFailed;
        result.systemError = firstError;
    } else if (sawEncodingFailure) {
        result.status = ProcessImageLookupStatus::EncodingFailed;
    } else if (processIds.empty()) {
        result.status = ProcessImageLookupStatus::NotFound;
    } else {
        result.status = ProcessImageLookupStatus::QueryFailed;
    }
    return result;
}

RegisteredExecutablePathResult ReadRegisteredExecutablePath(
    RegisteredExecutableScope scope, const std::string& executableNameUtf8) {
    RegisteredExecutablePathResult result;
    if (executableNameUtf8.empty() || executableNameUtf8.find('\0') != std::string::npos ||
        executableNameUtf8.find_first_of("\\/") != std::string::npos) {
        result.status = RegisteredExecutablePathStatus::InvalidArgument;
        return result;
    }
    const std::wstring executableName = TextEncoding::Utf8ToWideStrict(executableNameUtf8);
    if (executableName.empty()) {
        result.status = RegisteredExecutablePathStatus::EncodingFailed;
        return result;
    }
    HKEY root = nullptr;
    switch (scope) {
    case RegisteredExecutableScope::CurrentUser:
        root = HKEY_CURRENT_USER;
        break;
    case RegisteredExecutableScope::LocalMachine:
        root = HKEY_LOCAL_MACHINE;
        break;
    default:
        result.status = RegisteredExecutablePathStatus::InvalidArgument;
        return result;
    }
    const std::wstring key =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" +
        executableName;
    wchar_t value[32768] = {};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        result.status = RegisteredExecutablePathStatus::NotFound;
        return result;
    }
    if (status == ERROR_MORE_DATA) {
        result.status = RegisteredExecutablePathStatus::ValueTooLarge;
        result.systemError = static_cast<uint32_t>(status);
        return result;
    }
    if (status != ERROR_SUCCESS) {
        result.status = RegisteredExecutablePathStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(status);
        return result;
    }
    result.executablePathUtf8 = TextEncoding::WideToUtf8Strict(value);
    if (result.executablePathUtf8.empty()) {
        result.status = RegisteredExecutablePathStatus::EncodingFailed;
        return result;
    }
    result.status = RegisteredExecutablePathStatus::Success;
    return result;
}

RegistryStringResult ReadRegistryString(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8) {
    RegistryStringResult result;
    if (subkeyUtf8.empty() || subkeyUtf8.find('\0') != std::string::npos ||
        valueNameUtf8.find('\0') != std::string::npos) {
        result.status = RegistryStringStatus::InvalidArgument;
        return result;
    }
    const std::wstring subkey = TextEncoding::Utf8ToWideStrict(subkeyUtf8);
    if (subkey.empty()) {
        result.status = RegistryStringStatus::EncodingFailed;
        return result;
    }
    std::wstring valueName;
    if (!valueNameUtf8.empty()) {
        valueName = TextEncoding::Utf8ToWideStrict(valueNameUtf8);
        if (valueName.empty()) {
            result.status = RegistryStringStatus::EncodingFailed;
            return result;
        }
    }
    HKEY root = nullptr;
    switch (hive) {
    case RegistryHive::CurrentUser: root = HKEY_CURRENT_USER; break;
    case RegistryHive::LocalMachine: root = HKEY_LOCAL_MACHINE; break;
    default:
        result.status = RegistryStringStatus::InvalidArgument;
        return result;
    }
    wchar_t value[32768] = {};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        root, subkey.c_str(), valueName.empty() ? nullptr : valueName.c_str(),
        RRF_RT_REG_SZ, nullptr, value, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        result.status = RegistryStringStatus::NotFound;
        return result;
    }
    if (status == ERROR_MORE_DATA) {
        result.status = RegistryStringStatus::ValueTooLarge;
        result.systemError = static_cast<uint32_t>(status);
        return result;
    }
    if (status != ERROR_SUCCESS) {
        result.status = RegistryStringStatus::QueryFailed;
        result.systemError = static_cast<uint32_t>(status);
        return result;
    }
    if (value[0] == L'\0') {
        result.status = RegistryStringStatus::Success;
        return result;
    }
    result.valueUtf8 = TextEncoding::WideToUtf8Strict(value);
    if (result.valueUtf8.empty()) {
        result.status = RegistryStringStatus::EncodingFailed;
        return result;
    }
    result.status = RegistryStringStatus::Success;
    return result;
}

bool WriteRegistryString(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8,
    const std::string& valueUtf8) {
    if (HarkenUserData::PreviewIsolationActive()) return false;
    if (subkeyUtf8.empty() || subkeyUtf8.find('\0') != std::string::npos ||
        valueNameUtf8.find('\0') != std::string::npos ||
        valueUtf8.find('\0') != std::string::npos) {
        return false;
    }
    const std::wstring subkey = TextEncoding::Utf8ToWideStrict(subkeyUtf8);
    if (subkey.empty()) return false;
    std::wstring valueName;
    if (!valueNameUtf8.empty()) {
        valueName = TextEncoding::Utf8ToWideStrict(valueNameUtf8);
        if (valueName.empty()) return false;
    }
    const std::wstring value = TextEncoding::Utf8ToWideStrict(valueUtf8);
    if (!valueUtf8.empty() && value.empty()) return false;
    HKEY root = nullptr;
    switch (hive) {
    case RegistryHive::CurrentUser: root = HKEY_CURRENT_USER; break;
    case RegistryHive::LocalMachine: root = HKEY_LOCAL_MACHINE; break;
    default: return false;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const bool ok = RegSetValueExW(
        key, valueName.empty() ? nullptr : valueName.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool DeleteRegistryValue(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8) {
    if (subkeyUtf8.empty() || subkeyUtf8.find('\0') != std::string::npos ||
        valueNameUtf8.find('\0') != std::string::npos) {
        return false;
    }
    const std::wstring subkey = TextEncoding::Utf8ToWideStrict(subkeyUtf8);
    if (subkey.empty()) return false;
    std::wstring valueName;
    if (!valueNameUtf8.empty()) {
        valueName = TextEncoding::Utf8ToWideStrict(valueNameUtf8);
        if (valueName.empty()) return false;
    }
    HKEY root = nullptr;
    switch (hive) {
    case RegistryHive::CurrentUser: root = HKEY_CURRENT_USER; break;
    case RegistryHive::LocalMachine: root = HKEY_LOCAL_MACHINE; break;
    default: return false;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = RegDeleteValueW(
        key, valueName.empty() ? nullptr : valueName.c_str()) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

void NotifyShellAssociationsChanged() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

RegistryStringResult QueryEffectiveFileAssociation(const std::string& extensionUtf8) {
    RegistryStringResult result;
    if (extensionUtf8.empty() || extensionUtf8.find('\0') != std::string::npos) {
        result.status = RegistryStringStatus::InvalidArgument;
        return result;
    }
    const std::wstring extension = TextEncoding::Utf8ToWideStrict(extensionUtf8);
    if (extension.empty()) {
        result.status = RegistryStringStatus::EncodingFailed;
        return result;
    }

    IApplicationAssociationRegistration* registrar = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_ApplicationAssociationRegistration,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&registrar));
    if (SUCCEEDED(hr) && registrar) {
        LPWSTR progId = nullptr;
        hr = registrar->QueryCurrentDefault(
            extension.c_str(), AT_FILEEXTENSION, AL_EFFECTIVE, &progId);
        registrar->Release();
        if (SUCCEEDED(hr) && progId) {
            result.valueUtf8 = TextEncoding::WideToUtf8Strict(progId);
            CoTaskMemFree(progId);
            if (result.valueUtf8.empty()) {
                result.status = RegistryStringStatus::EncodingFailed;
                return result;
            }
            result.status = RegistryStringStatus::Success;
            return result;
        }
    }

    wchar_t buffer[256] = {};
    DWORD chars = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    hr = AssocQueryStringW(
        ASSOCF_NONE, ASSOCSTR_PROGID, extension.c_str(), nullptr, buffer, &chars);
    if (hr == S_OK && buffer[0] != L'\0') {
        result.valueUtf8 = TextEncoding::WideToUtf8Strict(buffer);
        if (result.valueUtf8.empty()) {
            result.status = RegistryStringStatus::EncodingFailed;
            return result;
        }
        result.status = RegistryStringStatus::Success;
        return result;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) ||
        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        result.status = RegistryStringStatus::NotFound;
        result.systemError = static_cast<uint32_t>(HRESULT_CODE(hr));
        return result;
    }
    result.status = RegistryStringStatus::QueryFailed;
    result.systemError = static_cast<uint32_t>(HRESULT_CODE(hr));
    return result;
}

bool LaunchDefaultAppsSettings() {
    if (HarkenUserData::PreviewIsolationActive()) {
        return false;
    }
    IApplicationAssociationRegistrationUI* ui = nullptr;
    const HRESULT created = CoCreateInstance(
        CLSID_ApplicationAssociationRegistrationUI,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&ui));
    bool launched = false;
    if (SUCCEEDED(created) && ui) {
        launched = SUCCEEDED(ui->LaunchAdvancedAssociationUI(L"HarkenSearch"));
        ui->Release();
    }
    if (launched) {
        return true;
    }
    OpenUrl("ms-settings:defaultapps");
    return true;
}

EnvironmentStringResult ReadEnvironmentVariable(const std::string& nameUtf8) {
    EnvironmentStringResult result;
    if (nameUtf8.empty() || nameUtf8.find('\0') != std::string::npos) {
        result.status = EnvironmentStringStatus::InvalidArgument;
        return result;
    }
    const std::wstring name = TextEncoding::Utf8ToWideStrict(nameUtf8);
    if (name.empty()) {
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    wchar_t buffer[32768] = {};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(
        name.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) {
        result.systemError = GetLastError();
        if (result.systemError == ERROR_SUCCESS) {
            result.status = EnvironmentStringStatus::Success;
        } else if (result.systemError == ERROR_ENVVAR_NOT_FOUND) {
            result.status = EnvironmentStringStatus::NotFound;
        } else {
            result.status = EnvironmentStringStatus::QueryFailed;
        }
        return result;
    }
    if (length >= std::size(buffer)) {
        result.status = EnvironmentStringStatus::ValueTooLarge;
        result.systemError = ERROR_MORE_DATA;
        return result;
    }
    result.valueUtf8 = TextEncoding::WideToUtf8Strict(
        std::wstring_view(buffer, length));
    if (result.valueUtf8.empty()) {
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    result.status = EnvironmentStringStatus::Success;
    return result;
}

EnvironmentStringResult ExpandEnvironmentVariables(const std::string& valueUtf8) {
    EnvironmentStringResult result;
    if (valueUtf8.empty() || valueUtf8.find('\0') != std::string::npos) {
        result.status = EnvironmentStringStatus::InvalidArgument;
        return result;
    }
    const std::wstring value = TextEncoding::Utf8ToWideStrict(valueUtf8);
    if (value.empty()) {
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    wchar_t buffer[32768] = {};
    SetLastError(ERROR_SUCCESS);
    const DWORD required = ExpandEnvironmentStringsW(
        value.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (required == 0) {
        result.status = EnvironmentStringStatus::QueryFailed;
        result.systemError = GetLastError();
        return result;
    }
    if (required >= std::size(buffer)) {
        result.status = EnvironmentStringStatus::ValueTooLarge;
        result.systemError = ERROR_MORE_DATA;
        return result;
    }
    result.valueUtf8 = TextEncoding::WideToUtf8Strict(
        std::wstring_view(buffer, required - 1));
    if (result.valueUtf8.empty()) {
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    result.status = EnvironmentStringStatus::Success;
    return result;
}

void SleepMilliseconds(uint32_t milliseconds) {
    Sleep(milliseconds);
}

void WriteDebuggerOutput(std::string_view text) {
    const std::string value(text);
    OutputDebugStringA(value.c_str());
}

ElevatedProcessWaitResult RunElevatedProcessAndWait(
    const ElevatedProcessWaitOptions& options) {
    ElevatedProcessWaitResult result;
    if (HarkenUserData::PreviewIsolationActive()) {
        result.status = ElevatedProcessWaitStatus::LaunchCancelled;
        return result;
    }
    const std::wstring executable = TextEncoding::Utf8ToWide(options.executableUtf8);
    const std::wstring parameters = TextEncoding::Utf8ToWide(options.parametersUtf8);

    SHELLEXECUTEINFOW executeInfo = {};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = executable.c_str();
    executeInfo.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    executeInfo.nShow = options.hideWindow ? SW_HIDE : SW_SHOWNORMAL;

    if (!ShellExecuteExW(&executeInfo)) {
        result.systemError = GetLastError();
        result.status = result.systemError == ERROR_CANCELLED
            ? ElevatedProcessWaitStatus::LaunchCancelled
            : ElevatedProcessWaitStatus::LaunchFailed;
        return result;
    }

    result.started = true;
    HANDLE processHandle = executeInfo.hProcess;
    const DWORD waitResult = WaitForSingleObject(
        processHandle, options.timeoutMs == 0 ? INFINITE : options.timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        result.status = ElevatedProcessWaitStatus::TimedOut;
        result.timedOut = true;
        result.terminateAttempted = true;
        result.terminateSucceeded =
            TerminateProcess(processHandle, options.timeoutTerminateExitCode) != FALSE;
        if (!result.terminateSucceeded) result.systemError = GetLastError();
    } else if (waitResult != WAIT_OBJECT_0) {
        result.status = ElevatedProcessWaitStatus::WaitFailed;
        result.systemError = GetLastError();
    } else {
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandle, &exitCode)) {
            result.status = ElevatedProcessWaitStatus::ExitCodeUnavailable;
            result.systemError = GetLastError();
        } else {
            result.status = ElevatedProcessWaitStatus::Exited;
            result.exitCode = static_cast<int>(exitCode);
        }
    }

    CloseHandle(processHandle);
    return result;
}

void InitThreadCom() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (hr == RPC_E_CHANGED_MODE)
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

void UninitThreadCom() {
    CoUninitialize();
}

void SetThreadBackgroundPriority() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

void SetThreadBackgroundMode(bool enable) {
    SetThreadPriority(GetCurrentThread(),
                      enable ? THREAD_MODE_BACKGROUND_BEGIN : THREAD_MODE_BACKGROUND_END);
}

std::string OpenFileDialog(void* ownerWindow, const std::string& title, const std::string& filter) {
    HWND hwnd = static_cast<HWND>(ownerWindow);
    wchar_t szFile[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    std::wstring wTitle = TextEncoding::Utf8ToWide(title);
    ofn.lpstrTitle = wTitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        return TextEncoding::WideToUtf8(szFile);
    }
    return "";
}

std::string SaveFileDialog(void* ownerWindow, const std::string& title, const std::string& filter) {
    HWND hwnd = static_cast<HWND>(ownerWindow);
    wchar_t szFile[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    std::wstring wTitle = TextEncoding::Utf8ToWide(title);
    ofn.lpstrTitle = wTitle.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        return TextEncoding::WideToUtf8(szFile);
    }
    return "";
}

namespace {

std::string BrowseFolderDialogLegacy(void* ownerWindow, const std::string& title) {
    HWND hwnd = static_cast<HWND>(ownerWindow);
    std::wstring wTitle = TextEncoding::Utf8ToWide(title);
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = wTitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = {};
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        return TextEncoding::WideToUtf8(path);
    }
    return "";
}

std::string BrowseFolderDialogModernWithInitialPath(void* ownerWindow,
                                                    const std::string& initialPathUtf8) {
    std::string result;
    const HRESULT hrInit = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool shouldUninit = SUCCEEDED(hrInit);

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(
                options | FOS_PICKFOLDERS | FOS_NOCHANGEDIR | FOS_FORCEFILESYSTEM);
        }

        const std::wstring initialPath = TextEncoding::Utf8ToWide(initialPathUtf8);
        if (!initialPath.empty()) {
            IShellItem* initialItem = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    initialPath.c_str(), nullptr, IID_PPV_ARGS(&initialItem)))) {
                dialog->SetFolder(initialItem);
                initialItem->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(static_cast<HWND>(ownerWindow)))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    result = TextEncoding::WideToUtf8(wpath);
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (shouldUninit) CoUninitialize();
    return result;
}

} // namespace

std::string BrowseFolderDialogModern(void* ownerWindow, const std::string& title) {
    std::string result;
    const HRESULT hrInit = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) {
        return result;
    }
    const bool shouldUninit = SUCCEEDED(hrInit);

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        }
        const std::wstring wTitle = TextEncoding::Utf8ToWide(title);
        dialog->SetTitle(wTitle.c_str());
        if (SUCCEEDED(dialog->Show(static_cast<HWND>(ownerWindow)))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    result = TextEncoding::WideToUtf8(wpath);
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (shouldUninit) CoUninitialize();
    return result;
}

std::string BrowseFolderDialog(void* ownerWindow, const std::string& title,
                               const std::string& initialPathUtf8) {
    if (initialPathUtf8.empty()) {
        return BrowseFolderDialogLegacy(ownerWindow, title);
    }
    return BrowseFolderDialogModernWithInitialPath(ownerWindow, initialPathUtf8);
}

std::string GetAppDataDir() {
    const auto layout = Platform::HarkenUserData::EnsureLayout();
    if (!layout.activeSettingsDir.empty()) {
        return Platform::HarkenUserData::WideToUtf8Path(layout.activeSettingsDir);
    }
    return ".";
}

UserDataDirectoryResult GetRoamingAppDataDirectory() {
    UserDataDirectoryResult result;
    if (HarkenUserData::PreviewIsolationActive()) {
        std::wstring roaming = HarkenUserData::Utf8ToWidePath(HarkenUserData::PreviewIsolationRootUtf8());
        roaming += L"\\profile\\Roaming";
        if (!roaming.empty()) SHCreateDirectoryExW(nullptr, roaming.c_str(), nullptr);
        result.pathUtf8 = HarkenUserData::WideToUtf8Path(roaming);
        result.status = result.pathUtf8.empty() ? UserDataDirectoryStatus::QueryFailed
                                               : UserDataDirectoryStatus::Success;
        return result;
    }
    wchar_t path[MAX_PATH] = {};
    const HRESULT hr = SHGetFolderPathW(
        nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path);
    if (hr != S_OK || path[0] == L'\0') {
        result.status = UserDataDirectoryStatus::QueryFailed;
        result.systemError = static_cast<unsigned long>(hr);
        return result;
    }
    result.pathUtf8 = TextEncoding::WideToUtf8Strict(path);
    if (result.pathUtf8.empty()) {
        result.status = UserDataDirectoryStatus::EncodingFailed;
        return result;
    }
    result.status = UserDataDirectoryStatus::Success;
    return result;
}

std::string GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring wpath(buf);
    auto pos = wpath.find_last_of(L"\\/");
    if (pos != std::wstring::npos) wpath.resize(pos);
    return TextEncoding::WideToUtf8(wpath);
}

std::string GetTempDirectory() {
    wchar_t tempPath[MAX_PATH] = {};
    DWORD n = GetTempPathBuffer(tempPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return TextEncoding::WideToUtf8(tempPath);
}

std::string GetTempDragDirectory() {
    std::string tempDir = GetTempDirectory();
    if (tempDir.empty()) return {};
    std::wstring dragPath = TextEncoding::Utf8ToWide(tempDir);
    dragPath += L"Auro_Drag\\";
    CreateDirectoryW(dragPath.c_str(), nullptr); // Existing directory is success.
    return TextEncoding::WideToUtf8(dragPath);
}

} // namespace Platform

#endif // _WIN32
