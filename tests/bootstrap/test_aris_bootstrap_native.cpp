#include "bootstrap/HarkenSearchBootstrapRuntime.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
namespace aris::bootstrap {
bool CaptureBootstrapParentIdentityForTesting(
    std::uintptr_t processHandle, BootstrapProcessIdentity& identity);
}
#endif

namespace {

using namespace aris::bootstrap;
int failures = 0;

void check(const bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

#if defined(_WIN32)
std::filesystem::path sibling(const wchar_t* name) {
    std::wstring module(512U, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, module.data(),
                                                 static_cast<DWORD>(module.size()));
        if (length == 0U) return {};
        if (length < module.size() - 1U) {
            module.resize(length);
            return std::filesystem::path(module).parent_path() / name;
        }
        module.resize(module.size() * 2U);
        if (module.size() > 32768U) return {};
    }
}

bool signedExpectation(const std::filesystem::path& path,
                       BootstrapExecutableExpectation& expectation,
                       DWORD& nativeError) {
    nativeError = ERROR_SUCCESS;
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        nativeError = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    if (!ok) nativeError = GetLastError();
    if (ok && CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                                   CRYPT_VERIFYCONTEXT) == FALSE) {
        nativeError = GetLastError();
        ok = false;
    }
    if (ok && CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) == FALSE) {
        nativeError = GetLastError();
        ok = false;
    }

    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (ok) {
        DWORD read = 0U;
        if (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                     &read, nullptr) == FALSE) {
            nativeError = GetLastError();
            ok = false;
            break;
        }
        if (read == 0U) break;
        if (CryptHashData(hash, buffer.data(), read, 0U) == FALSE) {
            nativeError = GetLastError();
            ok = false;
        }
    }

    std::array<std::uint8_t, 32U> digest{};
    DWORD digestLength = static_cast<DWORD>(digest.size());
    if (ok &&
        (CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestLength, 0U) ==
             FALSE ||
         digestLength != static_cast<DWORD>(digest.size()))) {
        nativeError = GetLastError();
        ok = false;
    }
    if (hash != 0) static_cast<void>(CryptDestroyHash(hash));
    if (provider != 0) static_cast<void>(CryptReleaseContext(provider, 0U));
    CloseHandle(file);
    if (!ok) return false;

    constexpr char hex[] = "0123456789abcdef";
    expectation.absolutePath = path.u8string();
    expectation.byteSize = static_cast<std::uint64_t>(size.QuadPart);
    expectation.sha256.clear();
    expectation.sha256.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        expectation.sha256.push_back(hex[byte >> 4U]);
        expectation.sha256.push_back(hex[byte & 0x0fU]);
    }
    return true;
}

void removeSandboxExactly(const std::filesystem::path& sandbox) {
    std::error_code removeError;
    static_cast<void>(std::filesystem::remove_all(sandbox, removeError));
    std::error_code existsError;
    const bool remains = std::filesystem::exists(sandbox, existsError);
    if (removeError || existsError || remains) {
        std::cerr << "image-pin cleanup path=" << sandbox.u8string()
                  << " remove=" << removeError.value() << ':'
                  << removeError.message()
                  << " exists=" << existsError.value() << ':'
                  << existsError.message() << " remains=" << remains << '\n';
    }
    check(!removeError && !existsError && !remains,
          "image-pin sandbox is removed after all native handles close");
}

void test_exact_old_process_wait_and_substitution() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "native process port and sleep fixture exist");
    if (process == nullptr || fixture.empty()) return;
    const std::string path = fixture.u8string();
    const std::string directory = fixture.parent_path().u8string();
    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"3000") != FALSE,
          "old-process fixture extends its bounded lifetime");
    auto launched = process->Launch(path, directory, {});
    static_cast<void>(
        SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "native launch retains the exact old-process handle");
    if (!launched.ok()) return;
    auto expected = launched.process.identity;
    auto waited = process->WaitForExit(launched.process, 10U);
    check(waited.status == BootstrapProcessStatus::TimedOut,
          "old-process wait is bounded");
    expected.imagePath += "/substituted";
    auto forged = process->OpenExpected(expected);
    check(forged.status == BootstrapProcessStatus::IdentityMismatch ||
              forged.status == BootstrapProcessStatus::NotFound,
          "path substitution cannot reopen an old process");
    check(process->WaitForExit(launched.process, 5000U).ok(),
          "old-process wait can complete on the retained handle");
    process->Close(launched.process);
    const auto absent = process->OpenExpected(expected);
    check(absent.status == BootstrapProcessStatus::NotFound ||
              absent.status == BootstrapProcessStatus::IdentityMismatch,
          "a completed lifetime is absent or rejects a safely detected PID reuse");

    BootstrapProcessIdentity systemProcess = expected;
    systemProcess.processId = 4U;
    systemProcess.creationTime = 1U;
    systemProcess.imagePath = "C:/Windows/System32/ntoskrnl.exe";
    systemProcess.imageVolumeSerial = 1U;
    systemProcess.imageFileId.fill(1U);
    const auto denied = process->OpenExpected(systemProcess);
    check((denied.status == BootstrapProcessStatus::OpenFailed &&
           denied.nativeError == ERROR_ACCESS_DENIED) ||
              denied.status == BootstrapProcessStatus::IdentityMismatch,
          "access denial is fail-closed and never classified as process absence");
}

void test_open_expected_pins_old_image_snapshot_through_wait() {
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(!fixture.empty(), "old-image snapshot fixture exists");
    if (fixture.empty()) return;

    const auto sandbox = std::filesystem::current_path() /
        ("aris-bootstrap-old-image-pin-" +
         std::to_string(GetCurrentProcessId()) + '-' +
         std::to_string(GetTickCount64()));
    std::error_code createError;
    const bool created = std::filesystem::create_directory(sandbox, createError);
    check(created && !createError,
          "unique old-image snapshot sandbox is created");
    if (!created || createError) return;

    {
        const auto oldImage = sandbox / L"Aris.exe";
        const auto displacedImage = sandbox / L"Aris.displaced.exe";
        const bool copied =
            CopyFileW(fixture.c_str(), oldImage.c_str(), TRUE) != FALSE;
        BootstrapExecutableExpectation expectedContent;
        DWORD hashError = ERROR_SUCCESS;
        const bool hashed = copied &&
            signedExpectation(oldImage, expectedContent, hashError);
        check(copied && hashed,
              "old-image content is copied and hashed before process launch");

        auto owner = CreateNativeBootstrapProcessPort();
        auto observer = CreateNativeBootstrapProcessPort();
        check(owner != nullptr && observer != nullptr,
              "independent old-process owner and observer ports exist");
        if (hashed && owner != nullptr && observer != nullptr) {
            check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"1000") != FALSE,
                  "old-image fixture receives a bounded lifetime");
            auto launched = owner->Launch(
                oldImage.u8string(), sandbox.u8string(), {});
            static_cast<void>(
                SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
            check(launched.ok() && launched.process.valid(),
                  "old-image fixture launches with an exact process identity");
            if (launched.ok()) {
                auto opened = observer->OpenExpected(launched.process.identity);
                check(opened.ok() && opened.process.valid() &&
                          opened.executable.valid() &&
                          opened.executable.absolutePath ==
                              launched.process.identity.imagePath &&
                          opened.executable.sha256 == expectedContent.sha256 &&
                          opened.executable.byteSize == expectedContent.byteSize &&
                          opened.executable.exactFileIdentity.valid &&
                          opened.executable.exactFileIdentity.volumeSerial ==
                              launched.process.identity.imageVolumeSerial &&
                          opened.executable.exactFileIdentity.fileId ==
                              launched.process.identity.imageFileId,
                      "OpenExpected returns a path, FileId, and content snapshot from the exact old image handle");
                if (opened.ok()) {
                    check(observer->WaitForExit(opened.process, 5000U).ok(),
                          "old-image observer completes the wait on its retained process handle");
                    SetLastError(ERROR_SUCCESS);
                    const bool renamed = MoveFileExW(
                        oldImage.c_str(), displacedImage.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                    const DWORD renameError = renamed ? ERROR_SUCCESS : GetLastError();
                    check(!renamed &&
                              (renameError == ERROR_SHARING_VIOLATION ||
                               renameError == ERROR_ACCESS_DENIED),
                          "the exact old-image handle blocks rename after exit until Close releases the pin");
                    observer->Close(opened.process);
                }
                check(owner->WaitForExit(launched.process, 5000U).ok(),
                      "old-image owner observes the same terminal lifetime");
                owner->Close(launched.process);
            }
        }
        observer.reset();
        owner.reset();
    }
    removeSandboxExactly(sandbox);
}

void test_old_snapshot_rejects_byte_identical_fileid_substitution_before_resume() {
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(!fixture.empty(), "old-image substitution fixture exists");
    if (fixture.empty()) return;

    const auto sandbox = std::filesystem::current_path() /
        ("aris-bootstrap-old-image-substitution-" +
         std::to_string(GetCurrentProcessId()) + '-' +
         std::to_string(GetTickCount64()));
    std::error_code createError;
    const bool created = std::filesystem::create_directory(sandbox, createError);
    check(created && !createError,
          "unique old-image substitution sandbox is created");
    if (!created || createError) return;

    {
        const auto oldImage = sandbox / L"Aris.exe";
        const auto displacedImage = sandbox / L"Aris.original.exe";
        const bool copied =
            CopyFileW(fixture.c_str(), oldImage.c_str(), TRUE) != FALSE;
        auto owner = CreateNativeBootstrapProcessPort();
        auto observer = CreateNativeBootstrapProcessPort();
        BootstrapExecutableExpectation oldSnapshot;
        check(copied && owner != nullptr && observer != nullptr,
              "old-image substitution prerequisites are available");
        if (copied && owner != nullptr && observer != nullptr) {
            check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"500") != FALSE,
                  "old-image substitution fixture receives a bounded lifetime");
            auto launched = owner->Launch(
                oldImage.u8string(), sandbox.u8string(), {});
            static_cast<void>(
                SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
            check(launched.ok(),
                  "old-image substitution fixture launches before snapshot capture");
            if (launched.ok()) {
                auto opened = observer->OpenExpected(launched.process.identity);
                check(opened.ok() && opened.executable.valid() &&
                          opened.executable.exactFileIdentity.valid,
                      "old-image substitution test captures an exact snapshot");
                if (opened.ok()) {
                    oldSnapshot = opened.executable;
                    check(observer->WaitForExit(opened.process, 5000U).ok(),
                          "old-image substitution observer reaches the terminal lifetime");
                    observer->Close(opened.process);
                }
                check(owner->WaitForExit(launched.process, 5000U).ok(),
                      "old-image substitution owner reaches the same terminal lifetime");
                owner->Close(launched.process);
            }
        }
        observer.reset();
        owner.reset();

        const bool displaced = oldSnapshot.valid() &&
            MoveFileExW(oldImage.c_str(), displacedImage.c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE;
        const bool substituted = displaced &&
            CopyFileW(fixture.c_str(), oldImage.c_str(), TRUE) != FALSE;
        check(displaced && substituted,
              "the released old path is replaced with byte-identical new FileId content");

        auto process = CreateNativeBootstrapProcessPort();
        if (substituted && process != nullptr) {
            auto launched = process->LaunchSuspended(
                oldImage.u8string(), sandbox.u8string(), {});
            check(launched.ok() && launched.process.valid() &&
                      launched.process.identity.imageFileId !=
                          oldSnapshot.exactFileIdentity.fileId,
                  "substitution child is suspended from a distinct FileId");
            if (launched.ok()) {
                auto child = launched.process;
                const BootstrapProcessResult rejected =
                    process->ValidateOwnedExecutable(child, oldSnapshot);
                check(rejected.status == BootstrapProcessStatus::IdentityMismatch,
                      "byte-identical path substitution is rejected by exact FileId before resume");
                check(process->TerminateOwned(child, 0xE006U).ok(),
                      "rejected substitution child remains contained for cleanup");
            }
        }
        process.reset();
    }
    removeSandboxExactly(sandbox);
}

void test_terminal_process_rebind_maps_signalled_handle_to_absence() {
    const auto owner = CreateNativeBootstrapProcessPort();
    const auto recovery = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(owner != nullptr && recovery != nullptr && !fixture.empty(),
          "terminal-window fixture has two independent process ports");
    if (owner == nullptr || recovery == nullptr || fixture.empty()) return;
    auto launched = owner->Launch(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    check(launched.ok() && launched.process.valid(),
          "terminal-window fixture retains the original process object");
    if (!launched.ok()) return;
    const BootstrapProcessIdentity expected = launched.process.identity;
    check(owner->WaitForExit(launched.process, 5000U).ok(),
          "terminal-window process exits while its original handle remains open");
    const BootstrapProcessResult oldProcess = recovery->OpenExpected(expected);
    const BootstrapProcessResult ownedChild =
        recovery->RebindOwnedChild(expected);
    check(oldProcess.status == BootstrapProcessStatus::NotFound &&
              ownedChild.status == BootstrapProcessStatus::NotFound,
          "a newly opened signalled lifetime is absence, while live mismatches remain rejected");
    owner->Close(launched.process);
}

void test_released_child_rebind_maps_terminal_job_to_absence() {
    auto owner = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(owner != nullptr && !fixture.empty(),
          "released-child terminal rebind fixture exists");
    if (owner == nullptr || fixture.empty()) return;

    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"1000") != FALSE,
          "released-child fixture receives a bounded lifetime");
    const auto launched = owner->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "released-child fixture starts suspended");
    if (!launched.ok()) return;

    auto child = launched.process;
    const BootstrapProcessIdentity expected = child.identity;
    HANDLE observer = nullptr;
    std::wstring fileId;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const std::uint8_t byte : expected.imageFileId) {
        fileId.push_back(hex[byte >> 4U]);
        fileId.push_back(hex[byte & 0x0fU]);
    }
    const std::wstring recoveryJobName =
        L"Local\\HarkenSearchBootstrap-Tree-v1-" +
        std::to_wstring(expected.processId) + L"-" +
        std::to_wstring(expected.creationTime) + L"-" +
        std::to_wstring(expected.imageVolumeSerial) + L"-" + fileId;
    HANDLE heldJob = OpenJobObjectW(
        JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE | JOB_OBJECT_SET_ATTRIBUTES,
        FALSE, recoveryJobName.c_str());
    check(heldJob != nullptr,
          "released-child test retains the exact recovery job object");
    check(DuplicateHandle(
              GetCurrentProcess(), reinterpret_cast<HANDLE>(child.waitToken),
              GetCurrentProcess(), &observer, SYNCHRONIZE, FALSE, 0U) != FALSE,
          "released-child fixture retains an independent wait handle");
    check(owner->ResumeOwned(child).ok() && owner->PrepareOwnedForCommit(child).ok(),
          "released-child fixture is prepared before ownership release");
    check(owner->ReleaseOwned(child).ok(),
          "released-child fixture disarms ownership after the commit boundary");

    auto recovery = CreateNativeBootstrapProcessPort();
    check(recovery != nullptr,
          "released-child terminal rebind has a fresh process port");
    if (recovery != nullptr && observer != nullptr) {
        check(WaitForSingleObject(observer, 5000U) == WAIT_OBJECT_0,
              "released-child fixture reaches the terminal state");
        const BootstrapProcessResult rebound =
            recovery->RebindOwnedChild(expected);
        check(rebound.status == BootstrapProcessStatus::NotFound,
              "a released job with no live child maps to absence, not substitution");
    }
    if (observer != nullptr) CloseHandle(observer);
    if (heldJob != nullptr) CloseHandle(heldJob);
}

void test_suspended_child_is_bound_to_expected_image_content() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "native process port and sleep fixture exist for child binding");
    if (process == nullptr || fixture.empty()) return;

    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    check(launched.ok() && launched.process.valid(),
          "child binding fixture starts suspended");
    if (!launched.ok()) return;
    auto child = launched.process;
    BootstrapExecutableExpectation wrongContent;
    wrongContent.absolutePath = fixture.u8string();
    wrongContent.sha256 = std::string(64U, '0');
    wrongContent.byteSize = 1U;
    const auto rejected = process->ValidateOwnedExecutable(child, wrongContent);
    check(rejected.status == BootstrapProcessStatus::IdentityMismatch,
          "suspended child content must match the signed inventory before resume");

    BootstrapExecutableExpectation wrongPath = wrongContent;
    wrongPath.absolutePath = (fixture.parent_path() / L"other.exe").u8string();
    const auto pathRejected = process->ValidateOwnedExecutable(child, wrongPath);
    check(pathRejected.status == BootstrapProcessStatus::IdentityMismatch,
          "suspended child path substitution is rejected before resume");
    check(process->TerminateOwned(child, 0xE005U).ok(),
          "rejected child remains owned for deterministic cleanup");
}

void test_suspended_launch_is_job_owned_at_process_creation() {
    auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "atomic-job fixture and native process port exist");
    if (process == nullptr || fixture.empty()) return;

    check(SetEnvironmentVariableW(
              L"ARIS_BOOTSTRAP_TEST_FAIL_AFTER_CHILD_CREATE", L"1") != FALSE,
          "atomic-job crash cut is enabled");
    const BootstrapProcessResult launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(SetEnvironmentVariableW(
        L"ARIS_BOOTSTRAP_TEST_FAIL_AFTER_CHILD_CREATE", nullptr));
    check(launched.status == BootstrapProcessStatus::LaunchFailed &&
              !launched.process.valid(),
          "post-create crash cut cannot return an unowned suspended child");
    if (launched.process.valid()) {
        auto cleanup = launched.process;
        static_cast<void>(process->TerminateOwned(cleanup, 0xE008U));
    }
}

void test_terminate_owned_drains_the_entire_child_job() {
    auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    const auto marker = std::filesystem::current_path() /
        ("aris-bootstrap-descendant-" +
         std::to_string(GetCurrentProcessId()) + ".bin");
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    check(process != nullptr && !fixture.empty(),
          "descendant containment fixture and process port exist");
    if (process == nullptr || fixture.empty()) return;

    const std::wstring markerText = marker.wstring();
    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"10000") !=
              FALSE &&
              SetEnvironmentVariableW(
                  L"ARIS_BOOTSTRAP_DESCENDANT_MARKER",
                  markerText.c_str()) != FALSE,
          "descendant containment environment is configured");
    const BootstrapProcessResult launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(SetEnvironmentVariableW(
        L"ARIS_BOOTSTRAP_DESCENDANT_MARKER", nullptr));
    static_cast<void>(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS",
                                               nullptr));
    check(launched.ok() && launched.process.valid(),
          "descendant containment parent starts suspended");
    if (!launched.ok()) return;
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "descendant containment parent resumes");

    DWORD descendantId = 0U;
    for (DWORD waited = 0U; waited < 5000U && descendantId == 0U;
         waited += 10U) {
        const HANDLE markerHandle = CreateFileW(
            markerText.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (markerHandle != INVALID_HANDLE_VALUE) {
            DWORD read = 0U;
            if (ReadFile(markerHandle, &descendantId, sizeof(descendantId),
                         &read, nullptr) == FALSE ||
                read != sizeof(descendantId)) {
                descendantId = 0U;
            }
            CloseHandle(markerHandle);
        }
        if (descendantId == 0U) Sleep(10U);
    }
    check(descendantId != 0U,
          "descendant publishes its exact process identity marker");

    const BootstrapProcessResult terminated =
        process->TerminateOwned(child, 0xE009U);
    if (!terminated.ok()) {
        std::cerr << "whole-job terminate status="
                  << static_cast<int>(terminated.status)
                  << " native=" << terminated.nativeError << '\n';
    }
    check(terminated.ok(), "whole-job termination succeeds");
    const bool markerReleased = DeleteFileW(markerText.c_str()) != FALSE;
    check(markerReleased,
          "TerminateOwned returns only after descendant handles are drained");
    if (!markerReleased) {
        const HANDLE descendant = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, FALSE, descendantId);
        if (descendant != nullptr) {
            static_cast<void>(TerminateProcess(descendant, 0xE009U));
            static_cast<void>(WaitForSingleObject(descendant, 5000U));
            CloseHandle(descendant);
        }
        std::filesystem::remove(marker, ignored);
    }
}

void test_terminate_owned_drains_descendants_after_root_already_exited() {
    auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    const auto marker = std::filesystem::current_path() /
        ("aris-bootstrap-terminal-root-descendant-" +
         std::to_string(GetCurrentProcessId()) + ".bin");
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    check(process != nullptr && !fixture.empty(),
          "terminal-root descendant fixture and process port exist");
    if (process == nullptr || fixture.empty()) return;

    const std::wstring markerText = marker.wstring();
    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"50") !=
              FALSE &&
              SetEnvironmentVariableW(
                  L"ARIS_BOOTSTRAP_DESCENDANT_MARKER",
                  markerText.c_str()) != FALSE,
          "terminal-root descendant environment is configured");
    const BootstrapProcessResult launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(SetEnvironmentVariableW(
        L"ARIS_BOOTSTRAP_DESCENDANT_MARKER", nullptr));
    static_cast<void>(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS",
                                               nullptr));
    check(launched.ok() && launched.process.valid(),
          "terminal-root parent starts suspended");
    if (!launched.ok()) return;
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(), "terminal-root parent resumes");

    DWORD descendantId = 0U;
    for (DWORD waited = 0U; waited < 5000U && descendantId == 0U;
         waited += 10U) {
        const HANDLE markerHandle = CreateFileW(
            markerText.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (markerHandle != INVALID_HANDLE_VALUE) {
            DWORD read = 0U;
            if (ReadFile(markerHandle, &descendantId, sizeof(descendantId),
                         &read, nullptr) == FALSE ||
                read != sizeof(descendantId)) {
                descendantId = 0U;
            }
            CloseHandle(markerHandle);
        }
        if (descendantId == 0U) Sleep(10U);
    }
    check(descendantId != 0U,
          "terminal-root descendant publishes its process identity marker");

    const DWORD rootWait = WaitForSingleObject(
        reinterpret_cast<HANDLE>(child.waitToken), 5000U);
    check(rootWait == WAIT_OBJECT_0,
          "owned root is signalled while its descendant remains live");
    const BootstrapProcessResult terminated =
        process->TerminateOwned(child, 0xE00AU);
    if (!terminated.ok()) {
        std::cerr << "terminal-root whole-job terminate status="
                  << static_cast<int>(terminated.status)
                  << " native=" << terminated.nativeError << '\n';
    }
    check(terminated.ok(), "terminal-root whole-job termination succeeds");
    const bool markerReleased = DeleteFileW(markerText.c_str()) != FALSE;
    check(markerReleased,
          "TerminateOwned drains descendants after the root is already signalled");
    if (!markerReleased) {
        const HANDLE descendant = OpenProcess(
            PROCESS_TERMINATE | SYNCHRONIZE, FALSE, descendantId);
        if (descendant != nullptr) {
            static_cast<void>(TerminateProcess(descendant, 0xE00AU));
            static_cast<void>(WaitForSingleObject(descendant, 5000U));
            CloseHandle(descendant);
        }
        std::filesystem::remove(marker, ignored);
    }
}

void test_launch_suspended_pins_image_until_signed_validation() {
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(!fixture.empty(), "image-pin sleep fixture exists");
    if (fixture.empty()) return;

    const auto sandbox = std::filesystem::current_path() /
        ("aris-bootstrap-native-image-pin-" +
         std::to_string(GetCurrentProcessId()) + '-' +
         std::to_string(GetTickCount64()));
    std::error_code createError;
    const bool created = std::filesystem::create_directory(sandbox, createError);
    check(created && !createError, "unique image-pin sandbox is created");
    if (!created || createError) return;

    {
        const auto launchedImage = sandbox / L"Aris.exe";
        const auto displacedImage = sandbox / L"Aris.launched.exe";
        const auto replacementImage = sandbox / L"replacement.exe";
        const bool copied =
            CopyFileW(fixture.c_str(), launchedImage.c_str(), TRUE) != FALSE;
        check(copied, "signed launch image is copied into the image-pin sandbox");

        const char replacementBytes[] = "not-the-signed-launched-image";
        const HANDLE replacement = CreateFileW(
            replacementImage.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        bool replacementWritten = replacement != INVALID_HANDLE_VALUE;
        if (replacementWritten) {
            DWORD written = 0U;
            replacementWritten =
                WriteFile(replacement, replacementBytes,
                          static_cast<DWORD>(sizeof(replacementBytes)), &written,
                          nullptr) != FALSE &&
                written == static_cast<DWORD>(sizeof(replacementBytes)) &&
                FlushFileBuffers(replacement) != FALSE;
            CloseHandle(replacement);
        }
        check(replacementWritten,
              "different replacement bytes are durably staged for the swap");

        BootstrapExecutableExpectation expected;
        DWORD hashError = ERROR_SUCCESS;
        const bool hashed = copied &&
            signedExpectation(launchedImage, expected, hashError);
        if (!hashed) {
            std::cerr << "image-pin signed expectation error=" << hashError
                      << '\n';
        }
        check(hashed, "signed size and SHA-256 are captured before launch");

        auto process = CreateNativeBootstrapProcessPort();
        check(process != nullptr, "native process port exists for image pinning");
        if (replacementWritten && hashed && process != nullptr) {
            const auto launched = process->LaunchSuspended(
                launchedImage.u8string(), sandbox.u8string(), {});
            check(launched.ok() && launched.process.valid(),
                  "image-pin child starts suspended");
            if (launched.ok()) {
                auto child = launched.process;
                HANDLE observer = nullptr;
                check(DuplicateHandle(
                          GetCurrentProcess(),
                          reinterpret_cast<HANDLE>(child.waitToken),
                          GetCurrentProcess(), &observer, SYNCHRONIZE, FALSE,
                          0U) != FALSE,
                      "image-pin test retains an exact cleanup wait handle");

                SetLastError(ERROR_SUCCESS);
                const bool renamed = MoveFileExW(
                    launchedImage.c_str(), displacedImage.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                const DWORD renameError = renamed ? ERROR_SUCCESS : GetLastError();
                bool replaced = false;
                DWORD replaceError = ERROR_SUCCESS;
                if (renamed) {
                    replaced = MoveFileExW(
                        replacementImage.c_str(), launchedImage.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                    replaceError = replaced ? ERROR_SUCCESS : GetLastError();
                }

                const auto validated =
                    process->ValidateOwnedExecutable(child, expected);
                if (renamed || !validated.ok()) {
                    std::cerr << "image-pin rename=" << renamed
                              << " rename-error=" << renameError
                              << " replace=" << replaced
                              << " replace-error=" << replaceError
                              << " validation-status="
                              << static_cast<int>(validated.status)
                              << " validation-native=" << validated.nativeError
                              << '\n';
                }
                check(!renamed && validated.ok(),
                      "LaunchSuspended pins the launched file until signed validation");

                process->Close(child);
                const DWORD wait = observer == nullptr
                    ? WAIT_FAILED
                    : WaitForSingleObject(observer, 5000U);
                if (observer != nullptr) CloseHandle(observer);
                check(wait == WAIT_OBJECT_0,
                      "image-pin child exits before sandbox cleanup");
            }
        }
        process.reset();
    }
    removeSandboxExactly(sandbox);
}

void test_prepare_owned_for_commit_keeps_rollback_containment_armed() {
    auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "native process port and sleep fixture exist for ownership cut");
    if (process == nullptr || fixture.empty()) return;

    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"5000") != FALSE,
          "ownership-cut fixture extends the child lifetime");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(
        SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "ownership-cut child starts suspended");
    if (!launched.ok()) return;
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "ownership-cut child resumes before health/commit handoff");

    HANDLE observer = nullptr;
    const HANDLE childHandle = reinterpret_cast<HANDLE>(child.waitToken);
    check(DuplicateHandle(GetCurrentProcess(), childHandle,
                          GetCurrentProcess(), &observer, SYNCHRONIZE, FALSE,
                          0U) != FALSE,
          "test retains an independent wait observation");
    if (observer == nullptr) {
        process->TerminateOwned(child, 0xE006U);
        return;
    }
    check(process->PrepareOwnedForCommit(child).ok(),
          "native port validates ownership before commit publication");

    // Simulate a Bootstrap crash/failure after health acceptance but before
    // durable CommitJournal. Closing the port must still kill the job; only
    // ReleaseOwned after commit may disarm containment.
    process.reset();
    const DWORD contained = WaitForSingleObject(observer, 2000U);
    check(contained == WAIT_OBJECT_0,
          "pre-commit teardown terminates the still-owned child job");
    if (contained != WAIT_OBJECT_0) {
        static_cast<void>(TerminateProcess(observer, 0xE006U));
        static_cast<void>(WaitForSingleObject(observer, 5000U));
    }
    CloseHandle(observer);
}

void test_release_owned_rejects_a_child_that_exited_after_health() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "native process port and sleep fixture exist for release race");
    if (process == nullptr || fixture.empty()) return;

    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    check(launched.ok() && launched.process.valid(),
          "release-race child starts suspended");
    if (!launched.ok()) return;
    auto child = launched.process;
    check(process->ResumeOwned(child).ok() &&
              process->PrepareOwnedForCommit(child).ok(),
          "release-race child is resumed and prepared while alive");
    const HANDLE exactChild = reinterpret_cast<HANDLE>(child.waitToken);
    check(WaitForSingleObject(exactChild, 5000U) == WAIT_OBJECT_0,
          "release-race child exits after health preparation");

    const BootstrapProcessResult released = process->ReleaseOwned(child);
    check(released.status == BootstrapProcessStatus::IdentityMismatch &&
              released.nativeError == ERROR_PROCESS_ABORTED && !child.valid(),
          "ReleaseOwned rejects an already-exited child instead of reporting success");
}

void test_rebound_running_child_is_bound_to_expected_image_content() {
    auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "native process port and sleep fixture exist for rebound validation");
    if (process == nullptr || fixture.empty()) return;

    BootstrapExecutableExpectation expected;
    DWORD hashError = ERROR_SUCCESS;
    check(signedExpectation(fixture, expected, hashError),
          "rebound child expectation captures exact signed image content");
    if (!expected.valid()) return;

    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"5000") != FALSE,
          "rebound child fixture receives a bounded long lifetime");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "rebound child starts suspended with exact ownership");
    if (!launched.ok()) return;

    auto child = launched.process;
    const BootstrapProcessIdentity identity = child.identity;
    HANDLE cleanup = nullptr;
    check(DuplicateHandle(GetCurrentProcess(),
                          reinterpret_cast<HANDLE>(child.waitToken),
                          GetCurrentProcess(), &cleanup,
                          PROCESS_TERMINATE | SYNCHRONIZE, FALSE, 0U) != FALSE,
          "rebound child retains an independent exact cleanup handle");
    check(process->ValidateOwnedExecutable(child, expected).ok(),
          "new child is signed-image-bound before its first resume");
    check(process->ResumeOwned(child).ok(),
          "rebound child resumes before the simulated restart cut");
    check(process->ReleaseOwned(child).ok(),
          "rebound child ownership is released before the checkpoint cut");
    process.reset();

    auto recovery = CreateNativeBootstrapProcessPort();
    check(recovery != nullptr,
          "fresh native process port exists after simulated Bootstrap restart");
    BootstrapProcessResult rebound;
    if (recovery != nullptr) {
        rebound = recovery->RebindOwnedChild(identity);
        check(rebound.ok() && rebound.process.valid(),
              "fresh process port rebinds the exact running child identity");
        if (rebound.ok()) {
            check(recovery->ValidateOwnedExecutable(rebound.process, expected).ok(),
                  "fresh process port validates the exact rebound image bytes");
            check(recovery->TerminateOwned(rebound.process, 0xE007U).ok(),
                  "rebound child remains owned for deterministic cleanup");
        }
    }

    if (cleanup != nullptr) {
        if (WaitForSingleObject(cleanup, 0U) == WAIT_TIMEOUT) {
            static_cast<void>(TerminateProcess(cleanup, 0xE007U));
        }
        static_cast<void>(WaitForSingleObject(cleanup, 5000U));
        CloseHandle(cleanup);
    }
}

void test_duplex_health_is_one_shot_and_child_bound() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto health = CreateNativeBootstrapHealthPort();
    const auto fixture = sibling(L"aris_bootstrap_health_fixture.exe");
    check(process != nullptr && health != nullptr && !fixture.empty(),
          "native health/process ports and fixture exist");
    if (process == nullptr || health == nullptr || fixture.empty()) return;
    BootstrapLaunchCapability capability;
    const std::string nonce(64U, 'c');
    check(health->Prepare(nonce, "1.2.3", "build-health", capability).ok() &&
              capability.inheritedStdin != 0U && capability.inheritedStdout != 0U,
          "health preparation creates an allowlisted duplex capability");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), capability);
    health->ReleaseLaunchCapability();
    check(launched.ok() && launched.process.valid(),
          "health child starts suspended with inherited handles");
    if (!launched.ok()) return;
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "health child resumes only after identity checkpoint seam");
    const auto acknowledged = health->Await(
        child, nonce, "1.2.3", "build-health", 5000U);
    check(acknowledged.status == BootstrapHealthStatus::Accepted &&
              acknowledged.acknowledgement.processId == child.identity.processId,
          "health acknowledgement binds to the exact launched child");
    const auto replay = health->Await(
        child, nonce, "1.2.3", "build-health", 1U);
    check(replay.status == BootstrapHealthStatus::ReplayRejected,
          "health capability is one-shot and rejects replay");
    const auto released = process->ReleaseOwned(child);
    if (!released.ok()) {
        std::cerr << "mismatch release status="
                  << static_cast<int>(released.status)
                  << " native=" << released.nativeError << '\n';
    }
    check(released.ok(),
          "successful child release leaves the healthy process running");
    health->Close();
}

void test_health_acknowledgement_is_exactly_challenge_bound() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto health = CreateNativeBootstrapHealthPort();
    const auto fixture = sibling(L"aris_bootstrap_health_fixture.exe");
    check(process != nullptr && health != nullptr && !fixture.empty(),
          "native health/process ports and health fixture exist for mismatch test");
    if (process == nullptr || health == nullptr || fixture.empty()) return;

    BootstrapLaunchCapability capability;
    const std::string nonce(64U, 'c');
    check(health->Prepare(nonce, "1.2.3", "build-health-bad", capability).ok(),
          "mismatch health challenge is prepared");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), capability);
    health->ReleaseLaunchCapability();
    check(launched.ok() && launched.process.valid(),
          "mismatch health child starts suspended");
    if (!launched.ok()) {
        health->Close();
        return;
    }
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "mismatch health child resumes after identity checkpoint");
    const auto rejected = health->Await(
        child, nonce, "1.2.3", "build-health-bad", 5000U);
    check(rejected.status == BootstrapHealthStatus::ChannelRejected,
          "native health port rejects mismatched token/readiness acknowledgement");
    const auto terminated = process->TerminateOwned(child, 0xE003U);
    if (!terminated.ok()) {
        std::cerr << "mismatch terminate status="
                  << static_cast<int>(terminated.status)
                  << " native=" << terminated.nativeError << '\n';
    }
    check(terminated.ok(),
          "mismatch health child remains owned for cleanup");
    health->Close();
}

void test_health_acknowledgement_waits_for_one_complete_frame() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto health = CreateNativeBootstrapHealthPort();
    const auto fixture = sibling(L"aris_bootstrap_health_fixture.exe");
    check(process != nullptr && health != nullptr && !fixture.empty(),
          "native health/process ports and fragmented fixture exist");
    if (process == nullptr || health == nullptr || fixture.empty()) return;

    BootstrapLaunchCapability capability;
    const std::string nonce(64U, 'c');
    check(health->Prepare(nonce, "1.2.3", "build-health-fragmented",
                          capability).ok(),
          "fragmented acknowledgement challenge is prepared");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), capability);
    health->ReleaseLaunchCapability();
    check(launched.ok() && launched.process.valid(),
          "fragmented acknowledgement child starts suspended");
    if (!launched.ok()) {
        health->Close();
        return;
    }
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "fragmented acknowledgement child resumes");
    const auto acknowledged = health->Await(
        child, nonce, "1.2.3", "build-health-fragmented", 5000U);
    check(acknowledged.status == BootstrapHealthStatus::Accepted,
          "health port waits for the complete bounded frame across partial pipe reads");
    health->Close();
    check(process->WaitForExit(child, 5000U).ok(),
          "fragmented health child exits after the channel ownership handoff");
    process->Close(child);
}

void test_health_acknowledgement_survives_child_stderr_diagnostic() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto health = CreateNativeBootstrapHealthPort();
    const auto fixture = sibling(L"aris_bootstrap_health_fixture.exe");
    check(process != nullptr && health != nullptr && !fixture.empty(),
          "stderr health fixture and native ports exist");
    if (process == nullptr || health == nullptr || fixture.empty()) return;
    BootstrapLaunchCapability capability;
    const std::string nonce(64U, 'c');
    check(health->Prepare(nonce, "1.2.3", "build-health-stderr", capability).ok(),
          "stderr health challenge is prepared");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), capability);
    health->ReleaseLaunchCapability();
    check(launched.ok() && launched.process.valid(),
          "stderr health child starts suspended");
    if (!launched.ok()) { health->Close(); return; }
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(), "stderr health child resumes");
    const auto acknowledged = health->Await(
        child, nonce, "1.2.3", "build-health-stderr", 5000U);
    check(acknowledged.status == BootstrapHealthStatus::Accepted,
          "old health protocol accepts new child despite unrelated stderr");
    if (acknowledged.ok()) process->ReleaseOwned(child);
    else process->TerminateOwned(child, 0xE020U);
    health->Close();
}

void test_health_channel_requires_owned_child_wait_capability() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto health = CreateNativeBootstrapHealthPort();
    const auto fixture = sibling(L"aris_bootstrap_health_fixture.exe");
    check(process != nullptr && health != nullptr && !fixture.empty(),
          "native health/process ports and health fixture exist for ownership test");
    if (process == nullptr || health == nullptr || fixture.empty()) return;

    BootstrapLaunchCapability capability;
    const std::string nonce(64U, 'c');
    check(health->Prepare(nonce, "1.2.3", "build-health", capability).ok(),
          "owned-child health challenge is prepared");
    const auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), capability);
    health->ReleaseLaunchCapability();
    check(launched.ok() && launched.process.valid(),
          "owned-child health fixture starts suspended");
    if (!launched.ok()) {
        health->Close();
        return;
    }
    auto child = launched.process;
    check(process->ResumeOwned(child).ok(),
          "owned-child health fixture resumes");
    auto forged = child;
    forged.waitToken = 0U;
    const auto rejected = health->Await(
        forged, nonce, "1.2.3", "build-health", 5000U);
    check(rejected.status == BootstrapHealthStatus::ChannelRejected,
          "health channel rejects a child value without the retained wait capability");
    check(process->TerminateOwned(child, 0xE004U).ok(),
          "owned child remains available for deterministic cleanup");
    health->Close();
}

void test_authenticated_channel_allows_unsigned_parent_to_reach_payload_validation() {
    const std::wstring pipeName =
        L"\\\\.\\pipe\\aris-bootstrap-untrusted-parent-" +
        std::to_wstring(GetCurrentProcessId());
    const HANDLE server = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1U, 4096U, 4096U, 1000U, nullptr);
    check(server != INVALID_HANDLE_VALUE,
          "unsigned-parent fixture creates its one-shot pipe");
    if (server == INVALID_HANDLE_VALUE) return;
    const HANDLE client = CreateFileW(
        pipeName.c_str(), GENERIC_READ, 0U, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(client != INVALID_HANDLE_VALUE,
          "unsigned-parent fixture opens its inherited client handle");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    static_cast<void>(ConnectNamedPipe(server, nullptr));

    HANDLE parent = nullptr;
    check(DuplicateHandle(
              GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
              &parent, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
              FALSE, 0U) != FALSE,
          "unsigned-parent fixture retains a real restricted process handle");
    if (parent == nullptr) {
        CloseHandle(client);
        CloseHandle(server);
        return;
    }
    const auto rejected = ReadAuthenticatedBootstrapChannel(
        reinterpret_cast<std::uintptr_t>(client),
        reinterpret_cast<std::uintptr_t>(parent), 1U);
    check(rejected.status == BootstrapChannelStatus::TimedOut,
          "an explicitly unsigned same-user parent reaches bounded payload validation while later package and target binding remain mandatory");
    CloseHandle(parent);
    CloseHandle(client);
    CloseHandle(server);
}

void test_channel_requires_live_exact_parent_identity() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "exited-parent fixture has a native process port and helper");
    if (process == nullptr || fixture.empty()) return;
    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"50") != FALSE,
          "exited-parent fixture configures a bounded child lifetime");
    auto launched = process->LaunchSuspended(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(
        SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "exited-parent fixture launches an exact child capability");
    if (!launched.ok()) return;
    const BootstrapProcessIdentity expected = launched.process.identity;
    BootstrapProcessIdentity observed;
    check(CaptureBootstrapParentIdentityForTesting(
              launched.process.waitToken, observed) && observed == expected,
          "a live retained parent handle binds PID, creation time, path, volume and FileId");
    // Keep the child suspended until its live identity is checked, independent
    // of scheduler delays on a busy test host.
    check(process->ResumeOwned(launched.process).ok(),
          "the identity-checked parent resumes before the exit check");
    const auto waited = process->WaitForExit(launched.process, 5000U);
    check(waited.ok(),
          "exited-parent fixture reaches a signaled retained handle");
    const auto rejected = ReadAuthenticatedBootstrapChannel(
        0U, launched.process.waitToken, 1U);
    check(rejected.status == BootstrapChannelStatus::ParentIdentityRejected,
          "an exited parent cannot start a new authenticated handoff");
    process->Close(launched.process);
}

void test_authenticated_channel_requires_an_exact_parent_process_capability() {
    const auto rejected = ReadAuthenticatedBootstrapChannel(0U, 0U, 1U);
    check(rejected.status == BootstrapChannelStatus::ParentIdentityRejected,
          "authenticated handoff rejects a missing inherited parent process capability");
}

void test_authenticated_channel_rejects_pipe_parent_substitution() {
    const auto process = CreateNativeBootstrapProcessPort();
    const auto fixture = sibling(L"aris_bootstrap_sleep_fixture.exe");
    check(process != nullptr && !fixture.empty(),
          "parent-substitution fixture has a distinct live process");
    if (process == nullptr || fixture.empty()) return;
    check(SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", L"5000") != FALSE,
          "parent-substitution fixture extends the distinct process lifetime");
    auto launched = process->Launch(
        fixture.u8string(), fixture.parent_path().u8string(), {});
    static_cast<void>(
        SetEnvironmentVariableW(L"ARIS_BOOTSTRAP_SLEEP_MS", nullptr));
    check(launched.ok() && launched.process.valid(),
          "parent-substitution fixture launches its distinct process");
    if (!launched.ok()) return;

    const std::wstring pipeName =
        L"\\\\.\\pipe\\aris-bootstrap-parent-substitution-" +
        std::to_wstring(GetCurrentProcessId());
    const HANDLE server = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1U, 4096U, 4096U, 1000U, nullptr);
    const HANDLE client = server == INVALID_HANDLE_VALUE
        ? INVALID_HANDLE_VALUE
        : CreateFileW(pipeName.c_str(), GENERIC_READ, 0U, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE,
          "parent-substitution fixture opens its one-shot pipe");
    if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE) {
        static_cast<void>(ConnectNamedPipe(server, nullptr));
        const auto rejected = ReadAuthenticatedBootstrapChannel(
            reinterpret_cast<std::uintptr_t>(client),
            launched.process.waitToken, 1U);
        check(rejected.status == BootstrapChannelStatus::ParentIdentityRejected,
              "a valid pipe server PID cannot be paired with another live process capability");
    }
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    const HANDLE launchedHandle =
        reinterpret_cast<HANDLE>(launched.process.waitToken);
    static_cast<void>(TerminateProcess(launchedHandle, 0xE019U));
    static_cast<void>(process->WaitForExit(launched.process, 5000U));
    process->Close(launched.process);
}
#endif

}  // namespace

int main() {
#if defined(_WIN32)
    test_exact_old_process_wait_and_substitution();
    test_open_expected_pins_old_image_snapshot_through_wait();
    test_old_snapshot_rejects_byte_identical_fileid_substitution_before_resume();
    test_terminal_process_rebind_maps_signalled_handle_to_absence();
    test_released_child_rebind_maps_terminal_job_to_absence();
    test_suspended_child_is_bound_to_expected_image_content();
    test_suspended_launch_is_job_owned_at_process_creation();
    test_terminate_owned_drains_the_entire_child_job();
    test_terminate_owned_drains_descendants_after_root_already_exited();
    test_launch_suspended_pins_image_until_signed_validation();
    test_prepare_owned_for_commit_keeps_rollback_containment_armed();
    test_release_owned_rejects_a_child_that_exited_after_health();
    test_rebound_running_child_is_bound_to_expected_image_content();
    test_duplex_health_is_one_shot_and_child_bound();
    test_health_acknowledgement_is_exactly_challenge_bound();
    test_health_acknowledgement_waits_for_one_complete_frame();
    test_health_acknowledgement_survives_child_stderr_diagnostic();
    test_health_channel_requires_owned_child_wait_capability();
    test_authenticated_channel_allows_unsigned_parent_to_reach_payload_validation();
    test_channel_requires_live_exact_parent_identity();
    test_authenticated_channel_requires_an_exact_parent_process_capability();
    test_authenticated_channel_rejects_pipe_parent_substitution();
#endif
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "HarkenSearchBootstrap native runtime tests passed\n";
    return 0;
}
