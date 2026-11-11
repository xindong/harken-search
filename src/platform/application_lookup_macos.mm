#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
#include "application_lookup_macos.h"

namespace Platform {
MacApplicationLookupResult FindMacApplicationByBundleIdentifier(const std::string& bundleIdentifier) {
    MacApplicationLookupResult result;
    if (bundleIdentifier.empty() || bundleIdentifier.size() > 255) return result;
    for (const unsigned char c : bundleIdentifier)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return result;
    @autoreleasepool {
        NSString* identifier = [NSString stringWithUTF8String:bundleIdentifier.c_str()];
        if (!identifier) return result;
        CFErrorRef error = nullptr;
        CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier((CFStringRef)identifier, &error);
        if (error) {
            result.systemError = CFErrorGetCode(error);
            CFRelease(error);
        }
        if (!urls) {
            result.status = result.systemError == 0 || result.systemError == kLSApplicationNotFoundErr
                ? MacApplicationLookupStatus::NotFound : MacApplicationLookupStatus::QueryFailed;
            return result;
        }
        result.status = MacApplicationLookupStatus::NotFound;
        NSFileManager* files = [NSFileManager defaultManager];
        for (CFIndex i = 0; i < CFArrayGetCount(urls); ++i) {
            const auto value = CFArrayGetValueAtIndex(urls, i);
            if (!value || CFGetTypeID(value) != CFURLGetTypeID()) continue;
            NSURL* url = (NSURL*)value;
            if (!url.isFileURL) continue;
            NSBundle* bundle = [NSBundle bundleWithURL:url];
            if (![bundle.bundleIdentifier isEqualToString:identifier]) continue;
            NSURL* executable = bundle.executableURL;
            BOOL isDirectory = NO;
            if (!executable.isFileURL ||
                ![files fileExistsAtPath:url.path isDirectory:&isDirectory] || !isDirectory ||
                ![files fileExistsAtPath:executable.path isDirectory:&isDirectory] || isDirectory ||
                ![files isExecutableFileAtPath:executable.path]) continue;
            const char* root = url.path.UTF8String;
            const char* path = executable.path.UTF8String;
            if (!root || !path || root[0] != '/' || path[0] != '/') continue;
            result.bundlePathUtf8 = root;
            result.executablePathUtf8 = path;
            result.status = MacApplicationLookupStatus::Found;
            result.systemError = 0;
            break;
        }
        CFRelease(urls);
    }
    return result;
}
}
