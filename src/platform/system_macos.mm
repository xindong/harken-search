#ifdef __APPLE__

#include "system.h"
#include "harken_user_data_layout.h"
#include "text_encoding.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <pthread/qos.h>
#include <thread>
#include <unistd.h>

namespace Platform {

std::wstring LegacyHardwareFingerprint() { return L"NON_WINDOWS_PLATFORM"; }
bool ProtectUserData(std::string_view, std::string& protectedBytes) {
    protectedBytes.clear();
    return false;
}
bool UnprotectUserData(std::string_view, std::string& plaintext) {
    plaintext.clear();
    return false;
}
bool StoreGenericCredential(const std::string&, std::string_view) { return false; }
bool LoadGenericCredential(const std::string&, std::string& value) {
    value.clear();
    return false;
}
bool DeleteGenericCredential(const std::string&) { return true; }

bool IsSystemDarkMode() {
    NSString* appearance = [[NSUserDefaults standardUserDefaults]
        stringForKey:@"AppleInterfaceStyle"];
    return [appearance isEqualToString:@"Dark"];
}

void WriteDebuggerOutput(std::string_view) {}

bool CloseInternetHandle(void* handle) {
    (void)handle;
    return false;
}

ProcessArgumentResult ReadProcessArgument(uint32_t /*index*/) {
    return {};
}

void FreeComTaskMemory(void* handle) {
    (void)handle;
}

void* OpenInternetSession(const wchar_t* /*agent*/, uint32_t /*accessType*/) {
    return nullptr;
}

bool SetInternetOption(void* /*handle*/, uint32_t /*option*/, uint32_t /*value*/) {
    return false;
}

void* ConnectInternetHttp(void* /*session*/, const std::wstring& /*host*/, uint16_t /*port*/) {
    return nullptr;
}

void* OpenHttpRequest(void* /*connection*/,
                      const wchar_t* /*method*/,
                      const std::wstring& /*path*/,
                      uint32_t /*flags*/,
                      const wchar_t* /*acceptType*/) {
    return nullptr;
}

bool SendHttpRequest(void* /*request*/,
                     const std::wstring& /*headers*/,
                     const void* /*body*/,
                     uint32_t /*bodyBytes*/) {
    return false;
}

bool QueryHttpInfoNumber(void* /*request*/, uint32_t /*infoLevel*/, uint32_t& value) {
    value = 0;
    return false;
}

void* OpenPreconfiguredInternetSession(const wchar_t* /*agent*/) {
    return nullptr;
}

bool SetInternetTimeouts(void* /*handle*/, uint32_t /*timeoutMs*/) {
    return false;
}

void* ConnectInternetHttps(void* /*session*/, const std::wstring& /*host*/) {
    return nullptr;
}

void* OpenSecureJsonPostRequest(void* /*connection*/, const std::wstring& /*path*/) {
    return nullptr;
}

void* OpenJsonPostRequest(void* /*connection*/, const std::wstring& /*path*/, bool /*secure*/) {
    return nullptr;
}

bool QueryHttpStatusCode(void* /*request*/, uint32_t& statusCode) {
    statusCode = 0;
    return false;
}

void* OpenInternetUrl(void* /*session*/, const std::wstring& /*url*/, uint32_t /*flags*/) {
    return nullptr;
}

bool ReadInternetFile(void* /*request*/, void* /*buffer*/, uint32_t /*bufferBytes*/, uint32_t& bytesRead) {
    bytesRead = 0;
    return false;
}

void* OpenExclusiveWriteFile(const std::string& /*pathUtf8*/) {
    return nullptr;
}

bool WriteFileBytes(void* /*fileHandle*/, const void* /*bytes*/, uint32_t /*byteCount*/) {
    return false;
}

void CloseFileHandle(void* /*fileHandle*/) {
}

bool CanOpenExclusiveWritePath(const std::string& pathUtf8) {
    if (pathUtf8.empty() || pathUtf8.find('\0') != std::string::npos) return false;
    std::error_code error;
    const bool exists = std::filesystem::exists(std::filesystem::u8path(pathUtf8), error);
    return !error && !exists;
}

std::string GetMachineId() {
    io_service_t platformExpert = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    if (!platformExpert) return "unknown-machine";

    CFStringRef serialRef = (CFStringRef)IORegistryEntryCreateCFProperty(
        platformExpert, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0);
    IOObjectRelease(platformExpert);

    if (!serialRef) return "unknown-machine";

    char buf[256] = {};
    CFStringGetCString(serialRef, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(serialRef);
    return std::string(buf);
}

void OpenInFileManager(const std::string& pathUtf8) {
    @autoreleasepool {
        NSString* nsPath = [NSString stringWithUTF8String:pathUtf8.c_str()];
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        BOOL isDir = NO;
        if ([[NSFileManager defaultManager] fileExistsAtPath:nsPath isDirectory:&isDir] && !isDir) {
            [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        } else {
            [[NSWorkspace sharedWorkspace] openURL:url];
        }
    }
}

void OpenUrl(const std::string& url) {
    @autoreleasepool {
        NSString* nsUrl = [NSString stringWithUTF8String:url.c_str()];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:nsUrl]];
    }
}

int ShowMessageBox(void* /*ownerWindow*/,
                   const wchar_t* message,
                   const wchar_t* title,
                   unsigned int flags) {
    constexpr unsigned int kMbIconError = 0x00000010U;
    constexpr unsigned int kMbIconWarning = 0x00000030U;
    @autoreleasepool {
        NSString* nsMessage = [NSString stringWithUTF8String:
            TextEncoding::WideToUtf8Permissive(message ? std::wstring_view(message) : std::wstring_view{}).c_str()];
        NSString* nsTitle = [NSString stringWithUTF8String:
            TextEncoding::WideToUtf8Permissive(title ? std::wstring_view(title) : std::wstring_view{}).c_str()];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = nsTitle ?: @"";
        alert.informativeText = nsMessage ?: @"";
        if ((flags & kMbIconWarning) == kMbIconWarning) {
            alert.alertStyle = NSAlertStyleWarning;
        } else if ((flags & kMbIconError) == kMbIconError) {
            alert.alertStyle = NSAlertStyleCritical;
        } else {
            alert.alertStyle = NSAlertStyleInformational;
        }
        [alert addButtonWithTitle:@"OK"];
        const NSModalResponse response = [alert runModal];
        return response == NSAlertFirstButtonReturn ? 1 : 0;
    }
}

uint64_t MonotonicMilliseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t CurrentProcessId() {
    return static_cast<uint64_t>(getpid());
}

bool CurrentProcessIsElevated() {
    return false;
}

bool WaitForNativeThreadHandle(void* /*threadHandle*/, uint32_t /*timeoutMilliseconds*/) {
    return false;
}

bool AcquireNamedMutex(const wchar_t* /*name*/, void*& ownedHandle,
                       bool& alreadyExisted, bool& abandoned) {
    ownedHandle = nullptr;
    alreadyExisted = false;
    abandoned = false;
    return false;
}

void ReleaseNamedMutex(void* /*ownedHandle*/) {}

uint32_t GetModuleFileNameBuffer(void* /*moduleHandle*/, wchar_t* /*buffer*/, uint32_t /*capacity*/) {
    return 0;
}

uint32_t GetTempPathBuffer(wchar_t* /*buffer*/, uint32_t /*capacity*/) {
    return 0;
}

void* OpenCrashFile(const wchar_t* /*path*/, bool /*append*/) {
    return nullptr;
}

void OpenCrashReportFolder(const wchar_t* /*path*/) noexcept {
}

void* CurrentProcessHandle() {
    return nullptr;
}

bool WideToUtf8Buffer(const wchar_t* /*value*/, char* /*buffer*/, uint32_t /*capacity*/) {
    return false;
}

ProcessMemoryUsageResult QueryCurrentProcessMemoryUsage() {
    return {};
}

void CloseNativeHandle(void* /*handle*/) {}

uint32_t LastSystemError() {
    return 0;
}

void EnumerateTopLevelWindows(WindowEnumerationCallback, void*) {
}

// Borrowed handles below are restricted to this application's live windows.
// Never message an arbitrary pointer or enumerate other applications.
static NSWindow* OwnedWindow(void* handle) {
    if (!handle || ![NSThread isMainThread]) return nil;
    for (NSWindow* window in [NSApp windows]) {
        if ((__bridge void*)window == handle) return window;
    }
    return nil;
}

void RequestWindowClose(void* windowHandle) {
    NSWindow* window = OwnedWindow(windowHandle);
    if (window) [window performClose:nil];
}

int WindowClassName(void* windowHandle, char* buffer, int capacity) {
    NSWindow* window = OwnedWindow(windowHandle);
    if (!window || !buffer || capacity <= 0) return 0;
    const char* name = NSStringFromClass([window class]).UTF8String;
    const size_t length = std::min(strlen(name), static_cast<size_t>(capacity - 1));
    memcpy(buffer, name, length);
    buffer[length] = 0;
    return static_cast<int>(length);
}

uint64_t WindowProcessId(void* windowHandle) {
    return OwnedWindow(windowHandle) ? CurrentProcessId() : 0;
}

uint32_t WindowThreadId(void* /*windowHandle*/) {
    return 0; // No Win32 thread ID mapping on macOS.
}

bool WindowIsValid(void* windowHandle) { return OwnedWindow(windowHandle) != nil; }
bool WindowIsVisible(void* windowHandle) { return [OwnedWindow(windowHandle) isVisible]; }
bool WindowSetForeground(void* windowHandle) {
    NSWindow* window = OwnedWindow(windowHandle);
    if (!window) return false;
    [window makeKeyAndOrderFront:nil];
    return true;
}

bool WindowShow(void* windowHandle, int command) {
    NSWindow* window = OwnedWindow(windowHandle);
    if (!window) return false;
    if (command == 0) [window orderOut:nil];
    else if (command == 2 || command == 6 || command == 7) [window miniaturize:nil];
    else {
        if ([window isMiniaturized]) [window deminiaturize:nil];
        [window orderFront:nil];
    }
    return true;
}

RegisteredExecutablePathResult ReadRegisteredExecutablePath(
    RegisteredExecutableScope, const std::string&) {
    RegisteredExecutablePathResult result;
    result.status = RegisteredExecutablePathStatus::Unsupported;
    result.systemError = 0;
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
    if (hive != RegistryHive::CurrentUser && hive != RegistryHive::LocalMachine) {
        result.status = RegistryStringStatus::InvalidArgument;
        return result;
    }
    if (TextEncoding::Utf8ToWideStrict(subkeyUtf8).empty() ||
        (!valueNameUtf8.empty() &&
         TextEncoding::Utf8ToWideStrict(valueNameUtf8).empty())) {
        result.status = RegistryStringStatus::EncodingFailed;
        return result;
    }
    result.status = RegistryStringStatus::Unsupported;
    return result;
}

bool WriteRegistryString(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8,
    const std::string& valueUtf8) {
    if (subkeyUtf8.empty() || subkeyUtf8.find('\0') != std::string::npos ||
        valueNameUtf8.find('\0') != std::string::npos ||
        valueUtf8.find('\0') != std::string::npos) {
        return false;
    }
    if (hive != RegistryHive::CurrentUser && hive != RegistryHive::LocalMachine) {
        return false;
    }
    if (TextEncoding::Utf8ToWideStrict(subkeyUtf8).empty() ||
        (!valueNameUtf8.empty() && TextEncoding::Utf8ToWideStrict(valueNameUtf8).empty()) ||
        (!valueUtf8.empty() && TextEncoding::Utf8ToWideStrict(valueUtf8).empty())) {
        return false;
    }
    return false;
}

bool DeleteRegistryValue(
    RegistryHive hive,
    const std::string& subkeyUtf8,
    const std::string& valueNameUtf8) {
    if (subkeyUtf8.empty() || subkeyUtf8.find('\0') != std::string::npos ||
        valueNameUtf8.find('\0') != std::string::npos) {
        return false;
    }
    if (hive != RegistryHive::CurrentUser && hive != RegistryHive::LocalMachine) {
        return false;
    }
    if (TextEncoding::Utf8ToWideStrict(subkeyUtf8).empty() ||
        (!valueNameUtf8.empty() && TextEncoding::Utf8ToWideStrict(valueNameUtf8).empty())) {
        return false;
    }
    return false;
}

void NotifyShellAssociationsChanged() {
}
RegistryStringResult QueryEffectiveFileAssociation(const std::string& extensionUtf8) {
    RegistryStringResult result;
    if (extensionUtf8.empty() || extensionUtf8.find('\0') != std::string::npos) {
        result.status = RegistryStringStatus::InvalidArgument;
        return result;
    }
    if (TextEncoding::Utf8ToWideStrict(extensionUtf8).empty()) {
        result.status = RegistryStringStatus::EncodingFailed;
        return result;
    }
    result.status = RegistryStringStatus::Unsupported;
    return result;
}

bool LaunchDefaultAppsSettings() {
    return false;
}


EnvironmentStringResult ReadEnvironmentVariable(const std::string& nameUtf8) {
    EnvironmentStringResult result;
    if (nameUtf8.empty() || nameUtf8.find('\0') != std::string::npos) {
        result.status = EnvironmentStringStatus::InvalidArgument;
        return result;
    }
    if (TextEncoding::Utf8ToWideStrict(nameUtf8).empty()) {
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    const char* value = getenv(nameUtf8.c_str());
    if (!value) {
        result.status = EnvironmentStringStatus::NotFound;
        return result;
    }
    result.valueUtf8 = value;
    if (!result.valueUtf8.empty() &&
        TextEncoding::Utf8ToWideStrict(result.valueUtf8).empty()) {
        result.valueUtf8.clear();
        result.status = EnvironmentStringStatus::EncodingFailed;
        return result;
    }
    result.status = EnvironmentStringStatus::Success;
    return result;
}

EnvironmentStringResult ExpandEnvironmentVariables(const std::string&) {
    EnvironmentStringResult result;
    result.status = EnvironmentStringStatus::Unsupported;
    result.systemError = 0;
    return result;
}

void SleepMilliseconds(uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

ElevatedProcessWaitResult RunElevatedProcessAndWait(
    const ElevatedProcessWaitOptions& /*options*/) {
    ElevatedProcessWaitResult result;
    result.status = ElevatedProcessWaitStatus::Unsupported;
    return result;
}

void InitThreadCom() {
    // No COM on macOS
}

void UninitThreadCom() {
    // No COM on macOS
}

void SetThreadBackgroundPriority() {
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
}

void SetThreadBackgroundMode(bool /*enable*/) {
    // No-op on macOS.
}

std::string OpenFileDialog(void* /*ownerWindow*/, const std::string& title, const std::string& filter) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        if ([panel runModal] == NSModalResponseOK) {
            return std::string([[[panel URL] path] UTF8String]);
        }
    }
    return "";
}

std::string SaveFileDialog(void* /*ownerWindow*/, const std::string& title, const std::string& filter) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        if ([panel runModal] == NSModalResponseOK) {
            return std::string([[[panel URL] path] UTF8String]);
        }
    }
    return "";
}

std::string BrowseFolderDialog(void* /*ownerWindow*/, const std::string& title,
                               const std::string& initialPathUtf8) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        if (!initialPathUtf8.empty()) {
            NSString* initialPath = [NSString stringWithUTF8String:initialPathUtf8.c_str()];
            if (initialPath != nil) {
                [panel setDirectoryURL:[NSURL fileURLWithPath:initialPath]];
            }
        }
        if ([panel runModal] == NSModalResponseOK) {
            return std::string([[[panel URL] path] UTF8String]);
        }
    }
    return "";
}

std::string BrowseFolderDialogModern(void* ownerWindow, const std::string& title) {
    return BrowseFolderDialog(ownerWindow, title, "");
}

std::string GetAppDataDir() {
    const auto layout = HarkenUserData::EnsureLayout();
    if (layout.readOnly) return {};
    return HarkenUserData::WideToUtf8Path(layout.activeSettingsDir);
}

UserDataDirectoryResult GetRoamingAppDataDirectory() {
    UserDataDirectoryResult result;
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(
            NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if ([paths count] == 0) {
            result.status = UserDataDirectoryStatus::QueryFailed;
            return result;
        }
        NSString* appSupport = [paths objectAtIndex:0];
        const char* utf8 = appSupport ? [appSupport UTF8String] : nullptr;
        if (!utf8 || utf8[0] == '\0') {
            result.status = UserDataDirectoryStatus::EncodingFailed;
            return result;
        }
        result.pathUtf8 = utf8;
        result.status = UserDataDirectoryStatus::Success;
    }
    return result;
}

std::string GetExeDir() {
    @autoreleasepool {
        NSString* path = [[NSBundle mainBundle] executablePath];
        NSString* dir = [path stringByDeletingLastPathComponent];
        return std::string([dir UTF8String]);
    }
}

std::string GetTempDirectory() {
    @autoreleasepool {
        NSString* base = NSTemporaryDirectory();
        const char* utf8 = base ? [base UTF8String] : nullptr;
        return utf8 ? std::string(utf8) : std::string();
    }
}

std::string GetTempDragDirectory() {
    @autoreleasepool {
        NSString* base = nil;
        if (HarkenUserData::PreviewIsolationActive()) {
            const auto isolated = (std::filesystem::path(HarkenUserData::PreviewIsolationRootUtf8()) /
                                   "temp").string();
            if (!HarkenUserData::PreviewIsolationAllowsDataPathUtf8(isolated)) return {};
            base = [NSString stringWithUTF8String:isolated.c_str()];
        } else {
            base = NSTemporaryDirectory();
        }
        if (!base) return {};
        NSString* drag = [base stringByAppendingPathComponent:@"Auro_Drag"];
        if (![[NSFileManager defaultManager] createDirectoryAtPath:drag
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil]) return {};
        const char* utf8 = [drag UTF8String];
        return utf8 ? std::string(utf8) + "/" : std::string();
    }
}

} // namespace Platform

#endif // __APPLE__
