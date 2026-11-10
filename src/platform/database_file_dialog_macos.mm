#import <Cocoa/Cocoa.h>
#include "platform/database_file_dialog.h"
#include "platform/text_encoding.h"
#include "Localization.h"

namespace Platform::DatabaseDialogs {
std::string OpenDatabaseFileDialogWithInitialDirectoryUtf8(
    void* /*ownerWindow*/, const std::wstring& initialDirectory) {
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = [NSString stringWithUTF8String:L(Dlg_TitleOpenDatabase)];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedFileTypes = @[@"db"];
        // Match the existing Windows picker, which also offers All Files.
        panel.allowsOtherFileTypes = YES;
        const std::string directory = TextEncoding::WideToUtf8Strict(initialDirectory);
        if (!directory.empty()) {
            NSString* path = [NSString stringWithUTF8String:directory.c_str()];
            if (path) panel.directoryURL = [NSURL fileURLWithPath:path isDirectory:YES];
        }
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}
std::string OpenDatabaseFileDialogUtf8(void* ownerWindow) {
    return OpenDatabaseFileDialogWithInitialDirectoryUtf8(ownerWindow, {});
}
}
