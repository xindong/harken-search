#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>

#include "bootstrap/HarkenSearchBootstrapAdmission.h"
#include "nlohmann_json.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using aris::bootstrap::BootstrapProcessIdentity;
using json = nlohmann::json;

bool secureUpdaterDirectory(const std::wstring& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD required = 0U;
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U,
                                          &required));
    if (required == 0U || required > 64U * 1024U) {
        CloseHandle(token);
        return false;
    }
    std::vector<std::uint8_t> userStorage(required);
    const bool userQueried = GetTokenInformation(
        token, TokenUser, userStorage.data(), required, &required) != FALSE;
    CloseHandle(token);
    if (!userQueried) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(userStorage.data());

    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> systemStorage{};
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> adminStorage{};
    DWORD systemSize = static_cast<DWORD>(systemStorage.size());
    DWORD adminSize = static_cast<DWORD>(adminStorage.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemStorage.data(),
                            &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                            adminStorage.data(), &adminSize)) {
        return false;
    }
    std::array<EXPLICIT_ACCESSW, 3U> entries{};
    const std::array<PSID, 3U> sids{
        user->User.Sid, systemStorage.data(), adminStorage.data()};
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        entries[index].grfAccessPermissions = GENERIC_ALL;
        entries[index].grfAccessMode = GRANT_ACCESS;
        entries[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[index].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[index].Trustee.ptstrName =
            reinterpret_cast<LPWSTR>(sids[index]);
    }
    PACL dacl = nullptr;
    const DWORD built = SetEntriesInAclW(
        static_cast<ULONG>(entries.size()), entries.data(), nullptr, &dacl);
    if (built != ERROR_SUCCESS || dacl == nullptr) return false;
    const DWORD applied = SetNamedSecurityInfoW(
        const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        user->User.Sid, nullptr, dacl, nullptr);
    LocalFree(dacl);
    return applied == ERROR_SUCCESS;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(),
                            required) != required) {
        return {};
    }
    return output;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), output.data(),
                            required, nullptr, nullptr) != required) {
        return {};
    }
    return output;
}

std::wstring finalPath(HANDLE handle) {
    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED);
        if (length == 0U) return {};
        if (length < buffer.size() - 1U) return {buffer.data(), length};
        if (buffer.size() > 32768U) return {};
        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring normalizePath(std::wstring value) {
    if (value.rfind(L"\\\\?\\", 0U) == 0U) value.erase(0U, 4U);
    for (wchar_t& character : value) {
        if (character == L'\\') character = L'/';
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    while (value.size() > 1U && value.back() == L'/') value.pop_back();
    return value;
}

std::wstring portablePath(std::wstring value) {
    if (value.rfind(L"\\\\?\\UNC\\", 0U) == 0U) return {};
    if (value.rfind(L"\\\\?\\", 0U) == 0U) value.erase(0U, 4U);
    for (wchar_t& character : value) {
        if (character == L'\\') character = L'/';
    }
    return value;
}

bool captureImageIdentity(HANDLE process, BootstrapProcessIdentity& output) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER creation{};
    creation.LowPart = created.dwLowDateTime;
    creation.HighPart = created.dwHighDateTime;
    std::vector<wchar_t> buffer(512U);
    DWORD length = static_cast<DWORD>(buffer.size());
    while (!QueryFullProcessImageNameW(process, 0U, buffer.data(), &length)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() > 32768U) return false;
        buffer.resize(buffer.size() * 2U);
        length = static_cast<DWORD>(buffer.size());
    }
    const std::wstring requested(buffer.data(), length);
    HANDLE image = CreateFileW(
        requested.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (image == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_ID_INFO id{};
    const bool attributes = GetFileInformationByHandleEx(
        image, FileAttributeTagInfo, &tags, sizeof(tags)) != FALSE;
    const bool identity = GetFileInformationByHandleEx(
        image, FileIdInfo, &id, sizeof(id)) != FALSE;
    const std::wstring canonical = finalPath(image);
    CloseHandle(image);
    if (!attributes || !identity || canonical.empty() ||
        (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        normalizePath(requested) != normalizePath(canonical)) return false;
    output.processId = GetProcessId(process);
    output.creationTime = creation.QuadPart;
    const std::wstring storedPath = portablePath(canonical);
    if (storedPath.empty()) return false;
    output.imagePath = wideToUtf8(storedPath);
    output.imageVolumeSerial = id.VolumeSerialNumber;
    std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier),
              output.imageFileId.begin());
    output.valid = output.processId != 0U && output.creationTime != 0U &&
                   !output.imagePath.empty();
    return output.valid;
}

bool captureFileIdentity(const std::wstring& path,
                         std::uint64_t& volume,
                         std::array<std::uint8_t, 16U>& fileId,
                         std::wstring& canonical) {
    HANDLE image = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (image == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO tags{};
    FILE_ID_INFO identity{};
    const bool observed =
        GetFileInformationByHandleEx(image, FileAttributeTagInfo, &tags,
                                     sizeof(tags)) != FALSE &&
        GetFileInformationByHandleEx(image, FileIdInfo, &identity,
                                     sizeof(identity)) != FALSE;
    canonical = finalPath(image);
    CloseHandle(image);
    if (!observed || canonical.empty() ||
        (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }
    volume = identity.VolumeSerialNumber;
    std::copy(std::begin(identity.FileId.Identifier),
              std::end(identity.FileId.Identifier), fileId.begin());
    return true;
}

json identityJson(const BootstrapProcessIdentity& identity) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string fileId;
    fileId.reserve(identity.imageFileId.size() * 2U);
    for (const std::uint8_t byte : identity.imageFileId) {
        fileId.push_back(digits[byte >> 4U]);
        fileId.push_back(digits[byte & 0x0fU]);
    }
    return {{"creation_time", identity.creationTime},
            {"file_id", fileId},
            {"image_path", identity.imagePath},
            {"image_volume_serial", identity.imageVolumeSerial},
            {"process_id", identity.processId}};
}

json deploymentIdentityJson(
    const std::string& canonicalPath, const std::uint64_t volumeSerial,
    const std::array<std::uint8_t, 16U>& identity) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string fileId;
    for (const auto byte : identity) {
        fileId.push_back(digits[byte >> 4U]);
        fileId.push_back(digits[byte & 0x0fU]);
    }
    return {{"canonical_path", canonicalPath},
            {"file_id", fileId},
            {"volume_serial", volumeSerial}};
}

bool readFile(const std::wstring& path, std::string& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    output.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return !output.empty();
}

std::string quoteForCommandLine(const std::wstring& value) {
    std::string utf8 = wideToUtf8(value);
    return "\"" + utf8 + "\"";
}

bool writeExitCode(const std::wstring& path, const DWORD exitCode) {
    const std::wstring temporary = path + L".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    output << exitCode;
    output.flush();
    if (!output.good()) return false;
    output.close();
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
}

bool createJournalCollision(const std::wstring& transactionRoot) {
    std::wstring path = transactionRoot;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path.append(L"aris-bootstrap.journal.a");
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    constexpr std::array<std::uint8_t, 8U> marker{
        'C', 'O', 'L', 'L', 'I', 'D', 'E', '1'};
    DWORD written = 0U;
    const bool completed =
        WriteFile(file, marker.data(), static_cast<DWORD>(marker.size()),
                  &written, nullptr) != FALSE &&
        written == marker.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return completed;
}

constexpr DWORD kMonitorTimeoutExitCode = 0xE195U;
constexpr DWORD kMonitorWaitMs = 12'000U;
constexpr DWORD kTimeoutOwnershipProbeWaitMs = 250U;
constexpr DWORD kProcessTerminationWaitMs = 2'000U;
constexpr DWORD kMonitorPublicationGraceMs = 2'000U;
constexpr std::wstring_view kTimeoutOwnershipProbeMode =
    L"harness-timeout-ownership";

bool terminateExactProcessAndWait(const HANDLE process,
                                  const DWORD terminationCode,
                                  DWORD& observedExitCode) {
    if (process == nullptr || process == INVALID_HANDLE_VALUE) return false;
    const DWORD initialWait = WaitForSingleObject(process, 0U);
    if (initialWait == WAIT_OBJECT_0) {
        return GetExitCodeProcess(process, &observedExitCode) != FALSE;
    }
    if (initialWait != WAIT_TIMEOUT) return false;
    if (!TerminateProcess(process, terminationCode) &&
        WaitForSingleObject(process, 0U) != WAIT_OBJECT_0) {
        return false;
    }
    if (WaitForSingleObject(process, kProcessTerminationWaitMs) !=
        WAIT_OBJECT_0) {
        return false;
    }
    return GetExitCodeProcess(process, &observedExitCode) != FALSE;
}

int monitorBootstrap(const std::uintptr_t processHandleValue,
                     const std::wstring& resultPath,
                     const DWORD timeoutMs) {
    const HANDLE process = reinterpret_cast<HANDLE>(processHandleValue);
    if (process == nullptr || process == INVALID_HANDLE_VALUE || timeoutMs == 0U) {
        return 101;
    }
    const DWORD wait = WaitForSingleObject(process, timeoutMs);
    DWORD exitCode = 102U;
    if (wait == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(process, &exitCode)) exitCode = 104U;
    } else if (wait == WAIT_TIMEOUT) {
        if (!terminateExactProcessAndWait(
                process, kMonitorTimeoutExitCode, exitCode)) {
            exitCode = 105U;
        }
    } else {
        exitCode = 106U;
    }
    CloseHandle(process);
    return writeExitCode(resultPath, exitCode) ? 0 : 103;
}

bool launchMonitor(const std::wstring& monitorPath,
                   const std::wstring& resultPath,
                   HANDLE bootstrapProcess,
                   const DWORD timeoutMs,
                   HANDLE& monitorProcess) {
    monitorProcess = INVALID_HANDLE_VALUE;
    if (!SetHandleInformation(bootstrapProcess, HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
        std::cerr << "monitor inherit error=" << GetLastError() << '\n';
        return false;
    }
    SIZE_T attributeBytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr, 1U, 0U, &attributeBytes));
    if (attributeBytes == 0U) {
        std::cerr << "monitor attr-size error=" << GetLastError() << '\n';
        return false;
    }
    std::vector<std::uint8_t> storage(attributeBytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U,
                                           &attributeBytes) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0U,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &bootstrapProcess, sizeof(bootstrapProcess),
                                   nullptr, nullptr)) {
        std::cerr << "monitor attr-init error=" << GetLastError() << '\n';
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return false;
    }
    const std::string commandUtf8 = quoteForCommandLine(monitorPath) +
        " --monitor " + std::to_string(
            reinterpret_cast<std::uintptr_t>(bootstrapProcess)) + " " +
        quoteForCommandLine(resultPath) + " " + std::to_string(timeoutMs);
    const std::wstring command = utf8ToWide(commandUtf8);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    PROCESS_INFORMATION info{};
    const BOOL launched = CreateProcessW(
        monitorPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
            CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!launched) {
        std::cerr << "monitor launch error=" << GetLastError() << '\n';
        return false;
    }
    CloseHandle(info.hThread);
    monitorProcess = info.hProcess;
    return true;
}

struct OldProcessFixture {
    HANDLE process = nullptr;
    HANDLE exitEvent = nullptr;
    BootstrapProcessIdentity identity;
};

void closeOldProcess(OldProcessFixture& process) {
    if (process.exitEvent != nullptr && process.exitEvent != INVALID_HANDLE_VALUE) {
        SetEvent(process.exitEvent);
    }
    if (process.process != nullptr && process.process != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(process.process, 5000U);
        CloseHandle(process.process);
    }
    if (process.exitEvent != nullptr && process.exitEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(process.exitEvent);
    }
    process = {};
}

class OldProcessGuard final {
public:
    OldProcessGuard() = default;
    ~OldProcessGuard() { closeOldProcess(process); }
    OldProcessGuard(const OldProcessGuard&) = delete;
    OldProcessGuard& operator=(const OldProcessGuard&) = delete;

    OldProcessFixture process;
};

bool launchOldProcess(const std::wstring& executablePath,
                      OldProcessFixture& output) {
    SECURITY_ATTRIBUTES eventAttributes{};
    eventAttributes.nLength = sizeof(eventAttributes);
    eventAttributes.bInheritHandle = TRUE;
    HANDLE exitEvent = CreateEventW(&eventAttributes, TRUE, FALSE, nullptr);
    if (exitEvent == nullptr) return false;

    SIZE_T attributeBytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr, 1U, 0U, &attributeBytes));
    std::vector<std::uint8_t> storage(attributeBytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (attributeBytes == 0U ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U,
                                           &attributeBytes) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0U,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &exitEvent, sizeof(exitEvent), nullptr,
                                   nullptr)) {
        if (startup.lpAttributeList != nullptr) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
        CloseHandle(exitEvent);
        return false;
    }
    const std::string commandUtf8 = quoteForCommandLine(executablePath) +
        " --old " + std::to_string(
            reinterpret_cast<std::uintptr_t>(exitEvent));
    const std::wstring command = utf8ToWide(commandUtf8);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    PROCESS_INFORMATION info{};
    const BOOL launched = CreateProcessW(
        executablePath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
            CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!launched) {
        CloseHandle(exitEvent);
        return false;
    }
    CloseHandle(info.hThread);
    BootstrapProcessIdentity identity;
    if (!captureImageIdentity(info.hProcess, identity)) {
        SetEvent(exitEvent);
        WaitForSingleObject(info.hProcess, 5000U);
        CloseHandle(info.hProcess);
        CloseHandle(exitEvent);
        return false;
    }
    output.process = info.hProcess;
    output.exitEvent = exitEvent;
    output.identity = std::move(identity);
    return true;
}

bool writeAll(HANDLE handle, const std::string& bytes) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1U << 20U));
        DWORD written = 0U;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0U) return false;
        offset += written;
    }
    return true;
}

void readAvailable(HANDLE handle, std::string& output) {
    for (;;) {
        DWORD available = 0U;
        if (!PeekNamedPipe(handle, nullptr, 0U, nullptr, &available, nullptr) ||
            available == 0U) {
            return;
        }
        std::string chunk(available, '\0');
        DWORD read = 0U;
        if (!ReadFile(handle, chunk.data(), available, &read, nullptr) || read == 0U) {
            return;
        }
        chunk.resize(read);
        output.append(chunk);
    }
}

struct LaunchBrokerFixture final {
    HANDLE channel = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    BootstrapProcessIdentity identity;
};

void closeLaunchBrokerFixture(LaunchBrokerFixture& broker) noexcept {
    if (broker.channel != INVALID_HANDLE_VALUE) CloseHandle(broker.channel);
    if (broker.process != nullptr) CloseHandle(broker.process);
    if (broker.job != nullptr) CloseHandle(broker.job);
    broker = {};
}

bool startLaunchBroker(const std::wstring& brokerExecutablePath,
                       const std::size_t launchIndex,
                       LaunchBrokerFixture& output,
                       std::string& diagnostics) {
    const std::wstring pipeName =
        L"\\\\.\\pipe\\aris-bootstrap-handoff-broker-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" +
        std::to_wstring(launchIndex);
    HANDLE channel = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1U, 64U * 1024U, 64U * 1024U, 0U, nullptr);
    if (channel == INVALID_HANDLE_VALUE) {
        diagnostics = "broker channel setup failed";
        return false;
    }
    SECURITY_ATTRIBUTES inherited{};
    inherited.nLength = sizeof(inherited);
    inherited.bInheritHandle = TRUE;
    HANDLE brokerPipe = CreateFileW(
        pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, &inherited,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (brokerPipe == INVALID_HANDLE_VALUE ||
        (ConnectNamedPipe(channel, nullptr) == FALSE &&
         GetLastError() != ERROR_PIPE_CONNECTED) ||
        !SetHandleInformation(channel, HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
        diagnostics = "broker private pipe connection failed";
        if (brokerPipe != INVALID_HANDLE_VALUE) CloseHandle(brokerPipe);
        CloseHandle(channel);
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (job == nullptr ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)) ||
        !SetHandleInformation(job, HANDLE_FLAG_INHERIT,
                              HANDLE_FLAG_INHERIT)) {
        diagnostics = "broker containment setup failed";
        if (job != nullptr) CloseHandle(job);
        CloseHandle(brokerPipe);
        CloseHandle(channel);
        return false;
    }

    SIZE_T attributeBytes = 0U;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr, 2U, 0U, &attributeBytes));
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = brokerPipe;
    startup.StartupInfo.hStdOutput = brokerPipe;
    startup.StartupInfo.hStdError = brokerPipe;
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    HANDLE brokerJob = job;
    if (attributeBytes == 0U ||
        !InitializeProcThreadAttributeList(startup.lpAttributeList, 2U, 0U,
                                           &attributeBytes) ||
        !UpdateProcThreadAttribute(
            startup.lpAttributeList, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &brokerPipe, sizeof(brokerPipe), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(
            startup.lpAttributeList, 0U, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &brokerJob, sizeof(brokerJob), nullptr, nullptr)) {
        diagnostics = "broker process attributes failed";
        if (startup.lpAttributeList != nullptr) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
        CloseHandle(job);
        CloseHandle(brokerPipe);
        CloseHandle(channel);
        return false;
    }

    std::wstring command =
        L"\"" + brokerExecutablePath + L"\" --aris-bootstrap-launch-broker";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    PROCESS_INFORMATION brokerInfo{};
    const BOOL launched = CreateProcessW(
        brokerExecutablePath.c_str(), mutableCommand.data(), nullptr, nullptr,
        TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &brokerInfo);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(brokerPipe);
    if (!launched) {
        diagnostics = "broker process launch failed";
        CloseHandle(job);
        CloseHandle(channel);
        return false;
    }
    BootstrapProcessIdentity identity;
    const bool admitted = captureImageIdentity(brokerInfo.hProcess, identity) &&
        SetHandleInformation(brokerInfo.hProcess, HANDLE_FLAG_INHERIT,
                             HANDLE_FLAG_INHERIT) &&
        ResumeThread(brokerInfo.hThread) != static_cast<DWORD>(-1);
    CloseHandle(brokerInfo.hThread);
    if (!admitted) {
        diagnostics = "broker exact identity setup failed";
        CloseHandle(job);
        static_cast<void>(WaitForSingleObject(brokerInfo.hProcess, 2000U));
        CloseHandle(brokerInfo.hProcess);
        CloseHandle(channel);
        return false;
    }
    output.channel = channel;
    output.process = brokerInfo.hProcess;
    output.job = job;
    output.identity = std::move(identity);
    return true;
}

int run(const std::wstring& monitorPath,
        const std::wstring& bootstrapPath,
        const std::wstring& packagePath,
        const std::wstring& signaturePath,
        const std::wstring& targetRoot,
        const std::wstring& updaterBase,
        const std::wstring& stagingRoot,
        const std::wstring& transactionRoot,
        const std::wstring& dataRoot,
        const std::wstring& resultPath,
        const DWORD parentHoldMs,
        const std::wstring_view mode,
        const std::wstring& oldExecutablePath) {
    std::string packageBytes;
    std::string signature;
    if (!readFile(packagePath, packageBytes) ||
        !readFile(signaturePath, signature)) return 91;
    if (!secureUpdaterDirectory(updaterBase) ||
        !secureUpdaterDirectory(stagingRoot) ||
        !secureUpdaterDirectory(transactionRoot)) {
        return 95;
    }
    if (mode == L"begin-journal-collision" &&
        !createJournalCollision(transactionRoot)) {
        std::cerr << "journal collision setup failed error="
                  << GetLastError() << '\n';
        return 97;
    }

    BootstrapProcessIdentity parentIdentity;
    if (!captureImageIdentity(GetCurrentProcess(), parentIdentity)) return 94;
    OldProcessGuard oldProcess;
    if (oldExecutablePath.empty() ||
        !launchOldProcess(oldExecutablePath, oldProcess.process)) {
        return 93;
    }
    const BootstrapProcessIdentity& oldIdentity = oldProcess.process.identity;

    std::uint64_t targetRootVolume = 0U;
    std::array<std::uint8_t, 16U> targetRootFileId{};
    std::wstring canonicalTargetRoot;
    if (!captureFileIdentity(targetRoot, targetRootVolume, targetRootFileId,
                             canonicalTargetRoot)) {
        return 96;
    }
    canonicalTargetRoot = portablePath(canonicalTargetRoot);
    if (canonicalTargetRoot.empty()) return 96;

    const std::string target = wideToUtf8(targetRoot);
    const std::string base = wideToUtf8(updaterBase);
    const std::string staging = wideToUtf8(stagingRoot);
    const std::string transaction = wideToUtf8(transactionRoot);
    const std::string data = wideToUtf8(dataRoot);
    const std::string transactionId =
        "blackbox-" + std::to_string(GetCurrentProcessId());
    const std::string invocationId =
        "blackbox-parent-" + std::to_string(GetCurrentProcessId());
    const bool crashMode = mode == L"crash-recovery" ||
                           mode == L"crash-recovery-idempotent";
    const bool commitCrashMode = mode == L"commit-crash-recovery";
    const bool cleanupCrashMode = mode == L"cleanup-crash-recovery";
    const bool releaseCrashMode = mode == L"release-crash-recovery";
    const bool previousRelaunchCrashMode =
        mode == L"previous-relaunch-crash-recovery";
    const std::string nonce(
        64U, crashMode ? 'd'
                       : (previousRelaunchCrashMode ? 'e'
                                                   : (commitCrashMode ? 'f'
                                                      : (cleanupCrashMode ? 'b'
                                                         : (releaseCrashMode ? 'a'
                                                                             : 'c')))));
    const std::string packageDigest =
        aris::bootstrap::BootstrapSha256Hex(packageBytes);
    json invocation = {
        {"deployment_mode", "portable"},
        {"entrypoint", "Aris.exe"},
        {"existing_inventory", json::array({{{"relative_path", "Aris.exe"}}})},
        {"health_nonce", nonce},
        {"installation_id", "portable-blackbox-install"},
        {"invocation_identity", invocationId},
        {"old_process", identityJson(oldIdentity)},
        {"old_executable_identity", deploymentIdentityJson(
            oldIdentity.imagePath, oldIdentity.imageVolumeSerial,
            oldIdentity.imageFileId)},
        {"old_process_id", oldIdentity.processId},
        {"package_plan_digest", packageDigest},
        {"parent_process", identityJson(parentIdentity)},
        {"previous_failure_count", 0U},
        {"protected_data_roots", json::array({data})},
        {"registered_app_id", ""},
        {"staging_root", staging},
        {"target_root", target},
        {"target_root_identity", deploymentIdentityJson(
            wideToUtf8(canonicalTargetRoot), targetRootVolume,
            targetRootFileId)},
        {"transaction_id", transactionId},
        {"transaction_root", transaction},
        {"updater_owned_base", base},
    };
    if (mode == L"identity-substitution") {
        // Keep the live old process unchanged while substituting one byte of
        // its authenticated FileId.  The native OpenExpected boundary must
        // reject this PID-reuse/substitution attempt before filesystem Begin.
        std::string forgedFileId =
            invocation["old_process"]["file_id"].get<std::string>();
        forgedFileId[0] = forgedFileId[0] == '0' ? '1' : '0';
        invocation["old_process"]["file_id"] = forgedFileId;
        invocation["old_executable_identity"]["file_id"] = forgedFileId;
    }
    const json baseHandoff = {
        {"invocation", invocation.dump()},
        {"package_plan", packageBytes},
        {"package_signature", signature},
    };

    std::wstring brokerSubstitutePath;
    std::uint64_t bootstrapVolume = 0U;
    std::uint64_t substituteVolume = 0U;
    std::array<std::uint8_t, 16U> bootstrapFileId{};
    std::array<std::uint8_t, 16U> substituteFileId{};
    std::wstring bootstrapCanonical;
    std::wstring substituteCanonical;
    if (mode == L"broker-identity-substitution") {
        brokerSubstitutePath = updaterBase;
        if (!brokerSubstitutePath.empty() &&
            brokerSubstitutePath.back() != L'\\' &&
            brokerSubstitutePath.back() != L'/') {
            brokerSubstitutePath.push_back(L'\\');
        }
        brokerSubstitutePath += L"HarkenSearchBootstrapBrokerSubstitute.exe";
        if (!CopyFileW(bootstrapPath.c_str(), brokerSubstitutePath.c_str(), TRUE) ||
            !captureFileIdentity(bootstrapPath, bootstrapVolume,
                                 bootstrapFileId, bootstrapCanonical) ||
            !captureFileIdentity(brokerSubstitutePath, substituteVolume,
                                 substituteFileId, substituteCanonical) ||
            normalizePath(bootstrapCanonical) == normalizePath(substituteCanonical) ||
            (bootstrapVolume == substituteVolume &&
             bootstrapFileId == substituteFileId)) {
            std::cerr << "broker substitute setup failed error="
                      << GetLastError() << '\n';
            return 96;
        }
    }

    std::size_t launchIndex = 0U;
    const auto launchOne = [&](const bool recovery,
                               const std::wstring& outputPath,
                               const bool waitForExit,
                               std::string& diagnostics,
                               const bool markerOnly = false) -> bool {
        static_cast<void>(waitForExit);
        const std::wstring pipeName = L"\\\\.\\pipe\\aris-bootstrap-blackbox-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(launchIndex++);
        const bool legacyOneWayChannel =
            mode == L"legacy-one-way-channel";
        HANDLE server = CreateNamedPipeW(
            pipeName.c_str(),
            legacyOneWayChannel ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1U, 2U * 1024U * 1024U,
            2U * 1024U * 1024U, 5000U, nullptr);
        if (server == INVALID_HANDLE_VALUE) {
            diagnostics = "CreateNamedPipe error=" +
                          std::to_string(GetLastError());
            return false;
        }
        HANDLE client = CreateFileW(
            pipeName.c_str(),
            legacyOneWayChannel ? GENERIC_READ
                                : (GENERIC_READ | GENERIC_WRITE),
            0U, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (client == INVALID_HANDLE_VALUE ||
            !SetHandleInformation(client, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            diagnostics = "handoff client setup error=" +
                          std::to_string(GetLastError());
            if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
            CloseHandle(server);
            return false;
        }
        HANDLE parentCapability = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(
                GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                &parentCapability,
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, TRUE, 0U)) {
            diagnostics = "parent capability setup error=" +
                          std::to_string(GetLastError());
            CloseHandle(client);
            CloseHandle(server);
            return false;
        }
        SECURITY_ATTRIBUTES pipeAttributes{};
        pipeAttributes.nLength = sizeof(pipeAttributes);
        pipeAttributes.bInheritHandle = TRUE;
        HANDLE diagnosticRead = INVALID_HANDLE_VALUE;
        HANDLE diagnosticWrite = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&diagnosticRead, &diagnosticWrite, &pipeAttributes, 0U) ||
            !SetHandleInformation(diagnosticRead, HANDLE_FLAG_INHERIT, 0U)) {
            diagnostics = "diagnostic pipe setup error=" +
                          std::to_string(GetLastError());
            if (diagnosticRead != INVALID_HANDLE_VALUE) CloseHandle(diagnosticRead);
            if (diagnosticWrite != INVALID_HANDLE_VALUE) CloseHandle(diagnosticWrite);
            CloseHandle(parentCapability);
            CloseHandle(client);
            CloseHandle(server);
            return false;
        }

        LaunchBrokerFixture launchBroker;
        const std::wstring& brokerExecutablePath =
            brokerSubstitutePath.empty() ? bootstrapPath : brokerSubstitutePath;
        const bool brokerTimeoutMode = mode == L"broker-timeout";
        const bool cutConfigured = !brokerTimeoutMode ||
            SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_TEST_BROKER_CUT",
                                    L"stall-after-create-1") != FALSE;
        const bool brokerStarted = cutConfigured &&
            startLaunchBroker(brokerExecutablePath, launchIndex, launchBroker,
                              diagnostics);
        if (brokerTimeoutMode) {
            static_cast<void>(SetEnvironmentVariableW(
                L"ARIS_BOOTSTRAP_TEST_BROKER_CUT", nullptr));
        }
        if (!brokerStarted) {
            if (!cutConfigured) {
                diagnostics = "broker timeout cut setup failed error=" +
                              std::to_string(GetLastError());
            }
            CloseHandle(diagnosticRead);
            CloseHandle(diagnosticWrite);
            CloseHandle(parentCapability);
            CloseHandle(client);
            CloseHandle(server);
            return false;
        }
        if (mode == L"broker-identity-substitution") {
            const bool distinctBroker =
                normalizePath(utf8ToWide(launchBroker.identity.imagePath)) ==
                    normalizePath(substituteCanonical) &&
                launchBroker.identity.imageVolumeSerial == substituteVolume &&
                launchBroker.identity.imageFileId == substituteFileId &&
                launchBroker.identity.imageFileId != bootstrapFileId;
            if (!distinctBroker) {
                diagnostics = "broker substitute identity was not distinct";
                closeLaunchBrokerFixture(launchBroker);
                return false;
            }
            // Emit this only after a copied executable's canonical path and
            // FileId have been verified against the original image.
            std::cerr << "fixture:broker-substitute=1\n";
        }
        BootstrapProcessIdentity brokerIdentity = launchBroker.identity;
        json launchHandoff = baseHandoff;
        launchHandoff["launch_broker"] = {
            {"channel_handle", reinterpret_cast<std::uintptr_t>(
                                   launchBroker.channel)},
            {"containment_job_handle", reinterpret_cast<std::uintptr_t>(
                                           launchBroker.job)},
            {"launch_timeout_ms", brokerTimeoutMode ? 100U : 5000U},
            {"process_handle", reinterpret_cast<std::uintptr_t>(
                                   launchBroker.process)},
            {"process_identity", identityJson(brokerIdentity)},
        };
        const std::string launchHandoffBytes = launchHandoff.dump();

        SIZE_T attributeBytes = 0U;
        static_cast<void>(InitializeProcThreadAttributeList(
            nullptr, 1U, 0U, &attributeBytes));
        std::vector<std::uint8_t> attributeStorage(attributeBytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = client;
        startup.StartupInfo.hStdOutput = parentCapability;
        startup.StartupInfo.hStdError = diagnosticWrite;
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        std::array<HANDLE, 6> inheritedHandles{
            client, parentCapability, diagnosticWrite, launchBroker.channel,
            launchBroker.process, launchBroker.job};
        if (attributeBytes == 0U ||
            !InitializeProcThreadAttributeList(startup.lpAttributeList, 1U, 0U,
                                               &attributeBytes) ||
            !UpdateProcThreadAttribute(
                startup.lpAttributeList, 0U, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles.data(),
                sizeof(inheritedHandles), nullptr, nullptr)) {
            diagnostics = "handoff attribute setup error=" +
                          std::to_string(GetLastError());
            if (startup.lpAttributeList != nullptr) {
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            }
            CloseHandle(diagnosticRead);
            CloseHandle(diagnosticWrite);
            CloseHandle(parentCapability);
            CloseHandle(client);
            CloseHandle(server);
            closeLaunchBrokerFixture(launchBroker);
            return false;
        }
        std::vector<wchar_t> command(bootstrapPath.begin(), bootstrapPath.end());
        if (recovery) {
            command.push_back(L' ');
            command.insert(command.end(),
                           {L'-', L'-', L'r', L'e', L'c', L'o', L'v', L'e',
                            L'r', L'y'});
        }
        command.push_back(L'\0');
        DWORD clientFlags = 0U;
        DWORD diagnosticFlags = 0U;
        const BOOL clientInfo = GetHandleInformation(client, &clientFlags);
        const BOOL diagnosticInfo =
            GetHandleInformation(diagnosticWrite, &diagnosticFlags);
        PROCESS_INFORMATION bootstrapInfo{};
        const BOOL launched = CreateProcessW(
            bootstrapPath.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
            nullptr, targetRoot.c_str(), &startup.StartupInfo, &bootstrapInfo);
        const DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(client);
        CloseHandle(parentCapability);
        CloseHandle(diagnosticWrite);
        closeLaunchBrokerFixture(launchBroker);
        if (!launched) {
            diagnostics = "bootstrap launch error=" +
                          std::to_string(launchError) +
                          " recovery=" + std::to_string(recovery ? 1 : 0) +
                          " targetAttrs=" +
                          std::to_string(GetFileAttributesW(targetRoot.c_str())) +
                          " bootstrapAttrs=" +
                          std::to_string(GetFileAttributesW(bootstrapPath.c_str())) +
                          " clientInfo=" + std::to_string(clientInfo) +
                          "/" + std::to_string(clientFlags) +
                          " diagnosticInfo=" +
                          std::to_string(diagnosticInfo) + "/" +
                          std::to_string(diagnosticFlags);
            CloseHandle(diagnosticRead);
            CloseHandle(server);
            return false;
        }
        const bool timeoutOwnershipProbe = mode == kTimeoutOwnershipProbeMode;
        const BOOL connected = ConnectNamedPipe(server, nullptr) != FALSE ||
            GetLastError() == ERROR_PIPE_CONNECTED;
        const std::string payload = markerOnly
            ? R"({"recovery":true,"schema":1})"
            : launchHandoffBytes;
        const bool sent = connected && writeAll(server, payload);
        // The production channel is a one-way handoff, not an ACK protocol.
        // A server flush proves that the child consumed the bytes.
        const bool consumed = FlushFileBuffers(server) != FALSE;
        const DWORD consumeError = GetLastError();
        DisconnectNamedPipe(server);
        CloseHandle(server);
        if (!sent || (mode == L"channel-consumed" && !consumed)) {
            diagnostics = "handoff write/consume error=" +
                std::to_string(consumeError);
            CloseHandle(diagnosticRead);
            DWORD ignoredExitCode = 0U;
            static_cast<void>(terminateExactProcessAndWait(
                bootstrapInfo.hProcess, 0xE190U, ignoredExitCode));
            CloseHandle(bootstrapInfo.hThread);
            CloseHandle(bootstrapInfo.hProcess);
            return false;
        }
        // The probe must suspend only after the handoff is durably consumed.
        // Suspending earlier would make FlushFileBuffers wait on the child and
        // test pipe ownership rather than monitor process ownership.
        if (timeoutOwnershipProbe &&
            SuspendThread(bootstrapInfo.hThread) == static_cast<DWORD>(-1)) {
            diagnostics = "timeout ownership probe could not suspend Bootstrap";
            DWORD ignoredExitCode = 0U;
            static_cast<void>(terminateExactProcessAndWait(
                bootstrapInfo.hProcess, 0xE196U, ignoredExitCode));
            CloseHandle(diagnosticRead);
            CloseHandle(bootstrapInfo.hThread);
            CloseHandle(bootstrapInfo.hProcess);
            return false;
        }
        // Transfer the retained Bootstrap process handle to a separate monitor
        // before closing it here. The monitor owns the wait and publishes the
        // exit code while this authenticated parent remains alive.
        HANDLE monitorProcess = INVALID_HANDLE_VALUE;
        const DWORD monitorWaitMs = timeoutOwnershipProbe
            ? kTimeoutOwnershipProbeWaitMs
            : kMonitorWaitMs;
        if (!launchMonitor(monitorPath, outputPath, bootstrapInfo.hProcess,
                           monitorWaitMs, monitorProcess)) {
            diagnostics = "monitor launch failed";
            DWORD ignoredExitCode = 0U;
            static_cast<void>(terminateExactProcessAndWait(
                bootstrapInfo.hProcess, 0xE191U, ignoredExitCode));
            CloseHandle(diagnosticRead);
            CloseHandle(bootstrapInfo.hThread);
            CloseHandle(bootstrapInfo.hProcess);
            return false;
        }
        if (!recovery) {
            // The updater parent remains alive. Release the separate old-app
            // instance only after Bootstrap has retained its exact handle.
            const ULONGLONG bindDeadline = GetTickCount64() + 5000U;
            std::string earlyExit;
            while (!timeoutOwnershipProbe &&
                   diagnostics.find("runtime:old-opened") == std::string::npos &&
                   !readFile(outputPath, earlyExit) &&
                   GetTickCount64() < bindDeadline) {
                readAvailable(diagnosticRead, diagnostics);
                if (diagnostics.find("runtime:old-opened") != std::string::npos) break;
                Sleep(5U);
            }
            if (mode == L"timeout") Sleep(parentHoldMs);
            if (mode == L"progress-normal") Sleep(2200U);
            if (mode == L"progress-timeout") Sleep(7000U);
            const BOOL signaledOld = SetEvent(oldProcess.process.exitEvent);
            const DWORD oldWait = WaitForSingleObject(
                oldProcess.process.process, 5000U);
            if (!signaledOld || oldWait != WAIT_OBJECT_0) {
                diagnostics += "old-process-release failed set=" +
                               std::to_string(signaledOld ? 1 : 0) +
                               " wait=" + std::to_string(oldWait) +
                               " error=" + std::to_string(GetLastError()) +
                               "\n";
                // The fixture owns this exact old-process handle.  If the
                // fixture itself failed to deliver the release event, force
                // termination so a malformed test cannot strand Bootstrap's
                // bounded old-process wait or leak its sandbox.
                if (oldProcess.process.process != nullptr) {
                    DWORD ignoredExitCode = 0U;
                    static_cast<void>(terminateExactProcessAndWait(
                        oldProcess.process.process, 0xE192U, ignoredExitCode));
                }
            }
            // Release the old-app process/event handles before any durable
            // replacement; the identity value is already copied into the
            // authenticated envelope.
            CloseHandle(oldProcess.process.process);
            oldProcess.process.process = nullptr;
            CloseHandle(oldProcess.process.exitEvent);
            oldProcess.process.exitEvent = nullptr;
        }
        // Always wait for the monitor result before returning.  The normal
        // path still releases the old process above before this wait, so the
        // authenticated handoff remains a genuine separate-process test;
        // this only makes fixture ownership deterministic for cleanup.
        {
            const ULONGLONG deadline = GetTickCount64() + monitorWaitMs +
                                       kMonitorPublicationGraceMs;
            std::string publishedExit;
            while (!readFile(outputPath, publishedExit) &&
                   GetTickCount64() < deadline) {
                readAvailable(diagnosticRead, diagnostics);
                Sleep(5U);
            }
            if (publishedExit.empty()) {
                diagnostics += "bootstrap monitor timeout\n";
                DWORD ignoredExitCode = 0U;
                const bool bootstrapStopped = terminateExactProcessAndWait(
                    bootstrapInfo.hProcess, 0xE193U, ignoredExitCode);
                const bool monitorStopped =
                    monitorProcess == INVALID_HANDLE_VALUE ||
                    terminateExactProcessAndWait(
                        monitorProcess, 0xE194U, ignoredExitCode);
                if (!bootstrapStopped || !monitorStopped) {
                    diagnostics += "exact process termination failed\n";
                }
                CloseHandle(diagnosticRead);
                CloseHandle(bootstrapInfo.hThread);
                CloseHandle(bootstrapInfo.hProcess);
                if (monitorProcess != INVALID_HANDLE_VALUE) {
                    CloseHandle(monitorProcess);
                    monitorProcess = INVALID_HANDLE_VALUE;
                }
                return false;
            }
        }
        if (monitorProcess != INVALID_HANDLE_VALUE) {
            const DWORD monitorWait = WaitForSingleObject(
                monitorProcess, kProcessTerminationWaitMs);
            if (monitorWait != WAIT_OBJECT_0) {
                diagnostics += "bootstrap monitor did not terminate\n";
                DWORD ignoredExitCode = 0U;
                static_cast<void>(terminateExactProcessAndWait(
                    monitorProcess, 0xE194U, ignoredExitCode));
                CloseHandle(monitorProcess);
                monitorProcess = INVALID_HANDLE_VALUE;
                CloseHandle(diagnosticRead);
                CloseHandle(bootstrapInfo.hThread);
                CloseHandle(bootstrapInfo.hProcess);
                return false;
            }
            CloseHandle(monitorProcess);
            monitorProcess = INVALID_HANDLE_VALUE;
        }
        if (timeoutOwnershipProbe &&
            WaitForSingleObject(bootstrapInfo.hProcess, 0U) != WAIT_OBJECT_0) {
            diagnostics +=
                "monitor timeout returned while retaining the Bootstrap process\n";
            DWORD ignoredExitCode = 0U;
            static_cast<void>(terminateExactProcessAndWait(
                bootstrapInfo.hProcess, 0xE197U, ignoredExitCode));
            CloseHandle(diagnosticRead);
            CloseHandle(bootstrapInfo.hThread);
            CloseHandle(bootstrapInfo.hProcess);
            return false;
        }
        readAvailable(diagnosticRead, diagnostics);
        CloseHandle(diagnosticRead);
        CloseHandle(bootstrapInfo.hThread);
        CloseHandle(bootstrapInfo.hProcess);
        if (!diagnostics.empty()) std::cerr << diagnostics;
        return true;
    };

    if (mode == L"crash-recovery") {
        const std::wstring firstResult = resultPath + L".crash";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) {
            std::cerr << "crash first launch failed: " << firstDiagnostics << '\n';
            return 101;
        }
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE2U)) {
            std::cerr << "unexpected crash exit=" << firstExitCode << '\n';
            return 104;
        }
        std::cerr << "idempotent first=" << firstExitCode << '\n';
        Sleep(1000U);
        // A crash is expected in the first process. Reopen the same signed
        // handoff through a fresh named pipe and ask a new Bootstrap process to
        // rebind the durable journal and choose rollback/quarantine.
        std::string recoveryDiagnostics;
        if (!launchOne(true, resultPath, true, recoveryDiagnostics)) {
            std::cerr << "crash recovery launch failed: " << recoveryDiagnostics << '\n';
            return 102;
        }
        return DeleteFileW(firstResult.c_str()) != FALSE ? 0 : 105;
    }

    if (mode == L"crash-recovery-idempotent") {
        const std::wstring firstResult = resultPath + L".crash";
        const std::wstring replayResult = resultPath + L".replay";
        const std::wstring terminalResult = resultPath + L".terminal";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) return 111;
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE2U)) {
            return 112;
        }
        // Let the crashed process's job/handle teardown publish its terminal
        // namespace state before the fresh recovery rebinds the journal.
        Sleep(1000U);
        std::string replayDiagnostics;
        if (!launchOne(true, replayResult, true, replayDiagnostics)) {
            std::cerr << "idempotent replay launch failed: " << replayDiagnostics
                      << '\n';
            return 113;
        }
        std::string replayExitCode;
        if (!readFile(replayResult, replayExitCode) || replayExitCode != "60") {
            std::cerr << "idempotent replay exit=" << replayExitCode << '\n';
            return 114;
        }
        std::cerr << "idempotent replay=" << replayExitCode << '\n';
        // Replay cleanup retires the authenticated terminal journal after the
        // rollback and previous-app handoff are durable. Every later recovery
        // must fail closed for lack of a durable transaction, without a
        // second destructive write.
        std::string terminalDiagnostics;
        if (!launchOne(true, terminalResult, true, terminalDiagnostics)) return 115;
        std::string terminalExitCode;
        if (!readFile(terminalResult, terminalExitCode) ||
            terminalExitCode != "75") {
            std::cerr << "idempotent terminal exit=" << terminalExitCode << '\n';
            return 116;
        }
        std::cerr << "idempotent terminal=" << terminalExitCode << '\n';
        std::string finalDiagnostics;
        if (!launchOne(true, resultPath, true, finalDiagnostics)) return 117;
        std::string finalExitCode;
        if (!readFile(resultPath, finalExitCode) || finalExitCode != "75") {
            std::cerr << "idempotent final exit=" << finalExitCode << '\n';
            return 118;
        }
        std::cerr << "idempotent final=" << finalExitCode << '\n';
        DeleteFileW(firstResult.c_str());
        DeleteFileW(replayResult.c_str());
        DeleteFileW(terminalResult.c_str());
        return 0;
    }

    if (mode == L"previous-relaunch-crash-recovery") {
        const std::wstring firstResult = resultPath + L".previous-crash";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) return 121;
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE3U)) {
            std::cerr << "unexpected previous-relaunch crash exit="
                      << firstExitCode << '\n';
            return 122;
        }
        std::string recoveryDiagnostics;
        if (!launchOne(true, resultPath, true, recoveryDiagnostics)) return 123;
        // The relaunched test image is intentionally kept alive through the
        // recovery rebind, then allowed to exit before sandbox cleanup.
        Sleep(4500U);
        DeleteFileW(firstResult.c_str());
        return 0;
    }

    if (mode == L"commit-crash-recovery") {
        const std::wstring firstResult = resultPath + L".commit-crash";
        const std::wstring terminalResult = resultPath + L".commit-terminal";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) return 131;
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE4U)) {
            std::cerr << "unexpected committed crash exit=" << firstExitCode
                      << '\n';
            return 132;
        }
        Sleep(1000U);
        std::string recoveryDiagnostics;
        if (!launchOne(true, resultPath, true, recoveryDiagnostics)) return 133;
        std::string recoveryExitCode;
        if (!readFile(resultPath, recoveryExitCode) || recoveryExitCode != "0") {
            std::cerr << "unexpected committed recovery exit="
                      << recoveryExitCode << '\n';
            return 134;
        }
        std::string terminalDiagnostics;
        if (!launchOne(true, terminalResult, true, terminalDiagnostics)) return 135;
        std::string terminalExitCode;
        if (!readFile(terminalResult, terminalExitCode) ||
            terminalExitCode != "75") {
            std::cerr << "unexpected committed terminal replay exit="
                      << terminalExitCode << '\n';
            return 136;
        }
        const bool removedFirst = DeleteFileW(firstResult.c_str()) != FALSE;
        const bool removedTerminal =
            DeleteFileW(terminalResult.c_str()) != FALSE;
        return removedFirst && removedTerminal ? 0 : 137;
    }

    if (mode == L"cleanup-crash-recovery") {
        const std::wstring firstResult = resultPath + L".cleanup-crash";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) return 141;
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE5U)) {
            std::cerr << "unexpected committed-cleanup crash exit="
                      << firstExitCode << '\n';
            return 142;
        }
        Sleep(1000U);
        std::string recoveryDiagnostics;
        if (!launchOne(true, resultPath, true, recoveryDiagnostics)) return 143;
        DeleteFileW(firstResult.c_str());
        return 0;
    }

    if (mode == L"release-crash-recovery") {
        const std::wstring firstResult = resultPath + L".release-crash";
        std::string firstDiagnostics;
        if (!launchOne(false, firstResult, true, firstDiagnostics)) return 151;
        std::string firstExitCode;
        if (!readFile(firstResult, firstExitCode) ||
            firstExitCode != std::to_string(0xE6U)) {
            std::cerr << "unexpected committed-release crash exit="
                      << firstExitCode << '\n';
            return 152;
        }
        std::string recoveryDiagnostics;
        if (!launchOne(true, resultPath, true, recoveryDiagnostics)) return 153;
        DeleteFileW(firstResult.c_str());
        return 0;
    }

    std::string diagnostics;
    if (!launchOne(false, resultPath, true, diagnostics)) {
        std::cerr << "normal launch failed: " << diagnostics << '\n';
        return 103;
    }
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc == 1) {
        wchar_t delay[16]{};
        const DWORD length = GetEnvironmentVariableW(
            L"ARIS_BOOTSTRAP_RELAUNCHED_SLEEP_MS", delay,
            static_cast<DWORD>(std::size(delay)));
        if (length != 0U && length < std::size(delay)) {
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(delay, &end, 10);
            if (end != delay && *end == L'\0' && parsed != 0UL &&
                parsed <= 10'000UL) {
                Sleep(static_cast<DWORD>(parsed));
                return 0;
            }
        }
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--old") {
        const auto value = std::wcstoull(argv[2], nullptr, 10);
        const HANDLE exitEvent = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(value));
        if (exitEvent == nullptr || exitEvent == INVALID_HANDLE_VALUE) return 106;
        const DWORD waited = WaitForSingleObject(exitEvent, 90'000U);
        CloseHandle(exitEvent);
        return waited == WAIT_OBJECT_0 ? 0 : 107;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--monitor") {
        const auto value = std::wcstoull(argv[2], nullptr, 10);
        const auto timeout = std::wcstoul(argv[4], nullptr, 10);
        return monitorBootstrap(static_cast<std::uintptr_t>(value), argv[3],
                                static_cast<DWORD>(timeout));
    }
    if (argc != 14 || std::wstring_view(argv[1]) != L"--handoff") return 90;
    const DWORD hold = std::wstring_view(argv[12]) == L"timeout" ? 3000U : 250U;
    return run(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], argv[8],
               argv[9], argv[10], argv[11], hold, argv[12], argv[13]);
}
