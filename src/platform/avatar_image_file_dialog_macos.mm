#import <AppKit/AppKit.h>
#include "platform/avatar_image_file_dialog.h"

namespace Platform::AvatarDialogs {

std::string OpenAvatarImageFileDialogUtf8(void* ownerWindow) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.allowedFileTypes = @[@"png", @"jpg", @"jpeg", @"bmp", @"tga"];
        panel.allowsOtherFileTypes = YES;
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::AvatarDialogs
