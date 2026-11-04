#import <AppKit/AppKit.h>
#include "platform/settings_export_dialog.h"

namespace Platform::SettingsDialogs {

std::string SaveSettingsExportFileDialogUtf8(void* ownerWindow) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = @"aris_settings_export.ini";
        panel.allowedFileTypes = @[@"ini"];
        panel.allowsOtherFileTypes = NO;
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::SettingsDialogs
