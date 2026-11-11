#import <AppKit/AppKit.h>

#include "platform/executable_file_dialog.h"
#include "platform/text_encoding.h"

namespace Platform::ExecutableDialogs {
namespace Internal {

// Kept separate from the panel so an isolated test can exercise the exact
// path returned to the existing DAW settings UI without controlling a mouse.
std::string ResolveExecutableSelectionUtf8(const std::string& selectedPath) {
    if (selectedPath.empty() || selectedPath.find('\0') != std::string::npos) return {};
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:selectedPath.c_str()];
        if (!path) return {};
        NSFileManager* files = [NSFileManager defaultManager];
        BOOL directory = NO;
        if (![files fileExistsAtPath:path isDirectory:&directory]) return {};

        if (directory) {
            if (![[path.pathExtension lowercaseString] isEqualToString:@"app"]) return {};
            NSBundle* bundle = [NSBundle bundleWithPath:path];
            NSURL* executable = bundle.executableURL;
            if (!executable.isFileURL) return {};
            // Never accept an executable outside the selected application.
            NSString* actualBundle = [path stringByResolvingSymlinksInPath];
            NSString* actualExecutable = [executable.path stringByResolvingSymlinksInPath];
            NSString* bundlePrefix = [actualBundle stringByAppendingString:@"/"];
            if (![actualExecutable hasPrefix:bundlePrefix]) return {};
            path = executable.path;
            if (![files fileExistsAtPath:path isDirectory:&directory] || directory) return {};
        }
        if (![files isExecutableFileAtPath:path]) return {};
        const char* utf8 = path.UTF8String;
        return utf8 ? std::string(utf8) : std::string{};
    }
}

} // namespace Internal

std::string OpenExecutableFileDialogUtf8(
    void* ownerWindow,
    const std::wstring& title,
    const std::wstring& initialPath) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.treatsFilePackagesAsDirectories = NO;
        panel.allowsMultipleSelection = NO;
        // An executable can have any extension (or none). Keep all files
        // selectable and validate the result before changing the setting.

        const std::string titleUtf8 = TextEncoding::WideToUtf8Strict(title);
        if (!titleUtf8.empty()) panel.title = [NSString stringWithUTF8String:titleUtf8.c_str()];

        const std::string initialUtf8 = TextEncoding::WideToUtf8Strict(initialPath);
        if (!initialUtf8.empty()) {
            NSString* selected = [NSString stringWithUTF8String:initialUtf8.c_str()];
            if (selected) {
                NSString* directory = [selected stringByDeletingLastPathComponent];
                for (NSString* parent = selected; parent.length > 1;) {
                    if ([[parent.pathExtension lowercaseString] isEqualToString:@"app"]) {
                        directory = [parent stringByDeletingLastPathComponent];
                        break;
                    }
                    NSString* next = [parent stringByDeletingLastPathComponent];
                    if ([next isEqualToString:parent]) break;
                    parent = next;
                }
                BOOL isDirectory = NO;
                if ([[NSFileManager defaultManager] fileExistsAtPath:directory isDirectory:&isDirectory] && isDirectory) {
                    panel.directoryURL = [NSURL fileURLWithPath:directory isDirectory:YES];
                }
            }
        }

        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* selectedPath = panel.URL.path.UTF8String;
        return selectedPath ? Internal::ResolveExecutableSelectionUtf8(selectedPath) : std::string{};
    }
}

} // namespace Platform::ExecutableDialogs
