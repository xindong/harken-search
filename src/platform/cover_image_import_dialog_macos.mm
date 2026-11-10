#import <AppKit/AppKit.h>
#include "platform/cover_image_import_dialog.h"
#include "Localization.h"

namespace Platform::CoverDialogs {

std::string OpenCoverImageImportFileDialogUtf8(void* ownerWindow) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = [NSString stringWithUTF8String:L(Dlg_TitleChooseCover)];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedFileTypes = @[@"jpg", @"jpeg", @"png"];
        panel.allowsOtherFileTypes = YES;
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::CoverDialogs
