#ifdef __APPLE__

#import <AppKit/AppKit.h>
#include "platform/reaper_file_list_dialog.h"

namespace Platform::ReaperDialogs {

std::string OpenReaperFileListDialogUtf8(void* ownerWindow) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedFileTypes = @[@"ReaperFileList"];
        panel.allowsOtherFileTypes = YES;
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::ReaperDialogs

#endif // __APPLE__
