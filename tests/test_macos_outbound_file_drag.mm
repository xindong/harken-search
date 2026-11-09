#import <AppKit/AppKit.h>
#include "platform/macos_file_drag.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            require(argc == 2 && fs::path(argv[1]).is_absolute(), "isolated output root supplied");
            const fs::path root = fs::path(argv[1]) /
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            const fs::path fixture = root / u8"中文 素材";
            require(fs::create_directories(fixture), "fixture directory created");
            const fs::path first = fixture / u8"钟声 1.wav";
            const fs::path second = fixture / "tone 2.wav";
            std::ofstream(first, std::ios::binary) << "first sound";
            std::ofstream(second, std::ios::binary) << "second sound";

            std::vector<Platform::MacFileDrag::PreparedFile> prepared;
            std::string error;
            require(Platform::MacFileDrag::PrepareFileURLs(
                {first.string(), second.string(), first.string()}, prepared, error) &&
                prepared.size() == 2 && error.empty(), "Unicode and multiple files are deduplicated");
            NSMutableArray<NSURL*>* urls = [NSMutableArray array];
            for (size_t i = 0; i < prepared.size(); ++i) {
                NSString* value = [NSString stringWithUTF8String:prepared[i].fileUrl.c_str()];
                NSURL* url = [NSURL URLWithString:value];
                require(url.isFileURL && std::string(url.path.UTF8String) == prepared[i].absolutePath,
                    "native file URL decodes to the exact prepared file");
                [urls addObject:url];
                NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
                require(item != nil, "native dragging item accepts file URL writer");
#if !__has_feature(objc_arc)
                [item release];
#endif
            }
            NSPasteboard* pasteboard = [NSPasteboard pasteboardWithUniqueName];
            require([pasteboard writeObjects:urls], "file URLs can be published to macOS pasteboard");
            NSArray<NSURL*>* readBack = [pasteboard readObjectsForClasses:@[[NSURL class]]
                options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
            require(readBack.count == 2 && [readBack[0].path isEqualToString:urls[0].path] &&
                [readBack[1].path isEqualToString:urls[1].path],
                "another process can read both file URLs in order");

            require(!Platform::MacFileDrag::PrepareFileURLs(
                {first.string(), (fixture / "missing.wav").string()}, prepared, error) &&
                prepared.empty() && !error.empty(), "missing file rejects the entire batch");
            require(!Platform::MacFileDrag::PrepareFileURLs(
                {fixture.string()}, prepared, error) && prepared.empty(), "folder is not advertised as audio file");
            require(!Platform::MacFileDrag::PrepareFileURLs(
                {std::string("bad\0path", 8)}, prepared, error) && prepared.empty(),
                "embedded NUL cannot truncate a path");
            require(!Platform::MacFileDrag::PrepareFileURLs(
                {std::string(1, static_cast<char>(0xff))}, prepared, error) && prepared.empty(),
                "invalid UTF-8 is rejected");
            require(!Platform::MacFileDrag::PrepareFileURLs(
                std::vector<std::string>(201, first.string()), prepared, error) && prepared.empty(),
                "over-limit batch is rejected before native drag");
            require(!Platform::MacFileDrag::PrepareFileURLs({}, prepared, error) && prepared.empty(),
                "empty batch is rejected");
            require(fs::file_size(first) == 11 && fs::file_size(second) == 12,
                "source files remain unchanged");
            std::cout << "Evidence retained at " << root << '\n';
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "FAIL " << exception.what() << '\n';
            return 1;
        }
    }
}
