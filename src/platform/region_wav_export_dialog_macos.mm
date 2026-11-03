#import <AppKit/AppKit.h>
#include "platform/region_wav_export_dialog.h"
#include "Localization.h"

namespace Platform::RegionDialogs {

std::string SaveRegionWavFileDialogUtf8(void* ownerWindow) {
    (void)ownerWindow;
    if (![NSThread isMainThread]) return {};
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = [NSString stringWithUTF8String:L(Dlg_TitleExportRegion)];
        panel.nameFieldStringValue = @"region_export.wav";
        panel.allowedFileTypes = @[@"wav"];
        panel.allowsOtherFileTypes = NO;
        if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL) return {};
        const char* path = panel.URL.path.UTF8String;
        return path ? std::string(path) : std::string{};
    }
}

} // namespace Platform::RegionDialogs
