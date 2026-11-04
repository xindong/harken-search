#import <AppKit/AppKit.h>

#include "platform/theme_json_dialogs.h"

namespace Platform::ThemeDialogs {

std::string OpenThemeJsonFileDialogUtf8(void* ownerWindow, const std::string& suggestedPathUtf8) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedFileTypes = @[@"json"];
        panel.allowsOtherFileTypes = YES;
        if (!suggestedPathUtf8.empty()) {
            NSString* suggested = [NSString stringWithUTF8String:suggestedPathUtf8.c_str()];
            if (suggested) {
                NSString* directory = [suggested stringByDeletingLastPathComponent];
                BOOL isDirectory = NO;
                if (directory.length > 0 &&
                    [[NSFileManager defaultManager] fileExistsAtPath:directory isDirectory:&isDirectory] && isDirectory) {
                    panel.directoryURL = [NSURL fileURLWithPath:directory isDirectory:YES];
                }
            }
        }
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

std::string SaveThemeJsonFileDialogUtf8(void* ownerWindow, const std::string& suggestedPathUtf8) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.allowedFileTypes = @[@"json"];
        panel.allowsOtherFileTypes = NO;
        panel.canCreateDirectories = YES;
        if (!suggestedPathUtf8.empty()) {
            NSString* suggested = [NSString stringWithUTF8String:suggestedPathUtf8.c_str()];
            if (suggested) {
                NSString* name = suggested.lastPathComponent;
                if (name.length > 0) panel.nameFieldStringValue = name;
                NSString* directory = [suggested stringByDeletingLastPathComponent];
                BOOL isDirectory = NO;
                if (directory.length > 0 &&
                    [[NSFileManager defaultManager] fileExistsAtPath:directory isDirectory:&isDirectory] && isDirectory) {
                    panel.directoryURL = [NSURL fileURLWithPath:directory isDirectory:YES];
                }
            }
        }
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::ThemeDialogs
